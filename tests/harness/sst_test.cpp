#include <gtest/gtest.h>

#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "core/posix_file.h"
#include "core/sst.h"
#include "core/types.h"
#include "testing/fault_syscalls.h"
#include "testing/sst_builder.h"
#include "testing/temp_dir.h"

namespace kv {
namespace {

namespace fs = std::filesystem;

using test::RawBlock;
using test::RawFooter;
using test::RawFooterBytes;
using test::RawIndexBytes;
using test::RawIndexEntry;
using test::RawSst;
using test::RawSstEntry;
using test::RawSstEntryBytes;
using test::ReadBytesOrEmpty;
using test::SealBlock;
using test::WriteBytes;

bool Contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

// Expected bytes for the golden SST, computed outside this code base (by a
// separate script with its own CRC-32C and little-endian packing):
//   ("\x00\xff", seq 2, Put "\x01"), ("a", seq 3, Put "hi"),
//   ("b", seq 5, Delete), ("c", seq 4, Put "")
// in one 84-byte block, a 25-byte index, and the 48-byte footer.
constexpr char kGoldenSst[] =
    "\x02\x00\x00\x00\x01\x00\x00\x00\x02\x00\x00\x00\x00\x00\x00\x00"
    "\x01\x00\xff\x01\x01\x00\x00\x00\x02\x00\x00\x00\x03\x00\x00\x00"
    "\x00\x00\x00\x00\x01\x61\x68\x69\x01\x00\x00\x00\x00\x00\x00\x00"
    "\x05\x00\x00\x00\x00\x00\x00\x00\x02\x62\x01\x00\x00\x00\x00\x00"
    "\x00\x00\x04\x00\x00\x00\x00\x00\x00\x00\x01\x63\x04\x00\x00\x00"
    "\xc4\x69\x24\x1b\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
    "\x54\x00\x00\x00\x63\x01\x00\x00\x00\xa8\xb0\xd9\x00\x8d\x2e\x9f"
    "\x0b\x54\x00\x00\x00\x00\x00\x00\x00\x19\x00\x00\x00\x00\x00\x00"
    "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
    "\x00\x01\x00\x00\x00\x4b\x56\x53\x53\x54\x30\x30\x31";
const std::string kGolden(kGoldenSst, sizeof(kGoldenSst) - 1);

std::vector<RawSstEntry> GoldenEntries() {
    return {
        {std::string("\x00\xff", 2), 2, test::kRawPut, "\x01"},
        {"a", 3, test::kRawPut, "hi"},
        {"b", 5, test::kRawDelete, ""},
        {"c", 4, test::kRawPut, ""},
    };
}

class SstTest : public ::testing::Test {
protected:
    fs::path Path() const { return tmp_.path() / "t.sst"; }

    // Writes entries through the production builder; returns the bytes.
    std::string Build(const std::vector<RawSstEntry>& entries) {
        fs::remove(Path());
        std::unique_ptr<WritableFile> file;
        EXPECT_TRUE(NewWritableFile(Path(), file).ok());
        SstBuilder builder(*file);
        for (const RawSstEntry& e : entries) {
            const Status s = builder.Add(e.key, e.sequence,
                                         static_cast<ValueType>(e.type),
                                         e.value);
            EXPECT_TRUE(s.ok()) << s.ToString();
        }
        const Status s = builder.Finish();
        EXPECT_TRUE(s.ok()) << s.ToString();
        EXPECT_TRUE(file->Close().ok());
        const std::string bytes = ReadBytesOrEmpty(Path());
        EXPECT_EQ(builder.summary().file_size, bytes.size());
        return bytes;
    }

    // Adds entries and finishes under limits. Returns the first error and
    // sets *failed_call to the failing Add's index, or to entries.size()
    // for Finish. Whatever was written stays in Path().
    Status BuildLimited(const std::vector<RawSstEntry>& entries,
                        SstLimits limits,
                        std::size_t* failed_call) {
        fs::remove(Path());
        std::unique_ptr<WritableFile> file;
        Status s = NewWritableFile(Path(), file);
        if (!s.ok()) {
            return s;
        }
        SstBuilder builder(*file, limits);
        std::size_t call = 0;
        for (; call < entries.size() && s.ok(); ++call) {
            const RawSstEntry& e = entries[call];
            s = builder.Add(e.key, e.sequence,
                            static_cast<ValueType>(e.type), e.value);
        }
        if (s.ok()) {
            s = builder.Finish();
            call = entries.size() + 1;
        }
        *failed_call = call - 1;
        EXPECT_TRUE(file->Close().ok());
        return s;
    }

