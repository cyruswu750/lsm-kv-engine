#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "core/db.h"
#include "core/db_internal.h"
#include "core/file_lock.h"
#include "testing/fault_syscalls.h"
#include "testing/manifest_builder.h"
#include "testing/sst_builder.h"
#include "testing/temp_dir.h"
#include "testing/wal_builder.h"

namespace kv {
namespace {

namespace fs = std::filesystem;

using test::FaultSyscalls;
using test::kRawDelete;
using test::kRawPut;
using test::RawManifest;
using test::RawManifestSst;
using test::RawRecord;
using test::RawSst;
using test::RawSstV2;
using test::ReadBytesOrEmpty;
using test::WalFileHeaderBytes;

bool Contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

std::vector<std::string> Entries(const fs::path& dir) {
    std::vector<std::string> names;
    for (const auto& entry : fs::directory_iterator(dir)) {
        names.push_back(entry.path().filename().string());
    }
    std::sort(names.begin(), names.end());
    return names;
}

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

// Compaction (docs/m3-design.md section 4). With memtable_bytes = 1 every
// mutation that finds a nonempty memtable flushes first, and with trigger
// 2 the second live SST is compacted at once, so IDs are predictable:
//   Put a         -> WAL only (wal 1)
//   Put b         -> flush {a}: sst 2, wal 3                (generation 2)
//   Put c         -> flush {b}: sst 4, wal 5                (generation 3)
//                    compact {2, 4}: sst 6                  (generation 4)
class DBM3Test : public ::testing::Test {
protected:
    fs::path Db() const { return tmp_.path() / "db"; }

    Status OpenWith(PosixSyscalls& sys,
                    std::unique_ptr<DB>& db,
                    std::size_t trigger = 2,
                    std::size_t memtable_bytes = 1,
                    SstLimits limits = {}) {
        Options options;
        options.memtable_bytes = memtable_bytes;
        InternalOptions internal;
        internal.compaction_trigger = trigger;
        internal.sst_limits = limits;
        return DBOpener::Open(Db(), options, internal, sys, db);
    }

    std::unique_ptr<DB> MustOpen(std::size_t trigger = 2,
                                 std::size_t memtable_bytes = 1) {
        std::unique_ptr<DB> db;
        const Status s =
            OpenWith(DefaultPosixSyscalls(), db, trigger, memtable_bytes);
        EXPECT_TRUE(s.ok()) << s.ToString();
        return db;
    }

    RawManifest Manifest() const {
        RawManifest m;
        EXPECT_TRUE(test::ReadManifest(Db(), m));
        return m;
    }

    void ExpectLockReleased() {
        std::unique_ptr<FileLock> lock;
        EXPECT_TRUE(LockFile(Db() / "LOCK", lock).ok());
    }

