#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "core/bloom.h"
#include "core/posix_file.h"
#include "core/sst.h"
#include "testing/fault_syscalls.h"
#include "testing/sst_builder.h"
#include "testing/temp_dir.h"

namespace kv {
namespace {

namespace fs = std::filesystem;

using test::AssembleV2;
using test::kRawDelete;
using test::kRawPut;
using test::RawBlock;
using test::RawFilter;
using test::RawFilterBytes;
using test::RawFilterFor;
using test::RawFooter;
using test::RawFooterBytes;
using test::RawIndexBytes;
using test::RawSstEntry;
using test::RawSstV2;
using test::WriteBytes;

bool Contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

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

// ---- Malformed filter blocks inside an SST -----------------------------------------

// Values of 20 bytes make the data region large enough (D / 17 = 7) that a
// key count of 4 passes the header bound and must fail the entry count.
const std::string kValue(20, 'v');

class BloomSstTest : public ::testing::Test {
protected:
    fs::path Path() const { return tmp_.path() / "t.sst"; }

    Status Open(const std::string& bytes, test::FaultSyscalls* sys = nullptr) {
        EXPECT_TRUE(WriteBytes(Path(), bytes));
        std::unique_ptr<SstTable> table;
        PosixSyscalls& seam =
            sys != nullptr ? *sys : DefaultPosixSyscalls();
        return SstTable::Open(Path(), bytes.size(), seam, table);
    }

    void ExpectFails(const std::string& bytes, Code code,
                     std::string_view detail) {
        const Status s = Open(bytes);
        EXPECT_EQ(s.code, code) << s.ToString();
        EXPECT_TRUE(Contains(s.message, detail)) << s.message;
    }