    Status OpenBytes(const std::string& bytes,
                     std::unique_ptr<SstTable>& table,
                     std::optional<std::uint64_t> expected = std::nullopt) {
        table.reset();
        EXPECT_TRUE(WriteBytes(Path(), bytes));
        return SstTable::Open(Path(), expected.value_or(bytes.size()),
                              DefaultPosixSyscalls(), table);
    }

    // Open must fail with code and a message containing detail.
    void ExpectOpenFails(const std::string& bytes,
                         Code code,
                         std::string_view detail) {
        std::unique_ptr<SstTable> table;
        const Status s = OpenBytes(bytes, table);
        EXPECT_EQ(s.code, code) << s.ToString();
        EXPECT_TRUE(Contains(s.message, detail)) << s.message;
        EXPECT_TRUE(Contains(s.message, Path().string())) << s.message;
        EXPECT_EQ(table, nullptr);
    }

    test::TempDir tmp_;
};

struct Probe {
    LookupResult result;
    std::string value;
};

Probe Get(const SstTable& table, std::string_view key) {
    Probe p{LookupResult::kAbsent, "untouched"};
    const Status s = table.Get(key, p.result, &p.value);
    EXPECT_TRUE(s.ok()) << s.ToString();
    return p;
}

// ---- Golden vector and round trip -----------------------------------------

TEST_F(SstTest, ProductionAndTestBuildersMatchGoldenBytes) {
    EXPECT_EQ(Build(GoldenEntries()), kGolden);
    EXPECT_EQ(RawSst({GoldenEntries()}), kGolden);
    EXPECT_EQ(kGolden.size(), kMinSstFileSize + 60);
}

TEST_F(SstTest, GoldenFileOpensAndAnswersLookups) {
    std::unique_ptr<SstTable> table;
    ASSERT_TRUE(OpenBytes(kGolden, table).ok());
    const SstSummary& summary = table->summary();
    EXPECT_EQ(summary.min_sequence, 2u);
    EXPECT_EQ(summary.max_sequence, 5u);
    EXPECT_EQ(summary.file_size, kGolden.size());
    EXPECT_EQ(summary.smallest_key, std::string("\x00\xff", 2));
    ASSERT_EQ(summary.index.size(), 1u);

    Probe p = Get(*table, std::string("\x00\xff", 2));
    EXPECT_EQ(p.result, LookupResult::kFound);
    EXPECT_EQ(p.value, "\x01");
    p = Get(*table, "a");
    EXPECT_EQ(p.result, LookupResult::kFound);
    EXPECT_EQ(p.value, "hi");
    p = Get(*table, "b");
    EXPECT_EQ(p.result, LookupResult::kDeleted);
    EXPECT_EQ(p.value, "untouched");
    p = Get(*table, "c");
    EXPECT_EQ(p.result, LookupResult::kFound);
    EXPECT_EQ(p.value, "");  // empty value, distinct from Delete

    for (std::string_view missing :
         {std::string_view(""), std::string_view("\x00", 1),
          std::string_view("a0"), std::string_view("bb"),
          std::string_view("d"), std::string_view("\xff")}) {
        p = Get(*table, missing);
        EXPECT_EQ(p.result, LookupResult::kAbsent);
        EXPECT_EQ(p.value, "untouched");
    }
}

TEST_F(SstTest, MultiBlockLookupFindsFirstMiddleLastAndMisses) {
    std::vector<RawSstEntry> entries;
    for (int i = 0; i < 600; ++i) {
        char key[16];
        std::snprintf(key, sizeof(key), "key%05d", i * 2);
        entries.push_back({key, static_cast<std::uint64_t>(1000 + i),
                           i % 7 == 3 ? test::kRawDelete : test::kRawPut,
                           i % 7 == 3 ? "" : std::string(40, 'a' + i % 26)});
    }
    const std::string bytes = Build(entries);
    std::unique_ptr<SstTable> table;
    ASSERT_TRUE(OpenBytes(bytes, table).ok());
    const auto& index = table->summary().index;
    ASSERT_GT(index.size(), 5u);
    for (const SstBlockHandle& handle : index) {
        EXPECT_LE(handle.length, kSstBlockTarget + kSstBlockTrailerSize);
    }
    EXPECT_EQ(table->summary().min_sequence, 1000u);
    EXPECT_EQ(table->summary().max_sequence, 1599u);

    for (const RawSstEntry& e : entries) {
        const Probe p = Get(*table, e.key);
        if (e.type == test::kRawDelete) {
            EXPECT_EQ(p.result, LookupResult::kDeleted) << e.key;
        } else {
            EXPECT_EQ(p.result, LookupResult::kFound) << e.key;
            EXPECT_EQ(p.value, e.value) << e.key;
        }
    }
    // Odd numbers fall between keys, including at block boundaries.
    for (int i : {-1, 1, 599, 1197, 1199, 5000}) {
        char key[16];
        std::snprintf(key, sizeof(key), "key%05d", i);
        EXPECT_EQ(Get(*table, i < 0 ? "a" : key).result,
                  LookupResult::kAbsent) << key;
    }
    for (const SstBlockHandle& handle : index) {
        EXPECT_EQ(Get(*table, handle.last_key + '\0').result,
                  LookupResult::kAbsent);
    }
}

TEST_F(SstTest, LargeEntriesGetTheirOwnBlocksUpToApiLimits) {
    const std::string max_key(kMaxKeyLength, 'k');
    const std::string next_key(kMaxKeyLength, 'l');
    const std::string max_value(kMaxValueLength, '\x80');
    const std::vector<RawSstEntry> entries = {
        {"a", 1, test::kRawPut, "small"},
        {"b", 2, test::kRawPut, std::string(kSstBlockTarget, 'b')},
        {"c", 3, test::kRawPut, "small"},
        {max_key, 4, test::kRawPut, max_value},
        {next_key, 5, test::kRawDelete, ""},
    };
    const std::string bytes = Build(entries);
    std::unique_ptr<SstTable> table;
    ASSERT_TRUE(OpenBytes(bytes, table).ok());
    const auto& index = table->summary().index;
    // "a"; "b" alone (oversized); "c"; the maximum entry alone; the
    // 64 KiB-key tombstone alone (its entry also exceeds the target).
    ASSERT_EQ(index.size(), 5u);
    EXPECT_EQ(index[3].length, kMaxSstBlockSize);
    EXPECT_EQ(index[1].last_key, "b");

    Probe p = Get(*table, max_key);
    EXPECT_EQ(p.result, LookupResult::kFound);
    EXPECT_TRUE(p.value == max_value);
    EXPECT_EQ(Get(*table, next_key).result, LookupResult::kDeleted);
    p = Get(*table, "b");
    EXPECT_EQ(p.value, std::string(kSstBlockTarget, 'b'));
}

TEST_F(SstTest, BuilderRejectsInvalidInputBeforeWriting) {
    std::unique_ptr<WritableFile> file;
    ASSERT_TRUE(NewWritableFile(Path(), file).ok());
    SstBuilder builder(*file);
    EXPECT_EQ(builder.Finish().code, Code::InvalidArgument);  // empty SST
    ASSERT_TRUE(builder.Add("m", 5, ValueType::kPut, "v").ok());

    EXPECT_EQ(builder.Add("m", 6, ValueType::kPut, "v").code,
              Code::InvalidArgument);  // duplicate key
    EXPECT_EQ(builder.Add("a", 6, ValueType::kPut, "v").code,
              Code::InvalidArgument);  // decreasing key
    EXPECT_EQ(builder.Add("n", 0, ValueType::kPut, "v").code,
              Code::InvalidArgument);
    EXPECT_EQ(builder.Add("n", 6, ValueType::kDelete, "v").code,
              Code::InvalidArgument);
    EXPECT_EQ(builder.Add("n", 6, static_cast<ValueType>(3), "").code,
              Code::InvalidArgument);
    EXPECT_EQ(builder.Add(std::string(kMaxKeyLength + 1, 'n'), 6,
                          ValueType::kPut, "").code,
              Code::InvalidArgument);
    EXPECT_EQ(builder.Add("n", 6, ValueType::kPut,
                          std::string(kMaxValueLength + 1, 'v')).code,
              Code::InvalidArgument);
    EXPECT_EQ(fs::file_size(Path()), 0u);  // nothing written yet

    ASSERT_TRUE(builder.Finish().ok());
    EXPECT_EQ(builder.Add("z", 9, ValueType::kPut, "").code,
              Code::InvalidArgument);
    ASSERT_TRUE(file->Close().ok());

    std::unique_ptr<SstTable> table;
    ASSERT_TRUE(SstTable::Open(Path(), fs::file_size(Path()),
                               DefaultPosixSyscalls(), table).ok());
    EXPECT_EQ(Get(*table, "m").value, "v");
}

// The production limits need about 64 MiB of index or a million blocks to
// reach, so these tests lower them; the builder runs the same checks.
TEST_F(SstTest, BuilderRefusesABlockPastTheBlockLimitBeforeWritingIt) {
    // Each entry nearly fills a block, so every entry gets its own block.
    const std::string value(kSstBlockTarget - 100, 'v');
    const std::uint64_t block_size =
        kSstEntryHeaderSize + 1 + value.size() + kSstBlockTrailerSize;
    auto entries = [&](std::string_view keys) {
        std::vector<RawSstEntry> out;
        for (const char k : keys) {
            out.push_back({std::string(1, k), 1, test::kRawPut, value});
        }
        return out;
    };
    const SstLimits limits{3, kMaxSstIndexSize};
    std::size_t failed = 0;

    // Exactly at the limit.
    ASSERT_TRUE(BuildLimited(entries("abc"), limits, &failed).ok());
    std::unique_ptr<SstTable> table;
    ASSERT_TRUE(SstTable::Open(Path(), fs::file_size(Path()),
                               DefaultPosixSyscalls(), table).ok());
    EXPECT_EQ(table->summary().index.size(), 3u);

    // Finish refuses the fourth block; nothing after the third is written.
    Status s = BuildLimited(entries("abcd"), limits, &failed);
    EXPECT_EQ(s.code, Code::InvalidArgument) << s.ToString();
    EXPECT_TRUE(Contains(s.message, "would exceed 3 blocks")) << s.message;
    EXPECT_EQ(failed, 4u);  // Finish
    EXPECT_EQ(fs::file_size(Path()), 3 * block_size);

    // So does the Add that would close it.
    s = BuildLimited(entries("abcde"), limits, &failed);
    EXPECT_EQ(s.code, Code::InvalidArgument) << s.ToString();
    EXPECT_EQ(failed, 4u);  // Add("e")
    EXPECT_EQ(fs::file_size(Path()), 3 * block_size);
}

TEST_F(SstTest, BuilderIndexLimitIsExactAndCountsEachBlocksLastKey) {
    // "a" fills its block. "b" starts the second block and the long key
    // joins it, so the second index entry carries the long key.
    const std::string long_key(300, 'c');
    const std::vector<RawSstEntry> entries = {
        {"a", 1, test::kRawPut,
         std::string(kSstBlockTarget - kSstEntryHeaderSize - 1, 'v')},
        {"b", 2, test::kRawPut, "1"},
        {long_key, 3, test::kRawPut, "2"},
    };
    const std::uint64_t index_size = (kSstIndexEntryHeaderSize + 1) +
                                     (kSstIndexEntryHeaderSize + 300) +
                                     kSstIndexTrailerSize;
    const std::string unlimited = Build(entries);
    std::uint64_t footer_index_length = 0;
    for (int i = 7; i >= 0; --i) {
        footer_index_length = (footer_index_length << 8) |
                              static_cast<unsigned char>(
                                  unlimited[unlimited.size() - 36 + i]);
    }
    ASSERT_EQ(footer_index_length, index_size);

    // Exactly at the limit: the same bytes as without a limit.
    std::size_t failed = 0;
    ASSERT_TRUE(
        BuildLimited(entries, SstLimits{kMaxSstBlocks, index_size}, &failed)
            .ok());
    EXPECT_TRUE(ReadBytesOrEmpty(Path()) == unlimited);

    // One byte less: counting "b" instead of the block's last key would
    // still fit, and only the first block may be written.
    const Status s = BuildLimited(
        entries, SstLimits{kMaxSstBlocks, index_size - 1}, &failed);
    EXPECT_EQ(s.code, Code::InvalidArgument) << s.ToString();
    EXPECT_TRUE(Contains(s.message,
                         "index would exceed " +
                             std::to_string(index_size - 1) + " bytes"))
        << s.message;
    EXPECT_EQ(failed, entries.size());  // Finish
    EXPECT_EQ(fs::file_size(Path()),
              kSstBlockTarget + kSstBlockTrailerSize);
}

// ---- Truncation and bit flips -------------------------------------------------

TEST_F(SstTest, EveryTruncationIsCorruption) {
    for (std::size_t cut = 0; cut < kGolden.size(); ++cut) {
        SCOPED_TRACE("cut " + std::to_string(cut));
        const std::string bytes = kGolden.substr(0, cut);
        std::unique_ptr<SstTable> table;
        // The manifest still records the full size.
        Status s = OpenBytes(bytes, table, kGolden.size());
        EXPECT_EQ(s.code, Code::Corruption) << s.ToString();
        EXPECT_TRUE(Contains(s.message, "differs from the expected"));
        // Even if the recorded size agreed, the structure fails closed.
        s = OpenBytes(bytes, table, cut);
        EXPECT_EQ(s.code, Code::Corruption) << s.ToString();
        EXPECT_EQ(table, nullptr);
    }
}

TEST_F(SstTest, EveryBitFlipFailsClosed) {
    const std::size_t version_at = kGolden.size() - 12;
    for (std::size_t pos = 0; pos < kGolden.size(); ++pos) {
        for (unsigned char bit : {0x01, 0x80}) {
            SCOPED_TRACE("byte " + std::to_string(pos) + " bit " +
                         std::to_string(bit));
            std::string bytes = kGolden;
            bytes[pos] = static_cast<char>(bytes[pos] ^ bit);
            std::unique_ptr<SstTable> table;
            const Status s = OpenBytes(bytes, table);
            // The version is identified before the footer CRC is trusted.
            const Code want = (pos >= version_at && pos < version_at + 4)
                                  ? Code::UnsupportedFormat
                                  : Code::Corruption;
            EXPECT_EQ(s.code, want) << s.ToString();
            EXPECT_EQ(table, nullptr);
        }
    }
}

// ---- Malformed structure with correct checksums --------------------------------

// Assembles data, an index, and a footer; footer fields default to the
// consistent values.
std::string Assemble(const std::string& data,
                     const std::string& index,
                     const RawFooter* footer = nullptr) {
    RawFooter f;
    if (footer != nullptr) {
        f = *footer;
    } else {
        f.index_offset = data.size();
        f.index_length = index.size();
    }
    return data + index + RawFooterBytes(f);
}

std::string OneBlockSst(const std::string& block, const std::string& last) {
    return Assemble(block,
                    RawIndexBytes({{last, 0,
                                    static_cast<std::uint32_t>(block.size())}}));
}

TEST_F(SstTest, MalformedFooterFieldsFailClosed) {
    const std::string block = RawBlock({{"k", 1, test::kRawPut, "v"}});
    const std::string index = RawIndexBytes(
        {{"k", 0, static_cast<std::uint32_t>(block.size())}});
    auto with = [&](auto mutate) {
        RawFooter f;
        f.index_offset = block.size();
        f.index_length = index.size();
        mutate(f);
        return Assemble(block, index, &f);
    };

    ExpectOpenFails(with([](RawFooter& f) { f.version = 2; }),
                    Code::UnsupportedFormat, "unsupported SST version 2");
    ExpectOpenFails(with([](RawFooter& f) { f.version = 0; }),
                    Code::UnsupportedFormat, "version 0");
    ExpectOpenFails(with([](RawFooter& f) { f.magic = "KVSST002"; }),
                    Code::Corruption, "bad magic");
    ExpectOpenFails(with([](RawFooter& f) { f.filter_offset = 1; }),
                    Code::Corruption, "filter");
    ExpectOpenFails(with([](RawFooter& f) { f.filter_length = 8; }),
                    Code::Corruption, "filter");
    ExpectOpenFails(with([](RawFooter& f) { f.index_offset = 0; }),
                    Code::Corruption, "inconsistent");
    ExpectOpenFails(with([](RawFooter& f) { f.index_offset += 1; }),
                    Code::Corruption, "inconsistent");
    ExpectOpenFails(with([](RawFooter& f) { f.index_length += 1; }),
                    Code::Corruption, "inconsistent");
    // Huge values must be rejected by range checks, never allocated.
    ExpectOpenFails(with([](RawFooter& f) { f.index_length = ~0ull; }),
                    Code::Corruption, "inconsistent");
    ExpectOpenFails(with([](RawFooter& f) { f.index_offset = ~0ull; }),
                    Code::Corruption, "inconsistent");
}

TEST_F(SstTest, MalformedIndexFailsClosed) {
    const std::string b1 = RawBlock({{"a", 1, test::kRawPut, "1"}});
    const std::string b2 = RawBlock({{"b", 2, test::kRawPut, "2"}});
    const auto n1 = static_cast<std::uint32_t>(b1.size());
    const auto n2 = static_cast<std::uint32_t>(b2.size());
    const std::string data = b1 + b2;
    auto sst = [&](std::vector<RawIndexEntry> index) {
        return Assemble(data, RawIndexBytes(index));
    };

    EXPECT_TRUE([&] {
        std::unique_ptr<SstTable> t;
        return OpenBytes(sst({{"a", 0, n1}, {"b", n1, n2}}), t).ok();
    }());

    ExpectOpenFails(Assemble(data, RawIndexBytes({}, false, 0)),
                    Code::Corruption, "index length 8 is below the minimum");
    ExpectOpenFails(
        Assemble(data, RawIndexBytes({{"a", 0, n1}, {"b", n1, n2}}, true, 0)),
        Code::Corruption, "index has no blocks");
    ExpectOpenFails(
        Assemble(data, RawIndexBytes({{"a", 0, n1}, {"b", n1, n2}}, true, 3)),
        Code::Corruption, "extends past the index");
    ExpectOpenFails(
        Assemble(data, RawIndexBytes({{"a", 0, n1}, {"b", n1, n2}}, true, 1)),
        Code::Corruption, "after its last entry");
    ExpectOpenFails(sst({{"a", 0, n1}, {"b", n1 + 1, n2 - 1}}),
                    Code::Corruption, "block offset");  // gap
    ExpectOpenFails(sst({{"a", 0, n1}, {"b", n1 - 1, n2 + 1}}),
                    Code::Corruption, "block offset");  // overlap
    ExpectOpenFails(sst({{"a", 8, n1}, {"b", n1, n2}}), Code::Corruption,
                    "block offset");
    ExpectOpenFails(sst({{"a", 0, n1}}), Code::Corruption,
                    "but the index starts at");  // blocks end early
    ExpectOpenFails(sst({{"a", 0, n1}, {"b", n1, n2 + 1}}), Code::Corruption,
                    "out of range");
    // One index entry spanning two real blocks reads as one bad block.
    ExpectOpenFails(sst({{"a", 0, n1 + n2}}), Code::Corruption,
                    "block checksum mismatch");
    ExpectOpenFails(sst({{"b", 0, n1}, {"a", n1, n2}}), Code::Corruption,
                    "not greater");
    ExpectOpenFails(sst({{"a", 0, n1}, {"a", n1, n2}}), Code::Corruption,
                    "not greater");
    ExpectOpenFails(sst({{"a", 0, n1}, {"c", n1, n2}}), Code::Corruption,
                    "last key differs");

    RawIndexEntry huge_key{"a", 0, n1, true, 0xffffffffu};
    ExpectOpenFails(sst({huge_key, {"b", n1, n2}}), Code::Corruption,
                    "key length 4294967295 exceeds limit");
    RawIndexEntry long_key{"a", 0, n1, true, 1000};
    ExpectOpenFails(sst({long_key, {"b", n1, n2}}), Code::Corruption,
                    "key extends past the index");

    // A block length below the minimum or above the maximum.
    const std::string tiny = SealBlock("", 0);
    ExpectOpenFails(Assemble(b1 + tiny,
                             RawIndexBytes({{"a", 0, n1},
                                            {"a", n1, static_cast<std::uint32_t>(
                                                          tiny.size())}})),
                    Code::Corruption, "out of range");
    ExpectOpenFails(sst({{"a", 0, static_cast<std::uint32_t>(
                                          kMaxSstBlockSize + 1)}}),
                    Code::Corruption, "out of range");
}

// Writes data at offset 0 and the footer at the size the footer declares,
// leaving a hole between them, so a large declared index costs no storage.
bool WriteSparseSst(const fs::path& path,
                    const std::string& data,
                    const RawFooter& f) {
    if (!WriteBytes(path, data)) {
        return false;
    }
    std::error_code ec;
    fs::resize_file(path, f.index_offset + f.index_length, ec);
    if (ec) {
        return false;
    }
    std::ofstream out(path, std::ios::binary | std::ios::app);
    const std::string footer = RawFooterBytes(f);
    out.write(footer.data(), static_cast<std::streamsize>(footer.size()));
    return out.good();
}

struct SparseOpen {
    Status status;
    std::uint64_t pread_bytes = 0;
    std::size_t closes = 0;
    bool opened = false;
};

SparseOpen OpenSparse(const fs::path& path,
                      const std::string& data,
                      const RawFooter& f) {
    SparseOpen r;
    if (!WriteSparseSst(path, data, f)) {
        r.status = Status::IOError("could not write the sparse SST");
        return r;
    }
    test::FaultSyscalls sys;
    sys.armed = true;
    std::unique_ptr<SstTable> table;
    r.status = SstTable::Open(path, fs::file_size(path), sys, table);
    r.pread_bytes = sys.pread_bytes;
    r.closes = sys.closes;
    r.opened = table != nullptr;
    return r;
}

// docs/sst-format.md section 3.4 step 2 (B-006): one 25-byte block admits
// at most one index entry, whose key is at most kMaxKeyLength bytes. A
// longer index_length is rejected from the footer alone; the index is
// never allocated or read.
TEST_F(SstTest, ImpossibleIndexLengthIsRejectedBeforeTheIndexIsRead) {
    const std::string block = RawBlock({{"", 1, test::kRawPut, ""}});
    ASSERT_EQ(block.size(), kMinSstBlockSize);
    const std::uint64_t largest =
        kSstIndexEntryHeaderSize + kMaxKeyLength + kSstIndexTrailerSize;

    for (const std::uint64_t length : {std::uint64_t{256} << 20, largest + 1}) {
        SCOPED_TRACE("index_length " + std::to_string(length));
        RawFooter f;
        f.index_offset = block.size();
        f.index_length = length;
        const SparseOpen r = OpenSparse(Path(), block, f);
        EXPECT_EQ(r.status.code, Code::Corruption) << r.status.ToString();
        EXPECT_TRUE(Contains(r.status.message,
                             "index length " + std::to_string(length) +
                                 " exceeds"))
            << r.status.message;
        EXPECT_TRUE(Contains(r.status.message, Path().string()));
        EXPECT_EQ(r.pread_bytes, kSstFooterSize);  // the footer only
        EXPECT_EQ(r.closes, 1u);
        EXPECT_FALSE(r.opened);
    }

    // The bound itself is admitted: the index is read and fails its CRC.
    RawFooter f;
    f.index_offset = block.size();
    f.index_length = largest;
    const SparseOpen r = OpenSparse(Path(), block, f);
    EXPECT_EQ(r.status.code, Code::Corruption) << r.status.ToString();
    EXPECT_TRUE(Contains(r.status.message, "index checksum mismatch"))
        << r.status.message;
    EXPECT_EQ(r.pread_bytes, kSstFooterSize + largest);
    EXPECT_FALSE(r.opened);
}

// 1,024 minimum-size blocks would admit more than kMaxSstIndexSize bytes of
// index entries, so here only the format limit rejects the length. The
// limit itself is not opened, to avoid a 64 MiB buffer.
TEST_F(SstTest, IndexLengthAboveTheFormatLimitIsRejectedBeforeTheIndexIsRead) {
    RawFooter f;
    f.index_offset = 1024 * kMinSstBlockSize;
    f.index_length = kMaxSstIndexSize + 1;
    ASSERT_GT(1024 * (kSstIndexEntryHeaderSize + kMaxKeyLength) +
                  kSstIndexTrailerSize,
              f.index_length);
    const SparseOpen r = OpenSparse(Path(), "", f);
    EXPECT_EQ(r.status.code, Code::Corruption) << r.status.ToString();
    EXPECT_TRUE(Contains(r.status.message, "index length 67108865 exceeds"))
        << r.status.message;
    EXPECT_EQ(r.pread_bytes, kSstFooterSize);
    EXPECT_FALSE(r.opened);
}

TEST_F(SstTest, IndexBlockCountAboveTheLimitIsCorruption) {
    const std::string block = RawBlock({{"a", 1, test::kRawPut, "1"}});
    const auto n = static_cast<std::uint32_t>(block.size());
    ExpectOpenFails(
        Assemble(block, RawIndexBytes({{"a", 0, n}}, true, kMaxSstBlocks + 1)),
        Code::Corruption, "index has 1048577 blocks");
}

TEST_F(SstTest, MalformedBlocksFailClosed) {
    auto bad = [&](const std::string& block, const std::string& last,
                   std::string_view detail) {
        ExpectOpenFails(OneBlockSst(block, last), Code::Corruption, detail);
    };

    const RawSstEntry a{"a", 1, test::kRawPut, "1"};
    const RawSstEntry b{"b", 2, test::kRawPut, "2"};
    const std::string ab = RawSstEntryBytes(a) + RawSstEntryBytes(b);

    bad(SealBlock(ab, 0), "b", "no entries");
    bad(SealBlock(ab, 1), "a", "after its last entry");
    bad(SealBlock(ab, 3), "b", "header extends past");
    bad(SealBlock(RawSstEntryBytes(b) + RawSstEntryBytes(a), 2), "a",
        "not greater than the previous key");
    bad(SealBlock(RawSstEntryBytes(a) + RawSstEntryBytes(a), 2), "a",
        "not greater than the previous key");

    RawSstEntry e = a;
    e.sequence = 0;
    bad(RawBlock({e}), "a", "sequence is zero");
    e = a;
    e.type = 0;
    bad(RawBlock({e}), "a", "invalid type 0");
    e.type = 3;
    bad(RawBlock({e}), "a", "invalid type 3");
    e = {"a", 1, test::kRawDelete, "x"};
    bad(RawBlock({e}), "a", "Delete has value length 1");

    e = a;
    e.override_lengths = true;
    e.key_len = 0xffffffffu;
    e.value_len = 1;
    bad(RawBlock({e}), "a", "key length 4294967295 exceeds limit");
    e.key_len = 1;
    e.value_len = static_cast<std::uint32_t>(kMaxValueLength + 1);
    bad(RawBlock({e}), "a", "value length 16777217 exceeds limit");
    e.value_len = 100;
    bad(RawBlock({e}), "a", "entry extends past entries");

    // An entry region above the target may hold only one entry.
    const RawSstEntry big1{"a", 1, test::kRawPut, std::string(3000, 'x')};
    const RawSstEntry big2{"b", 2, test::kRawPut, std::string(3000, 'y')};
    bad(RawBlock({big1, big2}), "b", "only one is allowed");

    // A block whose CRC is wrong.
    std::string block = RawBlock({a});
    block[0] ^= 1;
    bad(block, "a", "block checksum mismatch");
}

TEST_F(SstTest, KeysMustIncreaseAcrossBlocks) {
    const std::string b1 = RawBlock({{"a", 1, test::kRawPut, "1"},
                                     {"m", 2, test::kRawPut, "2"}});
    const std::string b2 = RawBlock({{"c", 3, test::kRawPut, "3"},
                                     {"z", 4, test::kRawPut, "4"}});
    const auto n1 = static_cast<std::uint32_t>(b1.size());
    const auto n2 = static_cast<std::uint32_t>(b2.size());
    ExpectOpenFails(Assemble(b1 + b2, RawIndexBytes({{"m", 0, n1},
                                                     {"z", n1, n2}})),
                    Code::Corruption, "previous block's last key");
}

// ---- Get validates the block it reads -------------------------------------------

TEST_F(SstTest, GetRevalidatesTheBlockItReads) {
    std::unique_ptr<SstTable> table;
    ASSERT_TRUE(OpenBytes(kGolden, table).ok());

    std::string corrupt = kGolden;
    corrupt[20] ^= 0x04;  // inside the block, after Open validated it
    ASSERT_TRUE(WriteBytes(Path(), corrupt));
    LookupResult result = LookupResult::kFound;
    std::string value = "untouched";
    Status s = table->Get("a", result, &value);
    EXPECT_EQ(s.code, Code::Corruption) << s.ToString();
    EXPECT_TRUE(Contains(s.message, "block checksum mismatch")) << s.message;
    EXPECT_EQ(value, "untouched");

    // A key outside the index range needs no read at all.
    EXPECT_TRUE(table->Get("d", result, &value).ok());
    EXPECT_EQ(result, LookupResult::kAbsent);

    ASSERT_TRUE(WriteBytes(Path(), kGolden.substr(0, 40)));
    s = table->Get("a", result, &value);
    EXPECT_EQ(s.code, Code::IOError) << s.ToString();
    EXPECT_TRUE(Contains(s.message, "unexpected end of file")) << s.message;
}

TEST_F(SstTest, ReadsGoThroughTheSyscallSeam) {
    ASSERT_TRUE(WriteBytes(Path(), kGolden));
    test::FaultSyscalls sys;
    sys.armed = true;
    sys.pread_script = {test::FaultSyscalls::Short(3),
                        test::FaultSyscalls::Error(EINTR),
                        test::FaultSyscalls::Short(1)};
    std::unique_ptr<SstTable> table;
    ASSERT_TRUE(SstTable::Open(Path(), kGolden.size(), sys, table).ok());
    EXPECT_TRUE(sys.pread_script.empty());
    EXPECT_GE(sys.preads, 5u);
    EXPECT_EQ(sys.opens, 1u);
    EXPECT_EQ(sys.closes, 1u);  // no descriptor is kept

    sys.FailOn("pread", "t.sst", EIO);
    LookupResult result;
    std::string value;
    const Status s = table->Get("a", result, &value);
    EXPECT_EQ(s.code, Code::IOError) << s.ToString();
    EXPECT_EQ(sys.rules[0].fired, 1u);
    EXPECT_EQ(sys.closes, 2u);  // closed on the error path too
    ASSERT_TRUE(table->Get("a", result, &value).ok());
    EXPECT_EQ(value, "hi");

    std::unique_ptr<SstTable> other;
    sys.FailOn("open", "t.sst", EMFILE);
    EXPECT_EQ(SstTable::Open(Path(), kGolden.size(), sys, other).code,
              Code::IOError);
    EXPECT_EQ(other, nullptr);
}

}  // namespace
}  // namespace kv