    test::TempDir tmp_;
};

TEST_F(DBM3Test, CompactionPublishesOneOutputAndRetiresInputs) {
    auto db = MustOpen();
    ASSERT_TRUE(db->Put("a", "1").ok());
    ASSERT_TRUE(db->Put("b", "2").ok());
    EXPECT_EQ(Manifest().ssts.size(), 1u);
    ASSERT_TRUE(db->Put("c", "3").ok());

    EXPECT_EQ(Entries(Db()), (std::vector<std::string>{
                                 "LOCK", "MANIFEST", "sst-000006.sst",
                                 "wal-000005.log"}));
    const RawManifest m = Manifest();
    EXPECT_EQ(m.generation, 4u);
    EXPECT_EQ(m.next_file_id, 7u);
    // The flush of {b} set the checkpoint; compaction keeps it and the WAL.
    EXPECT_EQ(m.checkpoint, 2u);
    EXPECT_EQ(m.active_wal_id, 5u);
    EXPECT_EQ(m.base_run_count, 1u);
    const std::string expected =
        RawSstV2({{{"a", 1, kRawPut, "1"}, {"b", 2, kRawPut, "2"}}});
    ASSERT_EQ(m.ssts.size(), 1u);
    EXPECT_EQ(m.ssts[0].file_id, 6u);
    EXPECT_EQ(m.ssts[0].file_size, expected.size());
    EXPECT_EQ(m.ssts[0].min_sequence, 1u);
    EXPECT_EQ(m.ssts[0].max_sequence, 2u);
    EXPECT_EQ(ReadBytesOrEmpty(Db() / "sst-000006.sst"), expected);
    EXPECT_EQ(ReadBytesOrEmpty(Db() / "wal-000005.log"),
              WalFileHeaderBytes() + RawRecord(3, kRawPut, "c", "3"));

    for (const char* key : {"a", "b", "c"}) {
        EXPECT_TRUE(Lookup(*db, key).has_value()) << key;
    }
    db.reset();
    db = MustOpen();
    EXPECT_EQ(Lookup(*db, "a"), "1");
    EXPECT_EQ(Lookup(*db, "c"), "3");
}

// Shadowed versions and winning tombstones are dropped; the output's
// sequence range comes from the emitted entries only.
TEST_F(DBM3Test, CompactionDropsShadowedVersionsAndWinningTombstones) {
    auto db = MustOpen();
    ASSERT_TRUE(db->Put("a", "1").ok());   // seq 1
    ASSERT_TRUE(db->Put("k", "v1").ok());  // seq 2; flushes a
    ASSERT_TRUE(db->Put("k", "v2").ok());  // seq 3; flushes k=v1, compacts
    ASSERT_TRUE(db->Delete("a").ok());     // seq 4; flushes k=v2, compacts
    ASSERT_TRUE(db->Put("z", "z").ok());   // seq 5; flushes -a, compacts

    const RawManifest m = Manifest();
    ASSERT_EQ(m.ssts.size(), 1u);
    const std::string expected = RawSstV2({{{"k", 3, kRawPut, "v2"}}});
    EXPECT_EQ(ReadBytesOrEmpty(Db() / test::RawSstName(m.ssts[0].file_id)),
              expected);
    EXPECT_EQ(m.ssts[0].min_sequence, 3u);
    EXPECT_EQ(m.ssts[0].max_sequence, 3u);
    EXPECT_EQ(m.checkpoint, 4u);

    EXPECT_EQ(Lookup(*db, "a"), std::nullopt);
    EXPECT_EQ(Lookup(*db, "k"), "v2");
    EXPECT_EQ(Lookup(*db, "z"), "z");
}

// Put -> flush -> Delete -> flush -> compaction -> reopen: the value does
// not come back, and a later Put of the same key works normally.
TEST_F(DBM3Test, DeletedValueDoesNotResurrectAfterCompaction) {
    {
        auto db = MustOpen();
        ASSERT_TRUE(db->Put("k", "old").ok());  // seq 1
        ASSERT_TRUE(db->Delete("k").ok());      // seq 2; flushes the Put
        ASSERT_TRUE(db->Put("x", "1").ok());    // flushes -k, compacts
        EXPECT_EQ(Lookup(*db, "k"), std::nullopt);
    }
    // The tombstone and the value it shadowed are both gone.
    const RawManifest m = Manifest();
    EXPECT_TRUE(m.ssts.empty());
    EXPECT_EQ(m.checkpoint, 2u);
    for (int round = 0; round < 2; ++round) {
        auto db = MustOpen();
        EXPECT_EQ(Lookup(*db, "k"), std::nullopt);
        EXPECT_EQ(Lookup(*db, "x"), "1");
    }
    {
        auto db = MustOpen();
        ASSERT_TRUE(db->Put("k", "new").ok());
        EXPECT_EQ(Lookup(*db, "k"), "new");
    }
    auto db = MustOpen();
    EXPECT_EQ(Lookup(*db, "k"), "new");
}

// Deleting every key leaves no SST; the checkpoint is kept, so sequences
// continue above it after reopen instead of restarting at 1.
TEST_F(DBM3Test, AllDeletedLeavesNoSstAndSequencesContinue) {
    {
        auto db = MustOpen();
        ASSERT_TRUE(db->Put("a", "1").ok());  // seq 1
        ASSERT_TRUE(db->Put("b", "2").ok());  // seq 2; flushes a
        ASSERT_TRUE(db->Delete("a").ok());    // seq 3; flushes b, compacts
        ASSERT_TRUE(db->Delete("b").ok());    // seq 4; flushes -a, compacts
        ASSERT_TRUE(db->Put("c", "3").ok());  // seq 5; flushes -b, compacts
    }
    const RawManifest m = Manifest();
    EXPECT_TRUE(m.ssts.empty());
    EXPECT_EQ(m.checkpoint, 4u);
    EXPECT_EQ(m.active_wal_id, 11u);
    EXPECT_EQ(m.next_file_id, 12u);  // the empty compaction took no ID
    EXPECT_EQ(Entries(Db()), (std::vector<std::string>{"LOCK", "MANIFEST",
                                                       "wal-000011.log"}));

    auto db = MustOpen();
    EXPECT_EQ(Lookup(*db, "a"), std::nullopt);
    EXPECT_EQ(Lookup(*db, "b"), std::nullopt);
    EXPECT_EQ(Lookup(*db, "c"), "3");
    ASSERT_TRUE(db->Put("d", "4").ok());  // flushes c (seq 5) first
    EXPECT_EQ(ReadBytesOrEmpty(Db() / "wal-000013.log"),
              WalFileHeaderBytes() + RawRecord(6, kRawPut, "d", "4"));
    db.reset();
    db = MustOpen();
    EXPECT_EQ(Lookup(*db, "c"), "3");
    EXPECT_EQ(Lookup(*db, "d"), "4");
}

// Compaction runs before a mutation even without a flush, and versions in
// the memtable and WAL stay newer than the output.
TEST_F(DBM3Test, NewerMemtableVersionsShadowTheCompactedOutput) {
    {
        auto db = MustOpen(/*trigger=*/0);
        ASSERT_TRUE(db->Put("k", "v1").ok());  // seq 1
        ASSERT_TRUE(db->Put("a", "x").ok());   // seq 2; flushes k=v1
        ASSERT_TRUE(db->Put("k", "v2").ok());  // seq 3; flushes a
    }
    ASSERT_EQ(Manifest().ssts.size(), 2u);

    auto db = MustOpen(/*trigger=*/2, /*memtable_bytes=*/1 << 20);
    EXPECT_EQ(Lookup(*db, "k"), "v2");  // replayed into the memtable
    ASSERT_TRUE(db->Put("b", "y").ok());  // no flush; compacts first
    RawManifest m = Manifest();
    ASSERT_EQ(m.ssts.size(), 1u);
    EXPECT_EQ(m.ssts[0].min_sequence, 1u);
    EXPECT_EQ(m.ssts[0].max_sequence, 2u);
    EXPECT_EQ(m.checkpoint, 2u);
    EXPECT_EQ(m.active_wal_id, 5u);
    EXPECT_EQ(Lookup(*db, "k"), "v2");
    db.reset();
    db = MustOpen(/*trigger=*/2, /*memtable_bytes=*/1 << 20);
    EXPECT_EQ(Lookup(*db, "k"), "v2");
    EXPECT_EQ(Lookup(*db, "a"), "x");
    EXPECT_EQ(Lookup(*db, "b"), "y");
}

TEST_F(DBM3Test, TriggerZeroDisablesAndOneIsRejected) {
    {
        std::unique_ptr<DB> db;
        EXPECT_EQ(OpenWith(DefaultPosixSyscalls(), db, /*trigger=*/1).code,
                  Code::InvalidArgument);
        EXPECT_EQ(db, nullptr);
        EXPECT_FALSE(fs::exists(Db()));
    }
    auto db = MustOpen(/*trigger=*/0);
    for (int i = 0; i < 7; ++i) {
        ASSERT_TRUE(db->Put("k" + std::to_string(i), "v").ok());
    }
    EXPECT_EQ(Manifest().ssts.size(), 6u);

    // The default trigger (4) compacts once four SSTs are live.
    db.reset();
    db = MustOpen(InternalOptions{}.compaction_trigger);
    ASSERT_TRUE(db->Put("last", "v").ok());
    EXPECT_EQ(Manifest().ssts.size(), 1u);
    for (int i = 0; i < 7; ++i) {
        EXPECT_EQ(Lookup(*db, "k" + std::to_string(i)), "v");
    }
}

// ---- Multi-output compaction (docs/m3-design.md sections 4 and 5) ------------

const std::string kBig(3000, 'v');  // two such entries never share a block

// Lowered writer limits make the split points small and exact: at most
// two blocks per output SST.
const SstLimits kTwoBlocks{2, kMaxSstIndexSize};

TEST_F(DBM3Test, CompactionSplitsOutputAtExactBoundariesIntoOneBaseRun) {
    std::unique_ptr<DB> db;
    ASSERT_TRUE(OpenWith(DefaultPosixSyscalls(), db, 2, 1, kTwoBlocks).ok());
    // Keys arrive out of key order, so the outputs' sequence ranges overlap.
    ASSERT_TRUE(db->Put("m", kBig).ok());  // seq 1
    ASSERT_TRUE(db->Put("a", kBig).ok());  // seq 2; flushes m
    ASSERT_TRUE(db->Put("z", kBig).ok());  // seq 3; flushes a; compacts
    ASSERT_TRUE(db->Put("b", kBig).ok());  // seq 4; flushes z; compacts
    ASSERT_TRUE(db->Put("c", "s").ok());   // seq 5; flushes b; compacts

    const RawManifest m = Manifest();
    EXPECT_EQ(m.version, 2u);
    ASSERT_EQ(m.base_run_count, 2u);
    ASSERT_EQ(m.ssts.size(), 2u);
    // {a, b, m, z}: each entry fills a block, so each output holds two.
    EXPECT_EQ(ReadBytesOrEmpty(Db() / test::RawSstName(m.ssts[0].file_id)),
              RawSstV2({{{"a", 2, kRawPut, kBig}}, {{"b", 4, kRawPut, kBig}}}));
    EXPECT_EQ(ReadBytesOrEmpty(Db() / test::RawSstName(m.ssts[1].file_id)),
              RawSstV2({{{"m", 1, kRawPut, kBig}}, {{"z", 3, kRawPut, kBig}}}));
    // Disjoint keys, overlapping sequence intervals [2, 4] and [1, 3].
    EXPECT_EQ(m.ssts[0].min_sequence, 2u);
    EXPECT_EQ(m.ssts[0].max_sequence, 4u);
    EXPECT_EQ(m.ssts[1].min_sequence, 1u);
    EXPECT_EQ(m.ssts[1].max_sequence, 3u);
    EXPECT_EQ(m.checkpoint, 4u);
    for (const char* key : {"a", "b", "m", "z"}) {
        EXPECT_EQ(Lookup(*db, key), kBig) << key;
    }
    EXPECT_EQ(Lookup(*db, "c"), "s");
    EXPECT_EQ(Lookup(*db, "n"), std::nullopt);  // between the outputs
    db.reset();

    ASSERT_TRUE(OpenWith(DefaultPosixSyscalls(), db, 2, 1 << 20, kTwoBlocks)
                    .ok());
    for (const char* key : {"a", "b", "m", "z"}) {
        EXPECT_EQ(Lookup(*db, key), kBig) << key;
    }
    EXPECT_EQ(Manifest().base_run_count, 2u);
}

// A two-SST base run is one logical run: with no new flush, mutations do
// not compact it again, before or after reopen. Counting SSTs would.
TEST_F(DBM3Test, UnchangedBaseRunIsNotCompactedAgainAfterReopen) {
    std::unique_ptr<DB> db;
    ASSERT_TRUE(OpenWith(DefaultPosixSyscalls(), db, 2, 1, kTwoBlocks).ok());
    for (const char* key : {"m", "a", "z", "b"}) {
        ASSERT_TRUE(db->Put(key, kBig).ok());
    }
    ASSERT_TRUE(db->Put("c", "s").ok());
    db.reset();
    const RawManifest before = Manifest();
    ASSERT_EQ(before.base_run_count, 2u);
    ASSERT_EQ(before.ssts.size(), 2u);

    for (int round = 0; round < 2; ++round) {
        // A large memtable: these mutations never flush.
        ASSERT_TRUE(
            OpenWith(DefaultPosixSyscalls(), db, 2, 1 << 20, kTwoBlocks).ok());
        for (int i = 0; i < 5; ++i) {
            ASSERT_TRUE(db->Put("k" + std::to_string(round * 5 + i), "v").ok());
        }
        db.reset();
        EXPECT_EQ(Manifest().generation, before.generation);
    }

    // One new flush makes a second run, and exactly one compaction follows.
    ASSERT_TRUE(OpenWith(DefaultPosixSyscalls(), db, 2, 1, kTwoBlocks).ok());
    ASSERT_TRUE(db->Put("q", "v").ok());  // flushes the ten keys, compacts
    EXPECT_EQ(Manifest().generation, before.generation + 2);
    EXPECT_GE(Manifest().base_run_count, 2u);
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(Lookup(*db, "k" + std::to_string(i)), "v");
    }
}

// Flush runs above a multi-SST base shadow it (overwrite and tombstone),
// keep the grouping across reopen, and the next full compaction drops the
// deleted key without resurrecting it.
TEST_F(DBM3Test, FlushRunsOverABaseRunShadowItAndKeepTheGrouping) {
    std::unique_ptr<DB> db;
    ASSERT_TRUE(OpenWith(DefaultPosixSyscalls(), db, 2, 1, kTwoBlocks).ok());
    for (const char* key : {"m", "a", "z", "b"}) {
        ASSERT_TRUE(db->Put(key, kBig).ok());
    }
    ASSERT_TRUE(db->Put("c", "s").ok());
    db.reset();
    ASSERT_EQ(Manifest().base_run_count, 2u);

    // Compaction disabled: flush runs accumulate above the base.
    ASSERT_TRUE(OpenWith(DefaultPosixSyscalls(), db, 0, 1, kTwoBlocks).ok());
    ASSERT_TRUE(db->Put("a", "A2").ok());  // flushes c
    ASSERT_TRUE(db->Delete("m").ok());     // flushes a=A2
    ASSERT_TRUE(db->Put("y", "Y").ok());   // flushes -m
    for (int round = 0; round < 2; ++round) {
        const RawManifest m = Manifest();
        EXPECT_EQ(m.base_run_count, 2u);
        EXPECT_EQ(m.ssts.size(), 5u);
        EXPECT_EQ(Lookup(*db, "a"), "A2");
        EXPECT_EQ(Lookup(*db, "m"), std::nullopt);
        EXPECT_EQ(Lookup(*db, "b"), kBig);
        EXPECT_EQ(Lookup(*db, "z"), kBig);
        EXPECT_EQ(Lookup(*db, "y"), "Y");
        db.reset();
        ASSERT_TRUE(
            OpenWith(DefaultPosixSyscalls(), db, 0, 1, kTwoBlocks).ok());
    }
    db.reset();

    // Four runs reach the trigger: one compaction over every input.
    ASSERT_TRUE(OpenWith(DefaultPosixSyscalls(), db, 2, 1, kTwoBlocks).ok());
    ASSERT_TRUE(db->Put("last", "L").ok());
    const RawManifest m = Manifest();
    EXPECT_EQ(m.ssts.size(), m.base_run_count);  // only the new base run
    for (const RawManifestSst& sst : m.ssts) {
        const std::string bytes =
            ReadBytesOrEmpty(Db() / test::RawSstName(sst.file_id));
        EXPECT_EQ(bytes.find(std::string("\x01m", 2)), std::string::npos);
    }
    db.reset();
    ASSERT_TRUE(OpenWith(DefaultPosixSyscalls(), db, 2, 1, kTwoBlocks).ok());
    EXPECT_EQ(Lookup(*db, "m"), std::nullopt);
    EXPECT_EQ(Lookup(*db, "a"), "A2");
    EXPECT_EQ(Lookup(*db, "last"), "L");
}

// A maximum legal entry, the empty key, and binary keys across outputs of
// one block each; deleting them all leaves no SST and the checkpoint.
TEST_F(DBM3Test, MaximumEntryAndEmptyKeysSplitAndDeleteCleanly) {
    const SstLimits one_block{1, kMaxSstIndexSize};
    const std::string max_key(kMaxKeyLength, '\x80');
    const std::string max_value(kMaxValueLength, 'x');
    const std::string bin("\x00\xff", 2);
    std::unique_ptr<DB> db;
    ASSERT_TRUE(OpenWith(DefaultPosixSyscalls(), db, 2, 1, one_block).ok());
    ASSERT_TRUE(db->Put("", "empty").ok());
    ASSERT_TRUE(db->Put(max_key, max_value).ok());
    ASSERT_TRUE(db->Put(bin, "b").ok());  // compacts {"", max}
    ASSERT_TRUE(db->Put("\xff", kBig).ok());
    RawManifest m = Manifest();
    EXPECT_GE(m.base_run_count, 2u);
    EXPECT_EQ(Lookup(*db, ""), "empty");
    EXPECT_TRUE(Lookup(*db, max_key) == max_value);
    EXPECT_EQ(Lookup(*db, bin), "b");
    db.reset();
    ASSERT_TRUE(OpenWith(DefaultPosixSyscalls(), db, 2, 1, one_block).ok());
    EXPECT_TRUE(Lookup(*db, max_key) == max_value);

    // The maximum entry has an output to itself.
    bool found = false;
    for (const RawManifestSst& sst : Manifest().ssts) {
        if (sst.file_size > kMaxValueLength) {
            found = true;
            EXPECT_EQ(ReadBytesOrEmpty(Db() / test::RawSstName(sst.file_id))
                          .size(),
                      sst.file_size);
        }
    }
    EXPECT_TRUE(found);

    const std::uint64_t checkpoint_before = Manifest().checkpoint;
    for (const std::string& key : {std::string(""), max_key, bin,
                                   std::string("\xff")}) {
        ASSERT_TRUE(db->Delete(key).ok());
    }
    // Flushes the last tombstone, then compacts everything to nothing.
    ASSERT_TRUE(db->Delete("never").ok());
    m = Manifest();
    EXPECT_EQ(m.base_run_count, 0u);
    EXPECT_TRUE(m.ssts.empty());
    EXPECT_GE(m.checkpoint, checkpoint_before);
    ASSERT_TRUE(db->Put("", "again").ok());
    db.reset();
    ASSERT_TRUE(OpenWith(DefaultPosixSyscalls(), db, 2, 1, one_block).ok());
    EXPECT_EQ(Lookup(*db, ""), "again");
    EXPECT_EQ(Lookup(*db, max_key), std::nullopt);
    EXPECT_EQ(Lookup(*db, bin), std::nullopt);
}

// On-disk base runs that break the run rules are Corruption at Open.
TEST_F(DBM3Test, MalformedBaseRunsFailOpen) {
    struct Sst {
        std::uint64_t id;
        std::string bytes;
        std::uint64_t min_seq;
        std::uint64_t max_seq;
    };
    auto sst = [](std::uint64_t id, std::vector<test::RawSstEntry> entries) {
        std::uint64_t lo = ~std::uint64_t{0}, hi = 0;
        for (const auto& e : entries) {
            lo = std::min(lo, e.sequence);
            hi = std::max(hi, e.sequence);
        }
        return Sst{id, RawSst({entries}), lo, hi};
    };
    auto make = [&](const std::vector<Sst>& ssts, std::uint32_t base) {
        fs::remove_all(Db());
        fs::create_directory(Db());
        RawManifest m;
        m.generation = 3;
        m.next_file_id = 10;
        m.checkpoint = 9;
        m.active_wal_id = 9;
        m.base_run_count = base;
        for (const Sst& s : ssts) {
            ASSERT_TRUE(test::WriteBytes(Db() / test::RawSstName(s.id),
                                         s.bytes));
            m.ssts.push_back({s.id, s.bytes.size(), s.min_seq, s.max_seq});
        }
        ASSERT_TRUE(test::WriteBytes(Db() / "wal-000009.log",
                                     WalFileHeaderBytes()));
        ASSERT_TRUE(
            test::WriteBytes(Db() / "MANIFEST", test::ManifestBytes(m)));
    };
    auto open_code = [&]() {
        std::unique_ptr<DB> db;
        const Status s = OpenWith(DefaultPosixSyscalls(), db);
        return std::make_pair(s.code, s.message);
    };
    const Sst low = sst(2, {{"a", 3, kRawPut, "1"}, {"c", 4, kRawPut, "2"}});
    const Sst high = sst(3, {{"m", 1, kRawPut, "3"}, {"z", 2, kRawPut, "4"}});
    const Sst overlapping =
        sst(3, {{"b", 1, kRawPut, "3"}, {"z", 2, kRawPut, "4"}});

    // Valid: key-ordered and disjoint, with inverted sequence intervals.
    make({low, high}, 2);
    {
        std::unique_ptr<DB> db;
        ASSERT_TRUE(OpenWith(DefaultPosixSyscalls(), db).ok());
        EXPECT_EQ(Lookup(*db, "c"), "2");
        EXPECT_EQ(Lookup(*db, "m"), "3");
    }
    make({low, overlapping}, 2);
    auto r = open_code();
    EXPECT_EQ(r.first, Code::Corruption) << r.second;
    EXPECT_TRUE(Contains(r.second, "base run SST")) << r.second;
    make({high, low}, 2);  // key order reversed
    r = open_code();
    EXPECT_EQ(r.first, Code::Corruption) << r.second;
    EXPECT_TRUE(Contains(r.second, "base run SST")) << r.second;
    make({low, high}, 3);  // count above the SST count
    r = open_code();
    EXPECT_EQ(r.first, Code::Corruption) << r.second;
    EXPECT_TRUE(Contains(r.second, "base run count 3")) << r.second;
    make({low, high}, 1);  // the second is then a flush run, not newer
    r = open_code();
    EXPECT_EQ(r.first, Code::Corruption) << r.second;
    EXPECT_TRUE(Contains(r.second, "does not follow the base run"))
        << r.second;
    make({low, high}, 2);
    fs::remove(Db() / "sst-000003.sst");  // a missing referenced output
    r = open_code();
    EXPECT_EQ(r.first, Code::Corruption) << r.second;
    EXPECT_TRUE(Contains(r.second, "missing")) << r.second;
}

// docs/sst-format.md section 6: capacity limits during compaction.
TEST_F(DBM3Test, CompactionExhaustionFailsBeforePublication) {
    const SstLimits one_block{1, kMaxSstIndexSize};
    auto make = [&](std::uint64_t next_file_id) {
        fs::remove_all(Db());
        fs::create_directory(Db());
        RawManifest m;
        m.generation = 4;
        m.next_file_id = next_file_id;
        m.checkpoint = 3;
        m.active_wal_id = 1;
        std::uint64_t seq = 1;
        for (const char* key : {"a", "b", "c"}) {
            const std::string bytes = RawSst({{{key, seq, kRawPut, kBig}}});
            const std::uint64_t id = seq + 1;
            ASSERT_TRUE(test::WriteBytes(Db() / test::RawSstName(id), bytes));
            m.ssts.push_back({id, bytes.size(), seq, seq});
            ++seq;
        }
        ASSERT_TRUE(test::WriteBytes(Db() / "wal-000001.log",
                                     WalFileHeaderBytes()));
        ASSERT_TRUE(
            test::WriteBytes(Db() / "MANIFEST", test::ManifestBytes(m)));
    };
    auto expect_intact = [&](std::uint64_t generation) {
        EXPECT_EQ(Manifest().generation, generation);
        std::unique_ptr<DB> db;
        ASSERT_TRUE(
            OpenWith(DefaultPosixSyscalls(), db, 0, 1 << 20, one_block).ok());
        for (const char* key : {"a", "b", "c"}) {
            EXPECT_EQ(Lookup(*db, key), kBig) << key;
        }
        EXPECT_EQ(Lookup(*db, "d"), std::nullopt);
    };

    // File IDs: one is left, but three one-block outputs are needed. The
    // first output takes it; the second finds none.
    make(~std::uint64_t{0} - 1);
    {
        std::unique_ptr<DB> db;
        ASSERT_TRUE(
            OpenWith(DefaultPosixSyscalls(), db, 2, 1 << 20, one_block).ok());
        const Status s = db->Put("d", "4");
        EXPECT_EQ(s.code, Code::InvalidArgument) << s.ToString();
        EXPECT_TRUE(Contains(s.message, "file IDs are exhausted"))
            << s.message;
        std::string out;
        EXPECT_EQ(db->Get("a", out).code, Code::NeedsRecovery);
    }
    expect_intact(4);

    // Manifest entries: with a limit of 3 and trigger 2, an output may use
    // 3 - (2 - 1) = 2 SSTs, leaving room for the next flush; 3 are needed.
    make(5);
    {
        Options options;
        options.memtable_bytes = 1 << 20;
        InternalOptions internal;
        internal.compaction_trigger = 2;
        internal.sst_limits = one_block;
        internal.max_manifest_ssts = 3;
        std::unique_ptr<DB> db;
        ASSERT_TRUE(DBOpener::Open(Db(), options, internal,
                                   DefaultPosixSyscalls(), db)
                        .ok());
        const Status s = db->Put("d", "4");
        EXPECT_EQ(s.code, Code::InvalidArgument) << s.ToString();
        EXPECT_TRUE(Contains(s.message, "needs more than 2 SSTs"))
            << s.message;

        internal.compaction_trigger = 4;  // above the limit of 3
        std::unique_ptr<DB> other;
        db.reset();
        EXPECT_EQ(DBOpener::Open(Db(), options, internal,
                                 DefaultPosixSyscalls(), other)
                      .code,
                  Code::InvalidArgument);
    }
    expect_intact(4);
}

// A flush into a full manifest is rejected before any I/O.
TEST_F(DBM3Test, FlushIntoAFullManifestIsRejectedWithoutIo) {
    Options options;
    options.memtable_bytes = 1;
    InternalOptions internal;
    internal.compaction_trigger = 0;
    internal.max_manifest_ssts = 2;
    FaultSyscalls sys;
    std::unique_ptr<DB> db;
    ASSERT_TRUE(DBOpener::Open(Db(), options, internal, sys, db).ok());
    ASSERT_TRUE(db->Put("a", "1").ok());
    ASSERT_TRUE(db->Put("b", "2").ok());
    ASSERT_TRUE(db->Put("c", "3").ok());
    ASSERT_EQ(Manifest().ssts.size(), 2u);
    sys.armed = true;
    const Status s = db->Put("d", "4");
    EXPECT_EQ(s.code, Code::InvalidArgument) << s.ToString();
    EXPECT_EQ(sys.TotalCalls(), 0u);
    EXPECT_EQ(Lookup(*db, "c"), "3");  // still usable
}

// ---- The M2/M3 manifest format boundary (docs/sst-format.md 4.3) -----------

// Files written by the M2 release (v0.2-m2): Open(create), Put("a", "1"),
// Put("b", "2") with memtable_bytes = 1, which flushed {a}. The directory
// also held an empty LOCK.
// MANIFEST (92 bytes)
constexpr char kM2Manifest[] =
    "\x4b\x56\x4d\x41\x4e\x30\x30\x31\x01\x00\x00\x00\x00\x00\x00\x00"
    "\x02\x00\x00\x00\x00\x00\x00\x00\x04\x00\x00\x00\x00\x00\x00\x00"
    "\x01\x00\x00\x00\x00\x00\x00\x00\x03\x00\x00\x00\x00\x00\x00\x00"
    "\x01\x00\x00\x00\x00\x00\x00\x00\x02\x00\x00\x00\x00\x00\x00\x00"
    "\x64\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00"
    "\x01\x00\x00\x00\x00\x00\x00\x00\xd1\x7b\x4e\x82";
// sst-000002.sst (100 bytes)
constexpr char kM2Sst[] =
    "\x01\x00\x00\x00\x01\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00"
    "\x01\x61\x31\x01\x00\x00\x00\x1c\xe3\x78\x91\x01\x00\x00\x00\x00"
    "\x00\x00\x00\x00\x00\x00\x00\x1b\x00\x00\x00\x61\x01\x00\x00\x00"
    "\xf0\x6a\x2a\xde\x72\xc9\x94\x46\x1b\x00\x00\x00\x00\x00\x00\x00"
    "\x19\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
    "\x00\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x4b\x56\x53\x53"
    "\x54\x30\x30\x31";
// wal-000003.log (46 bytes)
constexpr char kM2Wal[] =
    "\x4b\x56\x57\x41\x4c\x30\x30\x31\x01\x00\x00\x00\x00\x00\x00\x00"
    "\x4e\x23\x06\xcd\x7c\x5b\x60\x87\x02\x00\x00\x00\x00\x00\x00\x00"
    "\x01\x00\x00\x00\x01\x00\x00\x00\x01\x00\x00\x00\x62\x32";

TEST_F(DBM3Test, M2DatabaseIsUnsupportedAndLeftUnchanged) {
    const std::vector<std::pair<std::string, std::string>> files = {
        {"LOCK", ""},
        {"MANIFEST", std::string(kM2Manifest, sizeof(kM2Manifest) - 1)},
        {"sst-000002.sst", std::string(kM2Sst, sizeof(kM2Sst) - 1)},
        {"wal-000003.log", std::string(kM2Wal, sizeof(kM2Wal) - 1)},
    };
    // The fixture is the M2 layout that the test builder writes as version 1.
    RawManifest v1;
    v1.version = 1;
    v1.generation = 2;
    v1.next_file_id = 4;
    v1.checkpoint = 1;
    v1.active_wal_id = 3;
    v1.ssts = {{2, files[2].second.size(), 1, 1}};
    EXPECT_EQ(test::ManifestBytes(v1), files[1].second);

    fs::create_directory(Db());
    for (const auto& [name, bytes] : files) {
        ASSERT_TRUE(test::WriteBytes(Db() / name, bytes));
    }
    for (const bool create : {false, true}) {
        FaultSyscalls sys;
        sys.armed = true;
        Options options;
        options.create_if_missing = create;
        std::unique_ptr<DB> db;
        const Status s = DBOpener::Open(Db(), options, sys, db);
        EXPECT_EQ(s.code, Code::UnsupportedFormat) << s.ToString();
        EXPECT_TRUE(Contains(s.message, "an M2 database")) << s.message;
        EXPECT_EQ(db, nullptr);
        // Nothing was written, truncated, synced, renamed, or removed.
        EXPECT_EQ(sys.writes + sys.ftruncates + sys.fsyncs + sys.renames +
                      sys.unlinks,
                  0u);
        EXPECT_EQ(Entries(Db()), (std::vector<std::string>{
                                     "LOCK", "MANIFEST", "sst-000002.sst",
                                     "wal-000003.log"}));
        for (const auto& [name, bytes] : files) {
            EXPECT_EQ(ReadBytesOrEmpty(Db() / name), bytes) << name;
        }
    }
}

// Every manifest M3 writes is version 2: new, single-SST, compacted, and
// emptied databases alike.
TEST_F(DBM3Test, M3AlwaysWritesManifestVersion2) {
    auto version = [&]() {
        const std::string bytes = ReadBytesOrEmpty(Db() / "MANIFEST");
        return bytes.size() < 12 ? 0u
                                 : static_cast<unsigned>(
                                       test::ReadLE(bytes, 8, 4));
    };
    auto db = MustOpen();
    EXPECT_EQ(version(), 2u);  // initialized, no SST
    ASSERT_TRUE(db->Put("a", "1").ok());
    ASSERT_TRUE(db->Put("b", "2").ok());  // one flushed SST
    EXPECT_EQ(version(), 2u);
    EXPECT_EQ(Manifest().base_run_count, 0u);
    ASSERT_TRUE(db->Put("c", "3").ok());  // compacted to one base SST
    EXPECT_EQ(version(), 2u);
    EXPECT_EQ(Manifest().base_run_count, 1u);
    ASSERT_TRUE(db->Delete("a").ok());
    ASSERT_TRUE(db->Delete("b").ok());
    ASSERT_TRUE(db->Delete("c").ok());
    ASSERT_TRUE(db->Put("d", "4").ok());  // the deletes compact to nothing
    EXPECT_EQ(version(), 2u);
    EXPECT_TRUE(Manifest().ssts.empty());
    EXPECT_EQ(Manifest().base_run_count, 0u);
}

// The B-006 flush limit end to end (docs/sst-format.md section 3.6): the
// flush fails in F1, the handle needs recovery, nothing acknowledged is
// lost, and every later flush of the replayed memtable fails the same way.
TEST_F(DBM3Test, FlushOverTheSstLimitFailsClosedAndRepeats) {
    const SstLimits one_block{1, kMaxSstIndexSize};
    const std::string big(3000, 'v');
    // Two 3,032-byte memtable entries reach the threshold together.
    const std::size_t threshold = 6000;
    {
        std::unique_ptr<DB> db;
        ASSERT_TRUE(OpenWith(DefaultPosixSyscalls(), db, 0, threshold,
                             one_block)
                        .ok());
        ASSERT_TRUE(db->Put("a", big).ok());
        ASSERT_TRUE(db->Put("b", big).ok());
        const Status s = db->Put("c", "3");  // flushes {a, b}: two blocks
        EXPECT_EQ(s.code, Code::InvalidArgument) << s.ToString();
        EXPECT_TRUE(Contains(s.message, "would exceed 1 blocks")) << s.message;
        std::string out;
        EXPECT_EQ(db->Get("a", out).code, Code::NeedsRecovery);
    }
    EXPECT_EQ(Manifest().generation, 1u);
    std::unique_ptr<DB> db;
    ASSERT_TRUE(
        OpenWith(DefaultPosixSyscalls(), db, 0, threshold, one_block).ok());
    EXPECT_EQ(Lookup(*db, "a"), big);
    EXPECT_EQ(Lookup(*db, "b"), big);
    EXPECT_EQ(Lookup(*db, "c"), std::nullopt);
    EXPECT_EQ(db->Put("c", "3").code, Code::InvalidArgument);
}

// Counter checks run before any compaction I/O; the mutation is rejected
// and the handle stays usable (docs/m3-design.md section 4, C0).
TEST_F(DBM3Test, CompactionCounterExhaustionRejectsWithoutIo) {
    const std::string s1 = RawSst({{{"a", 1, kRawPut, "1"}}});
    const std::string s2 = RawSst({{{"b", 2, kRawPut, "2"}}});
    for (const bool generations : {true, false}) {
        SCOPED_TRACE(generations ? "generation" : "file ID");
        fs::remove_all(Db());
        fs::create_directory(Db());
        RawManifest m;
        m.generation = generations ? ~std::uint64_t{0} : 5;
        m.next_file_id = generations ? 4 : ~std::uint64_t{0};
        m.checkpoint = 2;
        m.active_wal_id = 1;
        m.ssts = {{2, s1.size(), 1, 1}, {3, s2.size(), 2, 2}};
        ASSERT_TRUE(test::WriteBytes(Db() / "sst-000002.sst", s1));
        ASSERT_TRUE(test::WriteBytes(Db() / "sst-000003.sst", s2));
        ASSERT_TRUE(test::WriteBytes(Db() / "wal-000001.log",
                                     WalFileHeaderBytes()));
        ASSERT_TRUE(test::WriteBytes(Db() / "MANIFEST",
                                     test::ManifestBytes(m)));

        FaultSyscalls sys;
        std::unique_ptr<DB> db;
        ASSERT_TRUE(OpenWith(sys, db, 2, 1 << 20).ok());
        sys.armed = true;
        const Status s = db->Put("c", "3");
        EXPECT_EQ(s.code, Code::InvalidArgument) << s.ToString();
        EXPECT_TRUE(Contains(s.message, "exhausted")) << s.message;
        EXPECT_EQ(sys.TotalCalls(), 0u);
        EXPECT_EQ(Lookup(*db, "a"), "1");  // still usable
        EXPECT_EQ(db->Put("c", "3").code, Code::InvalidArgument);
        EXPECT_EQ(test::ReadManifest(Db(), m) ? m.ssts.size() : 0u, 2u);
    }
}

// ---- Publication and cleanup faults ------------------------------------------

class CompactionFaultTest : public DBM3Test {
protected:
    // Opens with sys, writes a and b (one live SST), then arms sys. The
    // next mutation flushes {b} and compacts SSTs 2 and 4 into SST 6.
    std::unique_ptr<DB> Prepare(FaultSyscalls& sys) {
        std::unique_ptr<DB> db;
        EXPECT_TRUE(OpenWith(sys, db).ok());
        EXPECT_TRUE(db->Put("a", "1").ok());
        EXPECT_TRUE(db->Put("b", "2").ok());
        sys.armed = true;
        return db;
    }

