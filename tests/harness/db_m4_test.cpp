#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/db.h"
#include "core/db_internal.h"
#include "core/sst.h"
#include "testing/manifest_builder.h"
#include "testing/sst_builder.h"
#include "testing/temp_dir.h"
#include "testing/wal_builder.h"

namespace kv {
namespace {

namespace fs = std::filesystem;

using test::kRawPut;
using test::RawManifest;
using test::RawManifestSst;
using test::RawRecord;
using test::RawSst;
using test::ReadBytesOrEmpty;
using test::WalFileHeaderBytes;

std::optional<std::string> Lookup(const DB& db, std::string_view key) {
    std::string out = "stale";
    const Status s = db.Get(key, out);
    if (s.code == Code::NotFound) {
        EXPECT_TRUE(out.empty());
        return std::nullopt;
    }
    EXPECT_TRUE(s.ok()) << s.ToString();
    return out;
}

// The SST version recorded in the footer (the last 12 bytes).
std::uint32_t SstVersion(const fs::path& path) {
    const std::string bytes = ReadBytesOrEmpty(path);
    return bytes.size() < 12
               ? 0
               : static_cast<std::uint32_t>(
                     test::ReadLE(bytes, bytes.size() - 12, 4));
}

class DBM4Test : public ::testing::Test {
protected:
    fs::path Db() const { return tmp_.path() / "db"; }

    std::unique_ptr<DB> MustOpen(std::size_t trigger,
                                 std::uint32_t bits_per_key = 10,
                                 bool bypass = false,
                                 std::size_t memtable_bytes = 1,
                                 SstLimits limits = {}) {
        Options options;
        options.memtable_bytes = memtable_bytes;
        InternalOptions internal;
        internal.compaction_trigger = trigger;
        internal.bloom_bits_per_key = bits_per_key;
        internal.bypass_bloom_queries = bypass;
        internal.sst_limits = limits;
        std::unique_ptr<DB> db;
        const Status s = DBOpener::Open(Db(), options, internal,
                                        DefaultPosixSyscalls(), db);
        EXPECT_TRUE(s.ok()) << s.ToString();
        return db;
    }

    RawManifest Manifest() const {
        RawManifest m;
        EXPECT_TRUE(test::ReadManifest(Db(), m));
        return m;
    }

    std::vector<std::uint32_t> Versions() const {
        std::vector<std::uint32_t> out;
        for (const RawManifestSst& sst : Manifest().ssts) {
            out.push_back(SstVersion(Db() / test::RawSstName(sst.file_id)));
        }
        return out;
    }

