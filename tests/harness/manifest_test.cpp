#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>

#include "core/manifest.h"
#include "testing/fault_syscalls.h"
#include "testing/manifest_builder.h"
#include "testing/temp_dir.h"

namespace kv {
namespace {

namespace fs = std::filesystem;

using test::ManifestBody;
using test::ManifestBytes;
using test::RawManifest;
using test::RawManifestSst;

constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();

bool Contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

// Computed outside this code base (separate script, own CRC-32C): version
// 1, generation 3, next file ID 6, checkpoint 9, active WAL 5, and SSTs
// {2, 157 bytes, [1, 4]} and {4, 200 bytes, [6, 9]}.
constexpr char kGoldenManifest[] =
    "\x4b\x56\x4d\x41\x4e\x30\x30\x31\x01\x00\x00\x00\x00\x00\x00\x00"
    "\x03\x00\x00\x00\x00\x00\x00\x00\x06\x00\x00\x00\x00\x00\x00\x00"
    "\x09\x00\x00\x00\x00\x00\x00\x00\x05\x00\x00\x00\x00\x00\x00\x00"
    "\x02\x00\x00\x00\x00\x00\x00\x00\x02\x00\x00\x00\x00\x00\x00\x00"
    "\x9d\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00"
    "\x04\x00\x00\x00\x00\x00\x00\x00\x04\x00\x00\x00\x00\x00\x00\x00"
    "\xc8\x00\x00\x00\x00\x00\x00\x00\x06\x00\x00\x00\x00\x00\x00\x00"
    "\x09\x00\x00\x00\x00\x00\x00\x00\x2a\x8f\x84\x45";
const std::string kGolden(kGoldenManifest, sizeof(kGoldenManifest) - 1);

Manifest GoldenManifest() {
    Manifest m;
    m.generation = 3;
    m.next_file_id = 6;
    m.checkpoint = 9;
    m.active_wal_id = 5;
    m.ssts = {{2, 157, 1, 4}, {4, 200, 6, 9}};
    return m;
}

RawManifest GoldenRaw() {
    RawManifest m;
    m.generation = 3;
    m.next_file_id = 6;
    m.checkpoint = 9;
    m.active_wal_id = 5;
    m.ssts = {{2, 157, 1, 4}, {4, 200, 6, 9}};
    return m;
}

bool Equal(const Manifest& a, const Manifest& b) {
    if (a.generation != b.generation || a.next_file_id != b.next_file_id ||
        a.checkpoint != b.checkpoint || a.active_wal_id != b.active_wal_id ||
        a.ssts.size() != b.ssts.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.ssts.size(); ++i) {
        const ManifestSst& x = a.ssts[i];
        const ManifestSst& y = b.ssts[i];
        if (x.file_id != y.file_id || x.file_size != y.file_size ||
            x.min_sequence != y.min_sequence ||
            x.max_sequence != y.max_sequence) {
            return false;
        }
    }
    return true;
}

void ExpectDecodeFails(const std::string& bytes,
                       Code code,
                       std::string_view detail) {
    Manifest out;
    out.generation = 77;
    const Status s = DecodeManifest(bytes, out);
    EXPECT_EQ(s.code, code) << s.ToString();
    EXPECT_TRUE(Contains(s.message, detail)) << s.message;
    EXPECT_EQ(out.generation, 77u);  // unchanged on failure
}

TEST(Manifest, EncodingMatchesGoldenBytes) {
    std::string encoded = "prefix";
    EncodeManifest(GoldenManifest(), &encoded);
    EXPECT_EQ(encoded, "prefix" + kGolden);
    EXPECT_EQ(ManifestBytes(GoldenRaw()), kGolden);
    EXPECT_EQ(kGolden.size(), kManifestMinSize + 2 * kManifestSstEntrySize);
}

TEST(Manifest, GoldenBytesDecode) {
    Manifest m;
    ASSERT_TRUE(DecodeManifest(kGolden, m).ok());
    EXPECT_TRUE(Equal(m, GoldenManifest()));

    // The smallest manifest: no SSTs.
    ASSERT_TRUE(DecodeManifest(ManifestBytes(RawManifest{}), m).ok());
    EXPECT_EQ(m.generation, 1u);
    EXPECT_EQ(m.next_file_id, 2u);
    EXPECT_EQ(m.checkpoint, 0u);
    EXPECT_EQ(m.active_wal_id, 1u);
    EXPECT_TRUE(m.ssts.empty());
}

TEST(Manifest, ExtremeLegalValuesRoundTrip) {
    Manifest m;
    m.generation = kMax;
    m.next_file_id = kMax;
    m.checkpoint = kMax;
    m.active_wal_id = kMax - 1;
    m.ssts = {{kMax - 2, kMax, kMax, kMax}};
    ASSERT_TRUE(ValidateManifest(m).ok());
    std::string bytes;
    EncodeManifest(m, &bytes);
    Manifest out;
    ASSERT_TRUE(DecodeManifest(bytes, out).ok());
    EXPECT_TRUE(Equal(out, m));

    // An empty SST list with a nonzero checkpoint is valid (it is what a
    // compaction that drops everything publishes in M3).
    RawManifest empty;
    empty.checkpoint = 40;
    ASSERT_TRUE(DecodeManifest(ManifestBytes(empty), out).ok());
    EXPECT_EQ(out.checkpoint, 40u);
}

TEST(Manifest, EveryTruncationIsCorruption) {
    for (std::size_t cut = 0; cut < kGolden.size(); ++cut) {
        SCOPED_TRACE("cut " + std::to_string(cut));
        ExpectDecodeFails(kGolden.substr(0, cut), Code::Corruption, "");
    }
}

TEST(Manifest, EveryBitFlipFailsClosed) {
    for (std::size_t pos = 0; pos < kGolden.size(); ++pos) {
        for (unsigned char bit : {0x01, 0x80}) {
            SCOPED_TRACE("byte " + std::to_string(pos));
            std::string bytes = kGolden;
            bytes[pos] = static_cast<char>(bytes[pos] ^ bit);
            // The version is identified before the checksum is trusted.
            const Code want = (pos >= 8 && pos < 12) ? Code::UnsupportedFormat
                                                     : Code::Corruption;
            ExpectDecodeFails(bytes, want, "");
        }
    }
}

TEST(Manifest, UnknownVersionIsUnsupportedFormat) {
    for (std::uint32_t version : {0u, 2u, 0xffffffffu}) {
        RawManifest m = GoldenRaw();
        m.version = version;
        ExpectDecodeFails(ManifestBytes(m), Code::UnsupportedFormat,
                          "unsupported manifest version " +
                              std::to_string(version));
        // Checked before size limits, so even a short file reports it.
        ExpectDecodeFails(ManifestBytes(m).substr(0, 12),
                          Code::UnsupportedFormat, "version");
    }
    RawManifest m = GoldenRaw();
    m.magic = "KVMAN002";
    ExpectDecodeFails(ManifestBytes(m), Code::Corruption, "bad magic");
}

TEST(Manifest, InvalidFieldsWithCorrectChecksumAreCorruption) {
    struct Case {
        const char* name;
        void (*mutate)(RawManifest&);
        const char* detail;
    };
    const Case cases[] = {
        {"reserved1", [](RawManifest& m) { m.reserved1 = 1; }, "reserved"},
        {"reserved2", [](RawManifest& m) { m.reserved2 = 0x100; }, "reserved"},
        {"generation 0", [](RawManifest& m) { m.generation = 0; },
         "generation is zero"},
        {"active WAL 0", [](RawManifest& m) { m.active_wal_id = 0; },
         "active WAL ID is zero"},
        {"next ID = active", [](RawManifest& m) { m.next_file_id = 5; },
         "does not exceed referenced ID 5"},
        {"next ID below SST",
         [](RawManifest& m) {
             m.active_wal_id = 1;
             m.next_file_id = 4;
         },
         "does not exceed referenced ID 4"},
        {"file ID 0", [](RawManifest& m) { m.ssts[0].file_id = 0; },
         "file ID is zero"},
        {"duplicate SST IDs", [](RawManifest& m) { m.ssts[1].file_id = 2; },
         "duplicate file ID"},
        {"SST ID = WAL ID", [](RawManifest& m) { m.ssts[1].file_id = 5; },
         "duplicate file ID"},
        {"tiny SST", [](RawManifest& m) { m.ssts[0].file_size = 96; },
         "below the minimum SST size"},
        {"min sequence 0", [](RawManifest& m) { m.ssts[0].min_sequence = 0; },
         "invalid sequence range"},
        {"min > max", [](RawManifest& m) { m.ssts[0].min_sequence = 5; },
         "invalid sequence range"},
        {"overlap", [](RawManifest& m) { m.ssts[1].min_sequence = 4; },
         "does not follow the previous SST"},
        {"newest first",
         [](RawManifest& m) { std::swap(m.ssts[0], m.ssts[1]); },
         "does not follow the previous SST"},
        {"above checkpoint", [](RawManifest& m) { m.checkpoint = 8; },
         "exceeds checkpoint 8"},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.name);
        RawManifest m = GoldenRaw();
        c.mutate(m);
        ExpectDecodeFails(ManifestBytes(m), Code::Corruption, c.detail);
    }
}

TEST(Manifest, CountAndSizeMustAgreeWithinBounds) {
    for (std::uint32_t count : {0u, 1u, 3u, kMaxManifestSsts,
                                kMaxManifestSsts + 1, 0xffffffffu}) {
        SCOPED_TRACE(count);
        RawManifest m = GoldenRaw();
        m.override_count = true;
        m.sst_count = count;
        ExpectDecodeFails(ManifestBytes(m), Code::Corruption, "");
    }
    // A sealed file larger than the version 1 maximum is rejected by size
    // before anything else is decoded.
    std::string huge = ManifestBody(RawManifest{});
    huge.resize(kMaxManifestSize + 4, '\0');
    test::AppendLE(huge, test::RefCrc32c(huge), 4);
    ExpectDecodeFails(huge, Code::Corruption, "out of range");
}

class ManifestFileTest : public ::testing::Test {
protected:
    fs::path Path() const { return tmp_.path() / "MANIFEST"; }
    test::TempDir tmp_;
};

TEST_F(ManifestFileTest, ReadsThroughTheSeamAndNamesThePath) {
    ASSERT_TRUE(test::WriteBytes(Path(), kGolden));
    test::FaultSyscalls sys;
    sys.armed = true;
    Manifest m;
    ASSERT_TRUE(ReadManifestFile(Path(), sys, m).ok());
    EXPECT_TRUE(Equal(m, GoldenManifest()));
    EXPECT_GE(sys.preads, 2u);
    EXPECT_EQ(sys.closes, 1u);

    sys.FailOn("pread", "MANIFEST", EIO);
    Status s = ReadManifestFile(Path(), sys, m);
    EXPECT_EQ(s.code, Code::IOError) << s.ToString();

    std::string bad = kGolden;
    bad[20] ^= 1;
    ASSERT_TRUE(test::WriteBytes(Path(), bad));
    s = ReadManifestFile(Path(), sys, m);
    EXPECT_EQ(s.code, Code::Corruption) << s.ToString();
    EXPECT_TRUE(Contains(s.message, Path().string())) << s.message;
}

TEST_F(ManifestFileTest, OversizedFileIsRejectedBeforeReadingItAll) {
    // A sparse file far above the limit: only the 12-byte prefix is read.
    std::string prefix = ManifestBody(RawManifest{}).substr(0, 12);
    ASSERT_TRUE(test::WriteBytes(Path(), prefix));
    fs::resize_file(Path(), std::uint64_t{1} << 40);
    test::FaultSyscalls sys;
    sys.armed = true;
    Manifest m;
    const Status s = ReadManifestFile(Path(), sys, m);
    EXPECT_EQ(s.code, Code::Corruption) << s.ToString();
    EXPECT_TRUE(Contains(s.message, "out of range")) << s.message;
    EXPECT_EQ(sys.preads, 1u);
}

}  // namespace
}  // namespace kv