    void ExpectInvalidated(DB& db, const FaultSyscalls& sys) {
        const std::size_t calls = sys.TotalCalls();
        EXPECT_EQ(db.Put("x", "v").code, Code::NeedsRecovery);
        std::string out = "stale";
        EXPECT_EQ(db.Get("a", out).code, Code::NeedsRecovery);
        EXPECT_TRUE(out.empty());
        EXPECT_EQ(sys.TotalCalls(), calls);
    }

    // Reopens with real syscalls: every acknowledged value is present, the
    // failed mutation is absent, and the database keeps compacting.
    void ExpectRecovered(std::uint64_t generation) {
        auto db = MustOpen();
        EXPECT_EQ(Manifest().generation, generation);
        EXPECT_EQ(Lookup(*db, "a"), "1");
        EXPECT_EQ(Lookup(*db, "b"), "2");
        EXPECT_EQ(Lookup(*db, "c"), std::nullopt);
        ASSERT_TRUE(db->Put("c", "3").ok());
        ASSERT_TRUE(db->Put("d", "4").ok());
        db.reset();
        db = MustOpen();
        EXPECT_LE(Manifest().ssts.size(), 1u);
        for (const auto& [k, v] : std::vector<std::pair<std::string,
                                                        std::string>>{
                 {"a", "1"}, {"b", "2"}, {"c", "3"}, {"d", "4"}}) {
            EXPECT_EQ(Lookup(*db, k), v) << k;
        }
    }
};

struct CompactionFault {
    const char* step;
    const char* op;
    const char* name;
    std::size_t skip;
    std::uint64_t generation_after;
};

// Directory fsyncs in the triggering Put: F3, F5, F7 (old WAL removed),
// then C2, C4, C6. MANIFEST.tmp is written once by the flush first.
const CompactionFault kCompactionFaults[] = {
    {"C1", "pread", "sst-000002.sst", 0, 3},
    {"C1", "open", "sst-000006.sst.tmp", 0, 3},
    {"C1", "write", "sst-000006.sst.tmp", 0, 3},
    {"C1", "fsync", "sst-000006.sst.tmp", 0, 3},
    {"C1", "close", "sst-000006.sst.tmp", 0, 3},
    {"C1", "rename", "sst-000006.sst", 0, 3},
    {"C2", "fsync", "db", 3, 3},
    {"C3", "open", "MANIFEST.tmp", 1, 3},
    {"C3", "write", "MANIFEST.tmp", 1, 3},
    {"C3", "fsync", "MANIFEST.tmp", 1, 3},
    {"C3", "close", "MANIFEST.tmp", 1, 3},
    {"C4", "rename", "MANIFEST", 1, 3},
    // The rename already replaced MANIFEST; an error is not a rollback.
    {"C4", "fsync", "db", 4, 4},
    // Cleanup after durable publication: fatal, not retried.
    {"C6", "fsync", "db", 5, 4},
};

TEST_F(CompactionFaultTest, EveryCompactionFailureRequiresRecovery) {
    for (const CompactionFault& f : kCompactionFaults) {
        for (int err : {EIO, ENOSPC}) {
            SCOPED_TRACE(std::string(f.step) + " " + f.op + " " + f.name +
                         " errno " + std::to_string(err));
            fs::remove_all(Db());
            {
                FaultSyscalls sys;
                auto db = Prepare(sys);
                ASSERT_NE(db, nullptr);
                const std::size_t rule = sys.FailOn(f.op, f.name, err, f.skip);
                const Status s = db->Put("c", "3");
                EXPECT_EQ(s.code, Code::IOError) << s.ToString();
                EXPECT_EQ(sys.rules[rule].fired, 1u);  // reached production
                EXPECT_EQ(sys.Count("write", "wal-000005.log"), 0u);
                ExpectInvalidated(*db, sys);
            }
            ExpectLockReleased();
            ExpectRecovered(f.generation_after);
        }
    }
}

// Faults at the second of two outputs and at the steps after it. With one
// block per output, the triggering Put flushes {b} and compacts SSTs 2 and
// 4 into outputs 6 ({a}) and 7 ({b}).
TEST_F(CompactionFaultTest, FailuresAtALaterOutputRequireRecovery) {
    const SstLimits one_block{1, kMaxSstIndexSize};
    const CompactionFault faults[] = {
        {"C1", "open", "sst-000007.sst.tmp", 0, 3},
        {"C1", "write", "sst-000007.sst.tmp", 0, 3},
        {"C1", "fsync", "sst-000007.sst.tmp", 0, 3},
        {"C1", "close", "sst-000007.sst.tmp", 0, 3},
        {"C1", "rename", "sst-000007.sst", 0, 3},
        {"C2", "fsync", "db", 3, 3},
        {"C3", "write", "MANIFEST.tmp", 1, 3},
        {"C4", "rename", "MANIFEST", 1, 3},
        {"C4", "fsync", "db", 4, 4},
        {"C6", "fsync", "db", 5, 4},
    };
    for (const CompactionFault& f : faults) {
        SCOPED_TRACE(std::string(f.step) + " " + f.op + " " + f.name);
        fs::remove_all(Db());
        {
            FaultSyscalls sys;
            std::unique_ptr<DB> db;
            ASSERT_TRUE(OpenWith(sys, db, 2, 1, one_block).ok());
            ASSERT_TRUE(db->Put("a", kBig).ok());
            ASSERT_TRUE(db->Put("b", kBig).ok());
            sys.armed = true;
            const std::size_t rule = sys.FailOn(f.op, f.name, EIO, f.skip);
            const Status s = db->Put("c", "3");
            EXPECT_EQ(s.code, Code::IOError) << s.ToString();
            EXPECT_EQ(sys.rules[rule].fired, 1u);
            // The first output was complete before the fault.
            EXPECT_EQ(sys.Count("fsync", "sst-000006.sst.tmp"), 1u);
            EXPECT_EQ(sys.Count("write", "wal-000005.log"), 0u);
            ExpectInvalidated(*db, sys);
        }
        ExpectLockReleased();

        std::unique_ptr<DB> db;
        ASSERT_TRUE(OpenWith(DefaultPosixSyscalls(), db, 2, 1, one_block).ok());
        EXPECT_EQ(Manifest().generation, f.generation_after);
        EXPECT_EQ(Lookup(*db, "a"), kBig);
        EXPECT_EQ(Lookup(*db, "b"), kBig);
        EXPECT_EQ(Lookup(*db, "c"), std::nullopt);
        ASSERT_TRUE(db->Put("c", "3").ok());
        ASSERT_TRUE(db->Put("d", "4").ok());
        db.reset();
        ASSERT_TRUE(OpenWith(DefaultPosixSyscalls(), db, 2, 1, one_block).ok());
        EXPECT_EQ(Lookup(*db, "a"), kBig);
        EXPECT_EQ(Lookup(*db, "c"), "3");
        // Every SST file left is referenced: orphaned outputs were removed.
        const RawManifest m = Manifest();
        std::vector<std::string> referenced;
        for (const RawManifestSst& sst : m.ssts) {
            referenced.push_back(test::RawSstName(sst.file_id));
        }
        for (const std::string& name : Entries(Db())) {
            if (name.size() > 4 && name.substr(name.size() - 4) == ".sst") {
                EXPECT_NE(std::find(referenced.begin(), referenced.end(),
                                    name),
                          referenced.end())
                    << name;
            }
        }
    }
}

TEST_F(CompactionFaultTest, ExceptionDuringCompactionRequiresRecovery) {
    for (const auto& [op, name, generation] :
         std::vector<std::tuple<std::string, std::string, std::uint64_t>>{
             {"write", "sst-000006.sst.tmp", 3},
             {"rename", "MANIFEST", 3},
             {"unlink", "sst-000002.sst", 4}}) {
        SCOPED_TRACE(op + " " + name);
        fs::remove_all(Db());
        {
            FaultSyscalls sys;
            auto db = Prepare(sys);
            std::size_t rule = 0;
            if (op == "rename") {
                // The flush renames MANIFEST.tmp first.
                sys.rules.push_back({op, name, 0, true, 1, 1, 0});
            } else {
                rule = sys.ThrowOn(op, name);
            }
            EXPECT_THROW(db->Put("c", "3"), std::runtime_error);
            EXPECT_EQ(sys.rules[rule].fired, 1u);
            ExpectInvalidated(*db, sys);
        }
        ExpectRecovered(generation);
    }
}

// After durable publication an input unlink failure leaves garbage but
// keeps the handle; the removal is retried by the next publication.
TEST_F(CompactionFaultTest, InputUnlinkFailureLeavesGarbageButKeepsHandle) {
    FaultSyscalls sys;
    auto db = Prepare(sys);
    const std::size_t rule = sys.FailOn("unlink", "sst-000002.sst", EACCES);
    ASSERT_TRUE(db->Put("c", "3").ok());
    EXPECT_EQ(sys.rules[rule].fired, 1u);
    EXPECT_TRUE(fs::exists(Db() / "sst-000002.sst"));
    EXPECT_FALSE(fs::exists(Db() / "sst-000004.sst"));
    const RawManifest m = Manifest();
    ASSERT_EQ(m.ssts.size(), 1u);
    EXPECT_EQ(m.ssts[0].file_id, 6u);
    EXPECT_EQ(Lookup(*db, "a"), "1");

    ASSERT_TRUE(db->Put("d", "4").ok());  // flush + compaction + cleanup
    EXPECT_FALSE(fs::exists(Db() / "sst-000002.sst"));
    // The file the installed manifest references is never removed.
    const RawManifest after = Manifest();
    ASSERT_EQ(after.ssts.size(), 1u);
    EXPECT_TRUE(fs::exists(Db() / test::RawSstName(after.ssts[0].file_id)));
    for (const char* key : {"a", "b", "c", "d"}) {
        EXPECT_TRUE(Lookup(*db, key).has_value()) << key;
    }
}

// The compaction trace: inputs are read before the output is written, the
// output name is durable before the manifest names it, and inputs are
// removed only after the manifest's directory sync.
TEST_F(CompactionFaultTest, CompactionOrderSyncsOutputBeforePublication) {
    FaultSyscalls sys;
    auto db = Prepare(sys);
    ASSERT_TRUE(db->Put("c", "3").ok());
    const std::vector<std::string> expected_order = {
        "rename sst-000004.sst.tmp -> sst-000004.sst",  // the flush
        "rename MANIFEST.tmp -> MANIFEST",
        "pread sst-000002.sst",
        "pread sst-000004.sst",
        "open sst-000006.sst.tmp",
        "write sst-000006.sst.tmp",
        "fsync sst-000006.sst.tmp",
        "rename sst-000006.sst.tmp -> sst-000006.sst",
        "fsync db",
        "write MANIFEST.tmp",
        "fsync MANIFEST.tmp",
        "rename MANIFEST.tmp -> MANIFEST",
        "fsync db",
        "unlink sst-000002.sst",
        "unlink sst-000004.sst",
        "fsync db",
        "write wal-000005.log",
        "fsync wal-000005.log",
    };
    std::size_t at = 0;
    for (const std::string& step : expected_order) {
        const auto it =
            std::find(sys.trace.begin() + at, sys.trace.end(), step);
        ASSERT_NE(it, sys.trace.end()) << "missing or out of order: " << step;
        at = static_cast<std::size_t>(it - sys.trace.begin()) + 1;
    }
    EXPECT_EQ(sys.Count("fsync", "sst-000006.sst.tmp"), 1u);
    EXPECT_EQ(sys.Count("fsync", "db"), 6u);
}

TEST_F(CompactionFaultTest, CorruptInputFailsCompactionAndOpen) {
    FaultSyscalls sys;
    auto db = Prepare(sys);
    std::string bytes = ReadBytesOrEmpty(Db() / "sst-000002.sst");
    bytes[17] ^= 0x01;  // the key byte of "a"
    ASSERT_TRUE(test::WriteBytes(Db() / "sst-000002.sst", bytes));
    const Status s = db->Put("c", "3");
    EXPECT_EQ(s.code, Code::Corruption) << s.ToString();
    EXPECT_FALSE(fs::exists(Db() / "sst-000006.sst"));
    ExpectInvalidated(*db, sys);
    db.reset();
    EXPECT_EQ(Manifest().generation, 3u);
    std::unique_ptr<DB> again;
    EXPECT_EQ(OpenWith(DefaultPosixSyscalls(), again).code, Code::Corruption);
}

}  // namespace
}  // namespace kv