    test::TempDir tmp_;
};

// Filterless version 1 SSTs (written with filters disabled) and version 2
// SSTs live together in one manifest version 2 database.
TEST_F(DBM4Test, FilterlessAndFilterBearingSstsCoexist) {
    {
        auto db = MustOpen(/*trigger=*/0, /*bits_per_key=*/0);
        ASSERT_TRUE(db->Put("a", "1").ok());
        ASSERT_TRUE(db->Put("b", "2").ok());  // flushes {a}: version 1
        ASSERT_TRUE(db->Delete("a").ok());    // flushes {b}: version 1
    }
    {
        auto db = MustOpen(/*trigger=*/0);
        ASSERT_TRUE(db->Put("c", "3").ok());  // flushes -a: version 2
        ASSERT_TRUE(db->Put("d", "4").ok());  // flushes {c}: version 2
    }
    EXPECT_EQ(Versions(), (std::vector<std::uint32_t>{1, 1, 2, 2}));
    for (const bool bypass : {false, true}) {
        SCOPED_TRACE(bypass);
        auto db = MustOpen(0, 10, bypass);
        EXPECT_EQ(Lookup(*db, "a"), std::nullopt);  // v2 tombstone over v1
        EXPECT_EQ(Lookup(*db, "b"), "2");           // from a v1 SST
        EXPECT_EQ(Lookup(*db, "c"), "3");
        EXPECT_EQ(Lookup(*db, "d"), "4");
        EXPECT_EQ(Lookup(*db, "x"), std::nullopt);
    }
    // A full compaction rewrites everything as version 2.
    {
        auto db = MustOpen(/*trigger=*/2);
        ASSERT_TRUE(db->Put("e", "5").ok());
    }
    for (const std::uint32_t v : Versions()) {
        EXPECT_EQ(v, 2u);
    }
    auto db = MustOpen(2);
    EXPECT_EQ(Lookup(*db, "a"), std::nullopt);
    EXPECT_EQ(Lookup(*db, "b"), "2");
    EXPECT_EQ(Lookup(*db, "e"), "5");
}

// A hand-built filterless SST (the M2 SST format) referenced by an M3/M4
// manifest version 2 opens and answers lookups next to new SSTs.
TEST_F(DBM4Test, IndependentlyBuiltVersion1SstOpensBesideVersion2) {
    fs::create_directory(Db());
    const std::string v1 = RawSst({{{"m", 1, kRawPut, "old"}}});
    ASSERT_TRUE(test::WriteBytes(Db() / "sst-000002.sst", v1));
    ASSERT_TRUE(test::WriteBytes(Db() / "wal-000003.log", WalFileHeaderBytes()));
    RawManifest m;
    m.generation = 2;
    m.next_file_id = 4;
    m.checkpoint = 1;
    m.active_wal_id = 3;
    m.ssts = {{2, v1.size(), 1, 1}};
    ASSERT_TRUE(test::WriteBytes(Db() / "MANIFEST", test::ManifestBytes(m)));

    auto db = MustOpen(/*trigger=*/0);
    EXPECT_EQ(Lookup(*db, "m"), "old");
    ASSERT_TRUE(db->Put("m", "new").ok());
    ASSERT_TRUE(db->Put("n", "x").ok());  // flushes m=new: version 2
    EXPECT_EQ(Versions(), (std::vector<std::uint32_t>{1, 2}));
    EXPECT_EQ(Lookup(*db, "m"), "new");
}

// Put -> flush -> Delete -> flush with compaction disabled, so both SSTs
// stay live: the tombstone's key is in the newer SST's filter, and the
// value never reappears, with or without filter queries, before and after
// reopen.
TEST_F(DBM4Test, FlushedTombstoneMasksOlderValueThroughFilters) {
    {
        auto db = MustOpen(/*trigger=*/0);
        ASSERT_TRUE(db->Put("k", "v").ok());
        ASSERT_TRUE(db->Delete("k").ok());   // flushes k=v
        ASSERT_TRUE(db->Put("z", "z").ok()); // flushes the tombstone
        EXPECT_EQ(Lookup(*db, "k"), std::nullopt);
    }
    const RawManifest m = Manifest();
    ASSERT_EQ(m.ssts.size(), 2u);
    // The tombstone SST: exactly the independent version 2 encoding, so
    // its filter holds "k".
    EXPECT_EQ(ReadBytesOrEmpty(Db() / test::RawSstName(m.ssts[1].file_id)),
              test::RawSstV2({{{"k", 2, test::kRawDelete, ""}}}));
    for (const bool bypass : {false, true}) {
        for (int round = 0; round < 2; ++round) {
            auto db = MustOpen(0, 10, bypass);
            EXPECT_EQ(Lookup(*db, "k"), std::nullopt);
            EXPECT_EQ(Lookup(*db, "z"), "z");
        }
    }
}

// Compaction into several outputs: each has its own filter covering exactly
// its keys, validated again at reopen.
TEST_F(DBM4Test, MultiOutputCompactionWritesAFilterPerOutput) {
    const std::string big(3000, 'v');
    const SstLimits one_block{1, kMaxSstIndexSize};
    {
        auto db = MustOpen(2, 10, false, 1, one_block);
        for (const char* key : {"a", "b", "c", "d"}) {
            ASSERT_TRUE(db->Put(key, big).ok());
        }
        ASSERT_TRUE(db->Delete("b").ok());
        ASSERT_TRUE(db->Put("e", "s").ok());
    }
    const RawManifest m = Manifest();
    ASSERT_GE(m.base_run_count, 2u);
    std::uint64_t keys = 0;
    for (std::size_t i = 0; i < m.base_run_count; ++i) {
        const fs::path path = Db() / test::RawSstName(m.ssts[i].file_id);
        EXPECT_EQ(SstVersion(path), 2u);
        std::unique_ptr<SstTable> table;
        ASSERT_TRUE(SstTable::Open(path, m.ssts[i].file_size,
                                   DefaultPosixSyscalls(), table)
                        .ok());
        keys += table->summary().filter.key_count();
    }
    EXPECT_EQ(keys, 3u);  // a, c, d survive in the base run
    auto db = MustOpen(2, 10, false, 1, one_block);
    EXPECT_EQ(Lookup(*db, "a"), big);
    EXPECT_EQ(Lookup(*db, "b"), std::nullopt);
    EXPECT_EQ(Lookup(*db, "d"), big);
    EXPECT_EQ(Lookup(*db, "e"), "s");
}

// The filter key limit (lowered to 3): compaction starts a new output
// before an SST would hold a fourth key, every output keeps a valid filter,
// and lookups and reopen see every key.
TEST_F(DBM4Test, CompactionRollsOverAtTheFilterKeyLimit) {
    SstLimits limits;
    limits.max_filter_keys = 3;
    // About 36 bytes per entry: the 4th mutation flushes the first 3 keys.
    const std::size_t memtable = 100;
    {
        auto db = MustOpen(2, 10, false, memtable, limits);
        for (int i = 0; i < 10; ++i) {
            ASSERT_TRUE(db->Put("k" + std::to_string(10 + i), "v").ok());
        }
    }
    const RawManifest m = Manifest();
    ASSERT_GE(m.base_run_count, 3u);
    std::uint64_t keys = 0;
    for (const RawManifestSst& sst : m.ssts) {
        const fs::path path = Db() / test::RawSstName(sst.file_id);
        std::unique_ptr<SstTable> table;
        ASSERT_TRUE(SstTable::Open(path, sst.file_size, DefaultPosixSyscalls(),
                                   table)
                        .ok());
        EXPECT_LE(table->summary().filter.key_count(), 3u);
        keys += table->summary().filter.key_count();
    }
    EXPECT_EQ(keys, 9u);  // the 10th key is still in the WAL
    auto db = MustOpen(2, 10, false, memtable, limits);
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(Lookup(*db, "k" + std::to_string(10 + i)), "v");
    }
    EXPECT_EQ(Lookup(*db, "k155"), std::nullopt);
}

// A flush cannot split: one SST beyond the filter key limit fails in F1
// before publication, and nothing acknowledged is lost.
TEST_F(DBM4Test, FlushOverTheFilterKeyLimitFailsClosed) {
    SstLimits limits;
    limits.max_filter_keys = 2;
    {
        auto db = MustOpen(0, 10, false, 1 << 20, limits);
        for (const char* key : {"a", "b", "c"}) {
            ASSERT_TRUE(db->Put(key, "v").ok());
        }
    }
    {
        auto db = MustOpen(0, 10, false, 1, limits);
        const Status s = db->Put("d", "v");  // flushes {a, b, c}
        EXPECT_EQ(s.code, Code::InvalidArgument) << s.ToString();
        EXPECT_NE(s.message.find("2 filter keys"), std::string::npos)
            << s.message;
        std::string out;
        EXPECT_EQ(db->Get("a", out).code, Code::NeedsRecovery);
    }
    EXPECT_TRUE(Manifest().ssts.empty());
    auto db = MustOpen(0, 10, false, 1 << 20, limits);
    for (const char* key : {"a", "b", "c"}) {
        EXPECT_EQ(Lookup(*db, key), "v");
    }
    EXPECT_EQ(Lookup(*db, "d"), std::nullopt);
}

// Filterless (version 1) output has no filter key limit: only the block
// and index limits apply (docs/sst-format.md section 3.7).
TEST_F(DBM4Test, FilterlessOutputIgnoresTheFilterKeyLimit) {
    SstLimits limits;
    limits.max_filter_keys = 2;
    {
        auto db = MustOpen(0, 0, false, 1 << 20, limits);
        for (const char* key : {"a", "b", "c", "d", "e"}) {
            ASSERT_TRUE(db->Put(key, "v").ok());
        }
    }
    auto db = MustOpen(0, 0, false, 1, limits);
    ASSERT_TRUE(db->Put("f", "v").ok());  // flushes five keys into one SST
    const RawManifest m = Manifest();
    ASSERT_EQ(m.ssts.size(), 1u);
    EXPECT_EQ(SstVersion(Db() / test::RawSstName(m.ssts[0].file_id)), 1u);
    for (const char* key : {"a", "c", "e", "f"}) {
        EXPECT_EQ(Lookup(*db, key), "v");
    }
}

// The counters of docs/m4-design.md section 4.
TEST_F(DBM4Test, CountersFollowTheirDefinitions) {
    auto db = MustOpen(/*trigger=*/0, 10, false, /*memtable_bytes=*/1 << 20);
    DBOpener::ResetStats(*db);
    ASSERT_TRUE(db->Put("a", "11").ok());
    ASSERT_TRUE(db->Delete("bb").ok());
    DbStats s = DBOpener::Stats(*db);
    EXPECT_EQ(s.mutations, 2u);
    EXPECT_EQ(s.user_bytes, 1u + 2 + 2);  // Put: key + value; Delete: key
    EXPECT_EQ(s.wal_bytes, RawRecord(1, kRawPut, "a", "11").size() +
                               RawRecord(2, test::kRawDelete, "bb", "").size());
    EXPECT_EQ(s.flushes, 0u);
    db.reset();

    // Every mutation now flushes first: the published SSTs and manifests
    // are counted once each, after their publication.
    db = MustOpen(0, 10, false, 1);
    DBOpener::ResetStats(*db);
    ASSERT_TRUE(db->Put("c", "3").ok());  // flushes {a, -bb}
    s = DBOpener::Stats(*db);
    const RawManifest m = Manifest();
    ASSERT_EQ(m.ssts.size(), 1u);
    EXPECT_EQ(s.flushes, 1u);
    EXPECT_EQ(s.flush_sst_bytes, m.ssts[0].file_size);
    EXPECT_EQ(s.metadata_bytes,
              WalFileHeaderBytes().size() + test::ManifestBytes(m).size());
    EXPECT_EQ(s.compaction_sst_bytes, 0u);

    // Lookups: one candidate SST; "a" is read, "zz" is outside its key
    // range, and the memtable answers "c" without any SST.
    DBOpener::ResetStats(*db);
    EXPECT_EQ(Lookup(*db, "a"), "11");
    EXPECT_EQ(Lookup(*db, "zz"), std::nullopt);
    EXPECT_EQ(Lookup(*db, "c"), "3");
    s = DBOpener::Stats(*db);
    EXPECT_EQ(s.gets, 3u);
    EXPECT_EQ(s.candidate_ssts, 2u);
    EXPECT_EQ(s.range_rejections, 1u);
    EXPECT_EQ(s.data_block_reads, 1u);
    EXPECT_GT(s.data_block_bytes, 0u);
    EXPECT_EQ(DBOpener::LiveSsts(*db), 1u);
    EXPECT_EQ(DBOpener::LiveRuns(*db), 1u);
}

// A missing key inside an SST's key range: the filter rejects it unless
// queries are bypassed, and both answers are the same.
TEST_F(DBM4Test, BypassChangesRequestsButNotAnswers) {
    {
        auto db = MustOpen(/*trigger=*/0, 10, false, 1 << 20);
        for (int i = 0; i < 200; i += 2) {
            ASSERT_TRUE(db->Put("k" + std::to_string(1000 + i), "v").ok());
        }
    }
    {
        auto db = MustOpen(0, 10, false, 1);
        ASSERT_TRUE(db->Put("zz", "z").ok());  // flushes the 100 keys
    }
    std::uint64_t reads[2] = {0, 0};
    std::uint64_t rejections[2] = {0, 0};
    for (const bool bypass : {false, true}) {
        auto db = MustOpen(0, 10, bypass, 1 << 20);
        DBOpener::ResetStats(*db);
        // k1001 .. k1197: strictly inside [k1000, k1198].
        for (int i = 1; i < 198; i += 2) {
            EXPECT_EQ(Lookup(*db, "k" + std::to_string(1000 + i)),
                      std::nullopt);
        }
        const DbStats s = DBOpener::Stats(*db);
        EXPECT_EQ(s.candidate_ssts, 99u);
        EXPECT_EQ(s.range_rejections, 0u);
        reads[bypass] = s.data_block_reads;
        rejections[bypass] = s.filter_rejections;
    }
    EXPECT_EQ(rejections[1], 0u);
    EXPECT_EQ(reads[1], 99u);
    EXPECT_GE(rejections[0], 89u);  // about 1% false positives expected
    EXPECT_EQ(reads[0] + rejections[0], 99u);
}

}  // namespace
}  // namespace kv