    test::TempDir tmp_;
};

const std::vector<RawSstEntry> kEntries = {
    {"a", 1, kRawPut, kValue}, {"b", 2, kRawDelete, ""}, {"c", 3, kRawPut, kValue}};

std::string WithFilter(const RawFilter& filter) {
    const std::string data = RawBlock(kEntries);
    return AssembleV2(data, RawFilterBytes(filter),
                      RawIndexBytes({{"c", 0, static_cast<std::uint32_t>(
                                                  data.size())}}));
}

std::vector<std::string> EntryKeys() {
    return {"a", "b", "c"};
}

TEST_F(BloomSstTest, ValidFilterOpensAndTombstoneKeysAreInIt) {
    std::unique_ptr<SstTable> table;
    ASSERT_TRUE(WriteBytes(Path(), RawSstV2({kEntries})));
    ASSERT_TRUE(SstTable::Open(Path(), RawSstV2({kEntries}).size(),
                               DefaultPosixSyscalls(), table)
                    .ok());
    // The Delete's key is in the filter, so it shadows older SSTs.
    EXPECT_TRUE(table->summary().filter.MayContain("b"));
    LookupResult result;
    std::string value;
    ASSERT_TRUE(table->Get("b", result, &value).ok());
    EXPECT_EQ(result, LookupResult::kDeleted);
}

TEST_F(BloomSstTest, MalformedFiltersWithValidChecksumsFailOpen) {
    struct Case {
        const char* name;
        void (*mutate)(RawFilter&);
        Code code;
        const char* detail;
    };
    const Case cases[] = {
        {"filter version 2", [](RawFilter& f) { f.filter_version = 2; },
         Code::UnsupportedFormat, "unsupported filter version 2"},
        {"hash id 9", [](RawFilter& f) { f.hash_id = 9; },
         Code::UnsupportedFormat, "unsupported filter hash 9"},
        {"key count 0", [](RawFilter& f) { f.key_count = 0; },
         Code::Corruption, "key count 0"},
        {"key count above the entries", [](RawFilter& f) { f.key_count = 4; },
         Code::Corruption, "the filter counts 4 keys"},
        {"key count below the entries", [](RawFilter& f) { f.key_count = 2; },
         Code::Corruption, "the filter counts 2 keys"},
        {"key count above the data region", [](RawFilter& f) {
             f.key_count = 1000;
         },
         Code::Corruption, "key count 1000"},
        {"key count above the limit", [](RawFilter& f) {
             f.key_count = kMaxSstFilterKeys + 1;
         },
         Code::Corruption, "is out of range"},
        {"probes 0", [](RawFilter& f) { f.probes = 0; }, Code::Corruption,
         "probe count 0"},
        {"probes 31", [](RawFilter& f) { f.probes = 31; }, Code::Corruption,
         "probe count 31"},
        // 3 keys allow at most max(8, 24) bit bytes.
        {"too many bit bytes", [](RawFilter& f) { f.bits.resize(25, '\0'); },
         Code::Corruption, "bit bytes"},
        {"too few bit bytes", [](RawFilter& f) { f.bits.resize(7); },
         Code::Corruption, "filter length"},
        // Every bit clear: a valid shape that would deny stored keys.
        {"false negative", [](RawFilter& f) { f.bits.assign(8, '\0'); },
         Code::Corruption, "does not contain a stored key"},
        {"bad checksum", [](RawFilter& f) { f.corrupt_crc = true; },
         Code::Corruption, "filter checksum mismatch"},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.name);
        RawFilter f = RawFilterFor(EntryKeys());
        c.mutate(f);
        ExpectFails(WithFilter(f), c.code, c.detail);
    }
    // The unmodified filter opens.
    EXPECT_TRUE(Open(WithFilter(RawFilterFor(EntryKeys()))).ok());
}

TEST_F(BloomSstTest, EveryFilterBitFlipAndTruncationFailsClosed) {
    const std::string good = RawSstV2({kEntries});
    const std::string data = RawBlock(kEntries);
    const std::size_t begin = data.size();
    const std::size_t end = begin + RawFilterBytes(RawFilterFor(EntryKeys()))
                                        .size();
    for (std::size_t pos = begin; pos < end; ++pos) {
        for (const unsigned char bit : {0x01, 0x80}) {
            SCOPED_TRACE(pos);
            std::string bytes = good;
            bytes[pos] = static_cast<char>(bytes[pos] ^ bit);
            const Status s = Open(bytes);
            // The version and hash fields are identified first.
            const bool id_field = pos < begin + 8;
            EXPECT_EQ(s.code, id_field ? Code::UnsupportedFormat
                                       : Code::Corruption)
                << s.ToString();
        }
    }
    for (std::size_t cut = 0; cut < good.size(); ++cut) {
        std::unique_ptr<SstTable> table;
        ASSERT_TRUE(WriteBytes(Path(), good.substr(0, cut)));
        EXPECT_EQ(SstTable::Open(Path(), cut, DefaultPosixSyscalls(), table)
                      .code,
                  Code::Corruption)
            << cut;
    }
}

TEST_F(BloomSstTest, FooterFilterRegionsAreBoundedWithoutOverflow) {
    const std::string data = RawBlock(kEntries);
    const std::string filter = RawFilterBytes(RawFilterFor(EntryKeys()));
    const std::string index = RawIndexBytes(
        {{"c", 0, static_cast<std::uint32_t>(data.size())}});
    auto with = [&](void (*mutate)(RawFooter&)) {
        RawFooter f;
        f.version = 2;
        f.filter_offset = data.size();
        f.filter_length = filter.size();
        f.index_offset = data.size() + filter.size();
        f.index_length = index.size();
        mutate(f);
        return data + filter + index + RawFooterBytes(f);
    };
    ExpectFails(with([](RawFooter& f) { f.filter_length = 0; }),
                Code::Corruption, "filter length 0");
    ExpectFails(with([](RawFooter& f) { f.filter_length = 27; }),
                Code::Corruption, "filter length 27");
    ExpectFails(with([](RawFooter& f) { f.filter_length = ~0ull; }),
                Code::Corruption, "out of range");
    ExpectFails(with([](RawFooter& f) {
                    f.filter_length = kMaxSstFilterSize + 1;
                }),
                Code::Corruption, "out of range");
    ExpectFails(with([](RawFooter& f) { f.filter_offset = ~0ull - 4; }),
                Code::Corruption, "inconsistent");
    ExpectFails(with([](RawFooter& f) { f.filter_offset += 1; }),
                Code::Corruption, "inconsistent");
    ExpectFails(with([](RawFooter& f) { f.index_offset += 1; }),
                Code::Corruption, "inconsistent");
    // A version 1 footer must not describe a filter.
    ExpectFails(with([](RawFooter& f) { f.version = 1; }), Code::Corruption,
                "version 1 footer has a filter region");
}

// A sparse file whose footer declares the largest legal filter after a
// 25-byte data region: the 16-byte header shows that one key allows at
// most 8 bit bytes, so the bit array is never allocated or read.
TEST_F(BloomSstTest, ImpossibleFilterSizeIsRejectedBeforeTheBitArrayIsRead) {
    const std::string data = RawBlock({{"", 1, kRawPut, ""}});
    ASSERT_EQ(data.size(), kMinSstBlockSize);
    RawFilter header = RawFilterFor({""});
    const std::string index = RawIndexBytes(
        {{"", 0, static_cast<std::uint32_t>(data.size())}});
    RawFooter f;
    f.version = 2;
    f.filter_offset = data.size();
    f.filter_length = kMaxSstFilterSize;
    f.index_offset = f.filter_offset + f.filter_length;
    f.index_length = index.size();
    {
        std::ofstream out(Path(), std::ios::binary | std::ios::trunc);
        const std::string head =
            data + RawFilterBytes(header).substr(0, kFilterHeaderSize);
        out.write(head.data(), static_cast<std::streamsize>(head.size()));
    }
    std::error_code ec;
    fs::resize_file(Path(), f.index_offset, ec);
    ASSERT_FALSE(ec);
    {
        std::ofstream out(Path(), std::ios::binary | std::ios::app);
        const std::string tail = index + RawFooterBytes(f);
        out.write(tail.data(), static_cast<std::streamsize>(tail.size()));
    }
    test::FaultSyscalls sys;
    sys.armed = true;
    std::unique_ptr<SstTable> table;
    const Status s =
        SstTable::Open(Path(), fs::file_size(Path()), sys, table);
    EXPECT_EQ(s.code, Code::Corruption) << s.ToString();
    EXPECT_TRUE(Contains(s.message, "bit bytes for 1 keys")) << s.message;
    // The footer and the filter header only.
    EXPECT_EQ(sys.pread_bytes, kSstFooterSize + kFilterHeaderSize);
}

// ---- Lookups and the builder ----------------------------------------------------------

TEST_F(BloomSstTest, GetCountsRangeFilterAndBlockOutcomes) {
    std::vector<RawSstEntry> entries;
    std::vector<std::string> keys;
    for (int i = 0; i < 50; ++i) {
        const std::string key = "k" + std::to_string(1000 + 2 * i);
        entries.push_back({key, static_cast<std::uint64_t>(i + 1), kRawPut,
                           "v"});
        keys.push_back(key);
    }
    const std::string bytes = RawSstV2({entries});
    ASSERT_TRUE(WriteBytes(Path(), bytes));
    std::unique_ptr<SstTable> table;
    ASSERT_TRUE(
        SstTable::Open(Path(), bytes.size(), DefaultPosixSyscalls(), table)
            .ok());
    const BloomFilter& filter = table->summary().filter;

    // A missing key inside the key range that the filter rejects.
    std::string rejected;
    for (int i = 0; i < 49 && rejected.empty(); ++i) {
        const std::string key = "k" + std::to_string(1000 + 2 * i + 1);
        if (!filter.MayContain(key)) {
            rejected = key;
        }
    }
    ASSERT_FALSE(rejected.empty());

    LookupResult result;
    std::string value;
    SstReadStats stats;
    ASSERT_TRUE(table->Get("a", result, &value, &stats).ok());  // below range
    ASSERT_TRUE(table->Get("z", result, &value, &stats).ok());  // above range
    EXPECT_EQ(stats.range_rejections, 2u);
    ASSERT_TRUE(table->Get(rejected, result, &value, &stats).ok());
    EXPECT_EQ(result, LookupResult::kAbsent);
    EXPECT_EQ(stats.filter_rejections, 1u);
    EXPECT_EQ(stats.block_reads, 0u);
    // Bypassing the filter reads the block and still answers exactly.
    ASSERT_TRUE(table->Get(rejected, result, &value, &stats, false).ok());
    EXPECT_EQ(result, LookupResult::kAbsent);
    EXPECT_EQ(stats.block_reads, 1u);
    EXPECT_EQ(stats.block_bytes, table->summary().index[0].length);
    // A present key: the filter passes it and the block is read.
    ASSERT_TRUE(table->Get(keys[7], result, &value, &stats).ok());
    EXPECT_EQ(result, LookupResult::kFound);
    EXPECT_EQ(stats.block_reads, 2u);
    EXPECT_EQ(stats.filter_rejections, 1u);
}

TEST(BloomBuilderTest, FilterKeyLimitStopsTheBuilderBeforeWriting) {
    test::TempDir tmp;
    std::unique_ptr<WritableFile> file;
    ASSERT_TRUE(NewWritableFile(tmp.path() / "t.sst", file).ok());
    SstLimits limits;
    limits.max_filter_keys = 2;
    SstBuilder builder(*file, limits);
    ASSERT_TRUE(builder.Add("a", 1, ValueType::kPut, "1").ok());
    EXPECT_TRUE(builder.CanAdd("b", "2"));
    ASSERT_TRUE(builder.Add("b", 2, ValueType::kPut, "2").ok());
    EXPECT_FALSE(builder.CanAdd("c", "3"));
    const Status s = builder.Add("c", 3, ValueType::kPut, "3");
    EXPECT_EQ(s.code, Code::InvalidArgument) << s.ToString();
    EXPECT_TRUE(Contains(s.message, "2 filter keys")) << s.message;
    ASSERT_TRUE(builder.Finish().ok());
    EXPECT_EQ(builder.summary().filter.key_count(), 2u);

    // Without a filter there is no key limit, and the file is version 1.
    std::unique_ptr<WritableFile> plain;
    ASSERT_TRUE(NewWritableFile(tmp.path() / "v1.sst", plain).ok());
    SstBuilder v1(*plain, limits, 0);
    for (const char* key : {"a", "b", "c"}) {
        ASSERT_TRUE(v1.Add(key, 1, ValueType::kPut, "v").ok());
    }
    ASSERT_TRUE(v1.Finish().ok());
    EXPECT_EQ(v1.summary().version, kSstVersion1);
    EXPECT_TRUE(v1.summary().filter.empty());
}

}  // namespace
}  // namespace kv
