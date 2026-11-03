#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/bloom.h"
#include "testing/sst_builder.h"

namespace kv {
namespace {

using test::RawFilterBytes;
using test::RawFilterFor;

// ---- Golden vectors (scripts/bloom_reference.py) -------------------------------

struct HashVector {
    std::string key;
    std::uint64_t fnv;
    std::uint64_t h1;
    std::uint64_t h2;
};

const HashVector kHashVectors[] = {
    {std::string("", 0), 0xcbf29ce484222325ull, 0xf52a15e9a9b5e89bull,
     0xe9d327596b869821ull},
    {std::string("\x61", 1), 0xaf63dc4c8601ec8cull, 0x02c0bdbf481420f8ull,
     0x832be066bd43a3b9ull},
    {std::string("\x00", 1), 0xaf63bd4c8601b7dfull, 0x25fc6dd36ce04b20ull,
     0x4b32c4df3f01430bull},
    {std::string("\xff\x00", 2), 0x0a99a607b6f60beaull, 0x061c3e3f101d43ddull,
     0x39de09db2e9943e9ull},
    {std::string("\x61\x62\x63", 3), 0xe71fa2190541574bull,
     0x0dd490490804b508ull, 0xae806b2cdc26d551ull},
    {std::string("key00000001", 11), 0x239408f8a80e409full,
     0x560d3bb0c4eda889ull, 0x57e3d5702a05aaffull},
};

TEST(Bloom, Kvhash1MatchesIndependentVectors) {
    for (const HashVector& v : kHashVectors) {
        SCOPED_TRACE(v.key.size());
        const KvHash h = KvHash1(v.key);
        EXPECT_EQ(Fnv1a64(v.key), v.fnv);
        EXPECT_EQ(h.fnv, v.fnv);
        EXPECT_EQ(h.h1, v.h1);
        EXPECT_EQ(h.h2, v.h2);
        EXPECT_EQ(h.h2 & 1, 1u);
    }
    EXPECT_EQ(BloomProbes(1), 1u);
    EXPECT_EQ(BloomProbes(2), 1u);
    EXPECT_EQ(BloomProbes(5), 3u);
    EXPECT_EQ(BloomProbes(10), 7u);
    EXPECT_EQ(BloomProbes(16), 11u);
    EXPECT_EQ(BloomProbes(43), 30u);
    EXPECT_EQ(BloomProbes(64), 30u);
}

std::string Encode(const std::vector<std::string>& keys,
                   std::uint32_t bits_per_key = 10) {
    BloomFilterBuilder b(bits_per_key);
    for (const std::string& key : keys) {
        b.AddKey(key);
    }
    std::string out;
    b.Encode(&out);
    EXPECT_EQ(out.size(), b.EncodedSize(keys.size()));
    return out;
}

TEST(Bloom, EncodingMatchesIndependentFilterBytes) {
    // Three keys: B = max(8, ceil(30 / 8)) = 8 bytes, k = 7.
    constexpr char kSmall[] =
        "\x01\x00\x00\x00\x01\x00\x00\x00\x03\x00\x00\x00\x07\x00\x00\x00"
        "\x21\x40\x22\xb9\x1a\x0c\x82\x51\x93\xa1\xf7\x92";
    const std::vector<std::string> small = {"", "a", "b"};
    EXPECT_EQ(Encode(small), std::string(kSmall, sizeof(kSmall) - 1));

    // Twelve keys: B = ceil(120 / 8) = 15 bytes.
    constexpr char kTwelve[] =
        "\x01\x00\x00\x00\x01\x00\x00\x00\x0c\x00\x00\x00\x07\x00\x00\x00"
        "\x61\xa9\xdb\x76\xf2\x0d\xc7\x24\x90\x76\x98\x27\x8c\x51\x5f\x21"
        "\x4a\x6f\x85";
    std::vector<std::string> twelve = {std::string("\x00\xff", 2), "tomb"};
    for (int i = 0; i < 10; ++i) {
        twelve.push_back("k" + std::to_string(i));
    }
    EXPECT_EQ(Encode(twelve), std::string(kTwelve, sizeof(kTwelve) - 1));
    // The test-side builder agrees, independently of src/core/bloom.cc.
    EXPECT_EQ(RawFilterBytes(RawFilterFor(twelve)), Encode(twelve));
    EXPECT_EQ(RawFilterBytes(RawFilterFor(small)), Encode(small));
}

// ---- Behavior -------------------------------------------------------------------

TEST(Bloom, NoFalseNegativesAfterReload) {
    std::vector<std::string> keys = {"", std::string(1, '\0'),
                                     std::string("\x00\xff", 2),
                                     std::string(300, '\xff')};
    for (int i = 0; i < 2000; ++i) {
        std::string key = "key" + std::to_string(i * 7919);
        key.push_back(static_cast<char>(i & 0xff));
        keys.push_back(key);
    }
    for (const std::uint32_t bpk : {1u, 2u, 10u, 64u}) {
        SCOPED_TRACE(bpk);
        const std::string block = Encode(keys, bpk);
        BloomFilter f;
        ASSERT_TRUE(BloomFilter::Decode(block, 1u << 30, f).ok());
        EXPECT_EQ(f.key_count(), keys.size());
        for (const std::string& key : keys) {
            EXPECT_TRUE(f.MayContain(key)) << key.size();
        }
    }
}

// The false-positive rate follows from the actual m, n, and k:
// p = (1 - e^(-k n / m))^k. 10 bits per key gives about 0.8%. The check
// allows 1.5 p + 0.2% for 100,000 disjoint probes (deterministic keys),
// several standard deviations above p, and requires some false positives
// so that a filter answering "absent" for everything cannot pass.
TEST(Bloom, FalsePositiveRateMatchesTheParameters) {
    constexpr int kKeys = 10000;
    constexpr int kProbes = 100000;
    std::vector<std::string> keys;
    for (int i = 0; i < kKeys; ++i) {
        keys.push_back("present-" + std::to_string(i));
    }
    for (const std::uint32_t bpk : {5u, 10u, 16u}) {
        SCOPED_TRACE(bpk);
        const std::string block = Encode(keys, bpk);
        BloomFilter f;
        ASSERT_TRUE(BloomFilter::Decode(block, 1u << 30, f).ok());
        const double m = 8.0 * (block.size() - kFilterHeaderSize -
                                kFilterTrailerSize);
        const double k = BloomProbes(bpk);
        const double p = std::pow(1 - std::exp(-k * kKeys / m), k);
        int positives = 0;
        for (int i = 0; i < kProbes; ++i) {
            positives += f.MayContain("absent-" + std::to_string(i)) ? 1 : 0;
        }
        const double observed = static_cast<double>(positives) / kProbes;
        RecordProperty("fpr_bpk" + std::to_string(bpk),
                       std::to_string(observed));
        EXPECT_LE(observed, 1.5 * p + 0.002) << "expected about " << p;
        EXPECT_GT(positives, 0);
    }
}

}  // namespace
}  // namespace kv
