#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>

#include "core/file_names.h"

namespace kv {
namespace {

constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();

TEST(FileNames, FormatPadsToSixDigitsWithoutTruncating) {
    EXPECT_EQ(WalFileName(1), "wal-000001.log");
    EXPECT_EQ(SstFileName(42), "sst-000042.sst");
    EXPECT_EQ(SstFileName(999999), "sst-999999.sst");
    EXPECT_EQ(SstFileName(1000000), "sst-1000000.sst");
    EXPECT_EQ(WalFileName(kMax), "wal-18446744073709551615.log");
    EXPECT_EQ(TempFileName(SstFileName(7)), "sst-000007.sst.tmp");
}

TEST(FileNames, ParseRecognizesEveryKindAndRoundTrips) {
    struct Case {
        std::string name;
        FileKind kind;
        std::uint64_t id;
    };
    const Case cases[] = {
        {"wal-000001.log", FileKind::kWal, 1},
        {"wal-000001.log.tmp", FileKind::kWalTemp, 1},
        {"sst-000002.sst", FileKind::kSst, 2},
        {"sst-000002.sst.tmp", FileKind::kSstTemp, 2},
        {"sst-1000000.sst", FileKind::kSst, 1000000},
        {"wal-18446744073709551615.log", FileKind::kWal, kMax},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.name);
        FileKind kind = FileKind::kSstTemp;
        std::uint64_t id = 0;
        ASSERT_TRUE(ParseFileName(c.name, kind, id));
        EXPECT_EQ(kind, c.kind);
        EXPECT_EQ(id, c.id);
    }
    for (std::uint64_t id : {std::uint64_t{1}, std::uint64_t{123456},
                             std::uint64_t{1234567}, kMax - 1, kMax}) {
        FileKind kind;
        std::uint64_t parsed = 0;
        ASSERT_TRUE(ParseFileName(SstFileName(id), kind, parsed));
        EXPECT_EQ(parsed, id);
    }
}

TEST(FileNames, ParseRejectsNonCanonicalAndForeignNames) {
    const char* names[] = {
        "wal-00001.log",                  // too few digits
        "wal-0000001.log",                // extra leading zero
        "sst-000000.sst",                 // zero ID
        "wal-18446744073709551616.log",   // 2^64
        "wal-99999999999999999999.log",   // overflow
        "wal-000001a.log", "wal-+00001.log", "wal- 00001.log",
        "wal-000001.LOG", "WAL-000001.log", "wal-000001.log.tmp.tmp",
        "wal-000001.sst", "sst-000001.log", "wal-.log", "wal-000001",
        "000001.wal", "000001.wal.tmp", "MANIFEST", "MANIFEST.tmp", "LOCK",
        "", "sst-000001.sst.bak",
    };
    for (const char* name : names) {
        FileKind kind = FileKind::kWal;
        std::uint64_t id = 77;
        EXPECT_FALSE(ParseFileName(name, kind, id)) << name;
        EXPECT_EQ(kind, FileKind::kWal) << name;
        EXPECT_EQ(id, 77u) << name;
    }
}

}  // namespace
}  // namespace kv
