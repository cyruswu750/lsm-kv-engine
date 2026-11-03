#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/db.h"
#include "core/db_internal.h"
#include "core/file_lock.h"
#include "core/types.h"
#include "testing/fault_syscalls.h"
#include "testing/manifest_builder.h"
#include "testing/sst_builder.h"
#include "testing/temp_dir.h"
#include "testing/wal_builder.h"

namespace kv {
namespace {

namespace fs = std::filesystem;

using test::kRawDelete;
using test::kRawPut;
using test::ManifestBytes;
using test::RawManifest;
using test::RawManifestSst;
using test::RawRecord;
using test::RawSst;
using test::RawSstV2;
using test::RawSstEntry;
using test::RawSstName;
using test::RawWalName;
using test::ReadBytesOrEmpty;
using test::WalFileHeaderBytes;
using test::WriteBytes;

constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();

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

// Every entry name with the bytes of each regular file.
std::vector<std::pair<std::string, std::string>> Snapshot(const fs::path& dir) {
    std::vector<std::pair<std::string, std::string>> out;
    for (const std::string& name : Entries(dir)) {
        out.emplace_back(name, fs::is_regular_file(fs::symlink_status(
                                   dir / name))
                                   ? ReadBytesOrEmpty(dir / name)
                                   : "<non-regular>");
    }
    return out;
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

class DBM2Test : public ::testing::Test {
protected:
    fs::path Db() const { return tmp_.path() / "db"; }

    // memtable_bytes = 1 flushes before every mutation that finds a
    // nonempty memtable, so file IDs are predictable: the n-th flush
    // writes sst-(2n) and wal-(2n+1).
    // compaction_trigger defaults to the engine default. Tests whose
    // subject is lookup across several live SSTs (an M2 property that
    // still holds below the trigger) pass 0 to disable compaction.
    Status OpenDb(std::unique_ptr<DB>& db,
                  std::size_t memtable_bytes = 1,
                  bool create = true,
                  std::size_t compaction_trigger =
                      InternalOptions{}.compaction_trigger) {
        Options options;
        options.memtable_bytes = memtable_bytes;
        options.create_if_missing = create;
        InternalOptions internal;
        internal.compaction_trigger = compaction_trigger;
        return DBOpener::Open(Db(), options, internal, DefaultPosixSyscalls(),
                              db);
    }

    std::unique_ptr<DB> MustOpen(std::size_t memtable_bytes = 1,
                                 std::size_t compaction_trigger =
                                     InternalOptions{}.compaction_trigger) {
        std::unique_ptr<DB> db;
        const Status s =
            OpenDb(db, memtable_bytes, /*create=*/true, compaction_trigger);
        EXPECT_TRUE(s.ok()) << s.ToString();
        return db;
    }

    RawManifest Manifest() const {
        RawManifest m;
        EXPECT_TRUE(test::ReadManifest(Db(), m));
        return m;
    }

    // Opening must fail with code, change nothing, and release the lock.
    // With a MANIFEST present the directory is a database, so Open takes
    // the lock (creating an empty LOCK) before validating; every other
    // rejected state is decided before LOCK would be created.
    void ExpectOpenFailsUnchanged(Code code, std::string_view detail) {
        auto before = Snapshot(Db());
        const bool is_database =
            std::any_of(before.begin(), before.end(),
                        [](const auto& e) { return e.first == "MANIFEST"; });
        const bool had_lock =
            std::any_of(before.begin(), before.end(),
                        [](const auto& e) { return e.first == "LOCK"; });
        if (is_database && !had_lock) {
            before.insert(before.begin(), {"LOCK", ""});
        }
        for (bool create : {false, true}) {
            std::unique_ptr<DB> db;
            Options options;
            options.create_if_missing = create;
            const Status s = DB::Open(Db(), options, db);
            EXPECT_EQ(s.code, code) << s.ToString();
            EXPECT_TRUE(Contains(s.message, detail)) << s.message;
            EXPECT_EQ(db, nullptr);
            EXPECT_EQ(Snapshot(Db()), before);
        }
        if (fs::exists(Db() / "LOCK")) {
            std::unique_ptr<FileLock> lock;
            EXPECT_TRUE(LockFile(Db() / "LOCK", lock).ok());
        }
    }

    // Writes a database from independently built bytes.
    void MakeDb(const RawManifest& manifest,
                const std::vector<std::pair<std::uint64_t, std::string>>& ssts,
                const std::string& wal) {
        fs::create_directory(Db());
        for (const auto& [id, bytes] : ssts) {
            ASSERT_TRUE(WriteBytes(Db() / RawSstName(id), bytes));
        }
        ASSERT_TRUE(WriteBytes(Db() / RawWalName(manifest.active_wal_id), wal));
        ASSERT_TRUE(WriteBytes(Db() / "MANIFEST", ManifestBytes(manifest)));
    }

    test::TempDir tmp_;
};

// One SST holding a single Put at sequence seq, with its manifest entry.
std::pair<std::string, RawManifestSst> OneEntrySst(std::uint64_t id,
                                                   const std::string& key,
                                                   std::uint64_t seq,
                                                   const std::string& value) {
    const std::string bytes = RawSst({{{key, seq, kRawPut, value}}});
    return {bytes, {id, bytes.size(), seq, seq}};
}

// ---- Flush and publication --------------------------------------------------

TEST_F(DBM2Test, FlushPublishesSstNewWalAndManifest) {
    auto db = MustOpen();
    ASSERT_TRUE(db->Put("a", "1").ok());
    EXPECT_EQ(Entries(Db()), (std::vector<std::string>{"LOCK", "MANIFEST",
                                                       "wal-000001.log"}));

    ASSERT_TRUE(db->Put("b", "2").ok());  // flushes {a} first
    EXPECT_EQ(Entries(Db()),
              (std::vector<std::string>{"LOCK", "MANIFEST", "sst-000002.sst",
                                        "wal-000003.log"}));
    const std::string sst = RawSstV2({{{"a", 1, kRawPut, "1"}}});
    EXPECT_EQ(ReadBytesOrEmpty(Db() / "sst-000002.sst"), sst);
    EXPECT_EQ(ReadBytesOrEmpty(Db() / "wal-000003.log"),
              WalFileHeaderBytes() + RawRecord(2, kRawPut, "b", "2"));

    RawManifest want;
    want.generation = 2;
    want.next_file_id = 4;
    want.checkpoint = 1;
    want.active_wal_id = 3;
    want.ssts = {{2, sst.size(), 1, 1}};
    EXPECT_EQ(ReadBytesOrEmpty(Db() / "MANIFEST"), ManifestBytes(want));

    EXPECT_EQ(Lookup(*db, "a"), "1");
    EXPECT_EQ(Lookup(*db, "b"), "2");
    db.reset();
    db = MustOpen();
    EXPECT_EQ(Lookup(*db, "a"), "1");
    EXPECT_EQ(Lookup(*db, "b"), "2");
}

TEST_F(DBM2Test, FlushWritesEveryMemtableEntryIncludingTombstones) {
    auto db = MustOpen(1 << 20);
    ASSERT_TRUE(db->Put("b", "x").ok());
    ASSERT_TRUE(db->Put("", "empty key").ok());
    ASSERT_TRUE(db->Delete("gone").ok());
    ASSERT_TRUE(db->Put("b", "y").ok());  // overwrite keeps only seq 4
    ASSERT_TRUE(db->Put(std::string("\x00\xff", 2), "").ok());
    db.reset();

    // Reopen with a tiny threshold: the next mutation flushes the replay.
    db = MustOpen(1);
    ASSERT_TRUE(db->Put("z", "after").ok());
    EXPECT_EQ(ReadBytesOrEmpty(Db() / "sst-000002.sst"),
              RawSstV2({{{"", 2, kRawPut, "empty key"},
                       {std::string("\x00\xff", 2), 5, kRawPut, ""},
                       {"b", 4, kRawPut, "y"},
                       {"gone", 3, kRawDelete, ""}}}));
    const RawManifest m = Manifest();
    ASSERT_EQ(m.ssts.size(), 1u);
    EXPECT_EQ(m.ssts[0].min_sequence, 2u);
    EXPECT_EQ(m.ssts[0].max_sequence, 5u);
    EXPECT_EQ(m.checkpoint, 5u);
}

TEST_F(DBM2Test, FlushRunsBeforeTheMutationThatReachesTheThreshold) {
    // Each one-byte-key, one-byte-value entry counts 34 bytes.
    auto db = MustOpen(100);
    ASSERT_TRUE(db->Put("a", "1").ok());
    ASSERT_TRUE(db->Put("b", "2").ok());
    ASSERT_TRUE(db->Put("c", "3").ok());  // 102 bytes after this append
    EXPECT_FALSE(fs::exists(Db() / "sst-000002.sst"));
    ASSERT_TRUE(db->Put("d", "4").ok());  // finds 102 >= 100: flush first
    EXPECT_TRUE(fs::exists(Db() / "sst-000002.sst"));
    EXPECT_EQ(Manifest().checkpoint, 3u);
    EXPECT_EQ(ReadBytesOrEmpty(Db() / "wal-000003.log"),
              WalFileHeaderBytes() + RawRecord(4, kRawPut, "d", "4"));
}

TEST_F(DBM2Test, InvalidMutationDoesNotTriggerFlush) {
    auto db = MustOpen();
    ASSERT_TRUE(db->Put("a", "1").ok());
    const auto before = Snapshot(Db());
    EXPECT_EQ(db->Put(std::string(kMaxKeyLength + 1, 'k'), "v").code,
              Code::InvalidArgument);
    EXPECT_EQ(db->Put("k", std::string(kMaxValueLength + 1, 'v')).code,
              Code::InvalidArgument);
    EXPECT_EQ(Snapshot(Db()), before);
    ASSERT_TRUE(db->Put("b", "2").ok());
    EXPECT_TRUE(fs::exists(Db() / "sst-000002.sst"));
}

// Compaction is disabled so that four SSTs stay live (docs/m3-design.md
// section 10); compacted lookups are covered in db_m3_test.cpp.
TEST_F(DBM2Test, LookupStopsAtNewestValueOrTombstoneAcrossSsts) {
    auto db = MustOpen(1, /*compaction_trigger=*/0);
    ASSERT_TRUE(db->Put("k", "v1").ok());
    ASSERT_TRUE(db->Put("k", "v2").ok());   // v1 -> sst 2
    ASSERT_TRUE(db->Put("other", "o").ok());  // v2 -> sst 4
    ASSERT_TRUE(db->Delete("k").ok());        // other -> sst 6
    ASSERT_TRUE(db->Put("tail", "t").ok());   // tombstone -> sst 8
    EXPECT_EQ(Manifest().ssts.size(), 4u);
    EXPECT_EQ(Lookup(*db, "k"), std::nullopt);
    EXPECT_EQ(Lookup(*db, "other"), "o");
    ASSERT_TRUE(db->Put("k", "v3").ok());  // newest in the WAL/memtable
    EXPECT_EQ(Lookup(*db, "k"), "v3");
    db.reset();
    db = MustOpen(1, /*compaction_trigger=*/0);
    EXPECT_EQ(Lookup(*db, "k"), "v3");
    EXPECT_EQ(Lookup(*db, "tail"), "t");
}

// Put -> flush -> Delete -> flush: the tombstone SST shadows the value
// SST in memory and after reopen.
TEST_F(DBM2Test, FlushedDeleteShadowsOlderFlushedValue) {
    auto db = MustOpen();
    ASSERT_TRUE(db->Put("k", "old").ok());
    ASSERT_TRUE(db->Delete("k").ok());     // flushes the Put
    ASSERT_TRUE(db->Put("x", "1").ok());   // flushes the Delete
    const RawManifest m = Manifest();
    ASSERT_EQ(m.ssts.size(), 2u);
    EXPECT_EQ(ReadBytesOrEmpty(Db() / "sst-000004.sst"),
              RawSstV2({{{"k", 2, kRawDelete, ""}}}));
    EXPECT_EQ(Lookup(*db, "k"), std::nullopt);
    db.reset();
    for (int round = 0; round < 2; ++round) {
        db = MustOpen();
        EXPECT_EQ(Lookup(*db, "k"), std::nullopt);
        db.reset();
    }
}

// A Delete that exists only in the active WAL must shadow an SST value.
TEST_F(DBM2Test, WalOnlyDeleteShadowsFlushedValue) {
    {
        auto db = MustOpen();
        ASSERT_TRUE(db->Put("k", "old").ok());
        ASSERT_TRUE(db->Delete("k").ok());  // Put flushed; Delete in WAL
        EXPECT_EQ(Lookup(*db, "k"), std::nullopt);
    }
    EXPECT_EQ(ReadBytesOrEmpty(Db() / "wal-000003.log"),
              WalFileHeaderBytes() + RawRecord(2, kRawDelete, "k", ""));
    auto db = MustOpen(1 << 20);
    EXPECT_EQ(Lookup(*db, "k"), std::nullopt);
}

// Compaction is disabled so the maximum entry keeps its own SST.
TEST_F(DBM2Test, LargeEntriesAtApiLimitsSurviveFlushAndReopen) {
    const std::string max_key(kMaxKeyLength, 'k');
    const std::string max_value(kMaxValueLength, '\x80');
    const std::string mid_value(5000, 'm');
    {
        auto db = MustOpen(1, /*compaction_trigger=*/0);
        ASSERT_TRUE(db->Put(max_key, max_value).ok());
        ASSERT_TRUE(db->Put("mid", mid_value).ok());
        ASSERT_TRUE(db->Delete(max_key + "").ok());
        ASSERT_TRUE(db->Put(max_key, "small").ok());
        ASSERT_TRUE(db->Put("last", "").ok());
        EXPECT_EQ(Manifest().ssts.size(), 4u);
    }
    auto db = MustOpen(1, /*compaction_trigger=*/0);
    EXPECT_EQ(Lookup(*db, max_key), "small");
    EXPECT_EQ(Lookup(*db, "mid"), mid_value);
    EXPECT_EQ(Lookup(*db, "last"), "");
    // The maximum entry is still readable from its own SST.
    std::string sst = ReadBytesOrEmpty(Db() / "sst-000002.sst");
    EXPECT_GT(sst.size(), kMaxValueLength);
}

// ---- Sequence and checkpoint rules --------------------------------------------

TEST_F(DBM2Test, EmptyActiveWalContinuesAboveCheckpoint) {
    for (std::uint64_t checkpoint : {7u, 10u}) {
        SCOPED_TRACE(checkpoint);
        fs::remove_all(Db());
        const auto [sst, meta] = OneEntrySst(2, "a", 7, "x");
        RawManifest m;
        m.generation = 2;
        m.next_file_id = 4;
        m.checkpoint = checkpoint;
        m.active_wal_id = 3;
        m.ssts = {meta};
        MakeDb(m, {{2, sst}}, WalFileHeaderBytes());

        {
            auto db = MustOpen(1 << 20);
            EXPECT_EQ(Lookup(*db, "a"), "x");
            ASSERT_TRUE(db->Put("b", "y").ok());
        }
        EXPECT_EQ(ReadBytesOrEmpty(Db() / "wal-000003.log"),
                  WalFileHeaderBytes() +
                      RawRecord(checkpoint + 1, kRawPut, "b", "y"));
        auto db = MustOpen(1);
        ASSERT_TRUE(db->Put("c", "z").ok());  // flushes {b}
        EXPECT_EQ(Manifest().checkpoint, checkpoint + 1);
        EXPECT_EQ(Lookup(*db, "a"), "x");
        EXPECT_EQ(Lookup(*db, "b"), "y");
    }
}

TEST_F(DBM2Test, WalRecordAtOrBelowCheckpointIsCorruption) {
    const auto [sst, meta] = OneEntrySst(2, "a", 7, "x");
    RawManifest m;
    m.generation = 2;
    m.next_file_id = 4;
    m.checkpoint = 7;
    m.active_wal_id = 3;
    m.ssts = {meta};

    for (std::uint64_t seq : {1u, 7u}) {
        SCOPED_TRACE(seq);
        fs::remove_all(Db());
        MakeDb(m, {{2, sst}},
               WalFileHeaderBytes() + RawRecord(seq, kRawPut, "b", "y"));
        ExpectOpenFailsUnchanged(Code::Corruption, "manifest checkpoint");
    }

    // A complete header at the checkpoint whose payload is missing is
    // Corruption, not a torn tail to truncate.
    fs::remove_all(Db());
    const std::string partial =
        RawRecord(7, kRawPut, "b", "yyyy").substr(0, 30);
    MakeDb(m, {{2, sst}}, WalFileHeaderBytes() + partial);
    ExpectOpenFailsUnchanged(Code::Corruption, "does not exceed");

    fs::remove_all(Db());
    MakeDb(m, {{2, sst}},
           WalFileHeaderBytes() + RawRecord(8, kRawPut, "b", "y"));
    auto db = MustOpen();
    EXPECT_EQ(Lookup(*db, "b"), "y");
}

TEST_F(DBM2Test, MaxCheckpointAllowsReadsButRejectsMutationsBeforeIo) {
    const auto [sst, meta] = OneEntrySst(2, "k", kMax, "v");
    RawManifest m;
    m.generation = 2;
    m.next_file_id = 4;
    m.checkpoint = kMax;
    m.active_wal_id = 3;
    m.ssts = {meta};
    MakeDb(m, {{2, sst}}, WalFileHeaderBytes());
    const auto before = Snapshot(Db());
    for (int round = 0; round < 2; ++round) {
        auto db = MustOpen();
        EXPECT_EQ(Lookup(*db, "k"), "v");
        EXPECT_EQ(db->Put("k", "w").code, Code::InvalidArgument);
        EXPECT_EQ(db->Delete("k").code, Code::InvalidArgument);
        EXPECT_EQ(Lookup(*db, "k"), "v");
    }
    // Only LOCK is created; nothing else changes.
    auto after = Snapshot(Db());
    after.erase(std::remove_if(after.begin(), after.end(),
                               [](const auto& e) { return e.first == "LOCK"; }),
                after.end());
    EXPECT_EQ(after, before);
}

// The sequence check runs before maintenance: a full memtable at the
// last sequence must not flush for a mutation that cannot be accepted.
TEST_F(DBM2Test, ExhaustedSequenceDoesNotTriggerFlush) {
    ASSERT_TRUE(test::MakeWalOnlyDb(
        Db(), WalFileHeaderBytes() + RawRecord(kMax, kRawPut, "k", "v")));
    auto db = MustOpen(1);
    const auto before = Snapshot(Db());
    EXPECT_EQ(db->Put("k", "w").code, Code::InvalidArgument);
    EXPECT_EQ(db->Delete("k").code, Code::InvalidArgument);
    EXPECT_EQ(Snapshot(Db()), before);
    EXPECT_EQ(Lookup(*db, "k"), "v");
}

// ---- Counter exhaustion -------------------------------------------------------

TEST_F(DBM2Test, FileIdExhaustionRejectsMutationWithoutIo) {
    RawManifest m;
    m.active_wal_id = kMax - 3;
    m.next_file_id = kMax - 2;  // exactly one more flush fits
    MakeDb(m, {}, WalFileHeaderBytes());

    auto db = MustOpen();
    ASSERT_TRUE(db->Put("a", "1").ok());
    ASSERT_TRUE(db->Put("b", "2").ok());  // flush uses max-2 and max-1
    EXPECT_TRUE(fs::exists(Db() / RawSstName(kMax - 2)));
    EXPECT_TRUE(fs::exists(Db() / RawWalName(kMax - 1)));
    EXPECT_EQ(Manifest().next_file_id, kMax);

    const auto before = Snapshot(Db());
    const Status s = db->Put("c", "3");
    EXPECT_EQ(s.code, Code::InvalidArgument) << s.ToString();
    EXPECT_TRUE(Contains(s.message, "file IDs are exhausted")) << s.message;
    EXPECT_EQ(Snapshot(Db()), before);
    EXPECT_EQ(Lookup(*db, "a"), "1");  // still usable for reads
    EXPECT_EQ(Lookup(*db, "b"), "2");
    db.reset();

    db = MustOpen();
    EXPECT_EQ(Lookup(*db, "b"), "2");
    EXPECT_EQ(db->Put("c", "3").code, Code::InvalidArgument);
}

TEST_F(DBM2Test, ObservedMaxIdOrphanExhaustsFileIds) {
    RawManifest m;
    MakeDb(m, {}, WalFileHeaderBytes() + RawRecord(1, kRawPut, "a", "1"));
    ASSERT_TRUE(WriteBytes(Db() / (RawSstName(kMax) + ".tmp"), "orphan"));
    auto db = MustOpen();
    EXPECT_EQ(db->Put("b", "2").code, Code::InvalidArgument);
    EXPECT_TRUE(fs::exists(Db() / (RawSstName(kMax) + ".tmp")));
}

TEST_F(DBM2Test, GenerationExhaustionRejectsMutationWithoutIo) {
    RawManifest m;
    m.generation = kMax - 1;
    MakeDb(m, {}, WalFileHeaderBytes());
    auto db = MustOpen();
    ASSERT_TRUE(db->Put("a", "1").ok());
    ASSERT_TRUE(db->Put("b", "2").ok());  // publishes generation max
    EXPECT_EQ(Manifest().generation, kMax);

    const auto before = Snapshot(Db());
    const Status s = db->Put("c", "3");
    EXPECT_EQ(s.code, Code::InvalidArgument) << s.ToString();
    EXPECT_TRUE(Contains(s.message, "generations are exhausted"));
    EXPECT_EQ(Snapshot(Db()), before);
    EXPECT_EQ(Lookup(*db, "b"), "2");
}

// ---- Directory states (docs/sst-format.md section 8) ---------------------------

TEST_F(DBM2Test, ValidM1DirectoryIsUnsupportedAndUnchanged) {
    for (bool with_lock : {false, true}) {
        SCOPED_TRACE(with_lock);
        fs::remove_all(Db());
        fs::create_directory(Db());
        ASSERT_TRUE(WriteBytes(Db() / "000001.wal",
                               WalFileHeaderBytes() +
                                   RawRecord(1, kRawPut, "k", "v")));
        if (with_lock) {
            ASSERT_TRUE(WriteBytes(Db() / "LOCK", ""));
        }
        ExpectOpenFailsUnchanged(Code::UnsupportedFormat, "M1 database");
        EXPECT_EQ(fs::exists(Db() / "LOCK"), with_lock);
    }
    // Even a non-regular 000001.wal identifies an M1 directory.
    fs::remove_all(Db());
    fs::create_directory(Db());
    fs::create_directory(Db() / "000001.wal");
    ExpectOpenFailsUnchanged(Code::UnsupportedFormat, "M1 database");
}

TEST_F(DBM2Test, InterruptedInitializationResidueIsPreservedCorruption) {
    const std::string wal = WalFileHeaderBytes();
    const std::vector<std::vector<std::pair<std::string, std::string>>> cases =
        {
            {{"wal-000001.log.tmp", wal.substr(0, 5)}},
            {{"wal-000001.log.tmp", wal}},
            {{"wal-000001.log", wal}},
            {{"wal-000001.log", wal}, {"MANIFEST.tmp", "partial"}},
            {{"wal-000001.log", wal},
             {"MANIFEST.tmp", ManifestBytes(RawManifest{})}},
            {{"MANIFEST.tmp", ""}},
            {{"sst-000002.sst", "x"}},
            {{"LOCK", ""}, {"wal-000001.log", wal}},
            {{"wal-000001.log", wal}, {"notes.txt", "unrelated"}},
        };
    for (const auto& files : cases) {
        SCOPED_TRACE(files.size() == 1 ? files[0].first
                                       : files[0].first + " + " +
                                             files[1].first);
        fs::remove_all(Db());
        fs::create_directory(Db());
        for (const auto& [name, bytes] : files) {
            ASSERT_TRUE(WriteBytes(Db() / name, bytes));
        }
        ExpectOpenFailsUnchanged(Code::Corruption, "interrupted");
    }
}

TEST_F(DBM2Test, MalformedManifestIsNeverTreatedAsANewDatabase) {
    // Manifest version 2 is current (M3). Version 1 (the M2 format) and
    // unknown versions are UnsupportedFormat (docs/sst-format.md 4.3).
    RawManifest v1;
    v1.version = 1;
    RawManifest v3;
    v3.version = 3;
    std::string bad_crc = ManifestBytes(RawManifest{});
    bad_crc.back() ^= 1;
    const struct {
        const char* name;
        std::string bytes;
        Code code;
    } cases[] = {
        {"empty", "", Code::Corruption},
        {"garbage", "not a manifest at all, but long enough to read",
         Code::Corruption},
        {"truncated", ManifestBytes(RawManifest{}).substr(0, 59),
         Code::Corruption},
        {"bad crc", bad_crc, Code::Corruption},
        {"version 1", ManifestBytes(v1), Code::UnsupportedFormat},
        {"version 3", ManifestBytes(v3), Code::UnsupportedFormat},
    };
    for (const auto& c : cases) {
        SCOPED_TRACE(c.name);
        fs::remove_all(Db());
        fs::create_directory(Db());
        ASSERT_TRUE(WriteBytes(Db() / "wal-000001.log", WalFileHeaderBytes()));
        ASSERT_TRUE(WriteBytes(Db() / "MANIFEST", c.bytes));
        ExpectOpenFailsUnchanged(c.code, "MANIFEST");
    }

    fs::remove_all(Db());
    fs::create_directory(Db());
    fs::create_directory(Db() / "MANIFEST");
    ExpectOpenFailsUnchanged(Code::Corruption, "not a regular file");
}

TEST_F(DBM2Test, ManifestTakesPrecedenceOverM1NamesAndUnrelatedEntries) {
    ASSERT_TRUE(test::MakeWalOnlyDb(
        Db(), WalFileHeaderBytes() + RawRecord(1, kRawPut, "k", "v")));
    ASSERT_TRUE(WriteBytes(Db() / "000001.wal", "m1"));
    ASSERT_TRUE(WriteBytes(Db() / "notes.txt", "keep"));
    ASSERT_TRUE(WriteBytes(Db() / "sst-1.sst", "keep"));  // not canonical
    {
        auto db = MustOpen();
        EXPECT_EQ(Lookup(*db, "k"), "v");
        ASSERT_TRUE(db->Put("k2", "v2").ok());  // flush and cleanup
    }
    EXPECT_EQ(ReadBytesOrEmpty(Db() / "000001.wal"), "m1");
    EXPECT_EQ(ReadBytesOrEmpty(Db() / "notes.txt"), "keep");
    EXPECT_EQ(ReadBytesOrEmpty(Db() / "sst-1.sst"), "keep");
}

class DBM2ReferenceTest : public DBM2Test {
protected:
    // Two flushed SSTs and a WAL record: sst 2 {a@1}, sst 4 {b@2},
    // wal 5 {c@3}.
    void SetUp() override {
        auto db = MustOpen();
        ASSERT_TRUE(db->Put("a", "1").ok());
        ASSERT_TRUE(db->Put("b", "2").ok());
        ASSERT_TRUE(db->Put("c", "3").ok());
        db.reset();
        ASSERT_EQ(Entries(Db()),
                  (std::vector<std::string>{"LOCK", "MANIFEST",
                                            "sst-000002.sst", "sst-000004.sst",
                                            "wal-000005.log"}));
        sst_ = ReadBytesOrEmpty(Db() / "sst-000004.sst");
    }

    fs::path Sst() const { return Db() / "sst-000004.sst"; }

    std::string sst_;
};

TEST_F(DBM2ReferenceTest, MissingReferencedFilesAreCorruption) {
    fs::remove(Sst());
    ExpectOpenFailsUnchanged(Code::Corruption, "referenced by MANIFEST");
    ASSERT_TRUE(WriteBytes(Sst(), sst_));
    fs::rename(Db() / "wal-000005.log", Db() / "elsewhere");
    ExpectOpenFailsUnchanged(Code::Corruption,
                             "wal-000005.log: referenced by MANIFEST");
    fs::rename(Db() / "elsewhere", Db() / "wal-000005.log");
    auto db = MustOpen();
    EXPECT_EQ(Lookup(*db, "b"), "2");
}

TEST_F(DBM2ReferenceTest, NonRegularReferencedSstIsCorruption) {
    fs::remove(Sst());
    fs::create_symlink(Db() / "sst-000002.sst", Sst());
    ExpectOpenFailsUnchanged(Code::Corruption, "not a regular file");
}

TEST_F(DBM2ReferenceTest, DamagedReferencedSstFailsOpen) {
    ASSERT_TRUE(WriteBytes(Sst(), sst_ + "x"));
    ExpectOpenFailsUnchanged(Code::Corruption, "differs from the expected");
    ASSERT_TRUE(WriteBytes(Sst(), sst_.substr(0, sst_.size() - 1)));
    ExpectOpenFailsUnchanged(Code::Corruption, "differs from the expected");
    std::string flipped = sst_;
    flipped[3] ^= 0x10;
    ASSERT_TRUE(WriteBytes(Sst(), flipped));
    ExpectOpenFailsUnchanged(Code::Corruption, "sst-000004.sst");
    // The engine writes SST version 2 (M4); version 3 is unknown.
    std::string v3 = sst_;
    v3[v3.size() - 12] = 3;
    ASSERT_TRUE(WriteBytes(Sst(), v3));
    ExpectOpenFailsUnchanged(Code::UnsupportedFormat, "SST version 3");
}

// A valid SST whose actual sequences differ from the manifest's record.
TEST_F(DBM2ReferenceTest, SstSequenceRangeMustMatchManifest) {
    const std::string other = RawSstV2({{{"b", 3, kRawPut, "2"}}});
    ASSERT_EQ(other.size(), sst_.size());
    ASSERT_TRUE(WriteBytes(Sst(), other));
    ExpectOpenFailsUnchanged(Code::Corruption,
                             "differs from the manifest's [2, 2]");
}

// ---- Orphans and file-ID high-water (docs/sst-format.md section 7) -------------

TEST_F(DBM2Test, CoveredOrphansAreRemovedAndOthersKept) {
    {
        auto db = MustOpen();
        ASSERT_TRUE(db->Put("a", "1").ok());
        ASSERT_TRUE(db->Put("b", "2").ok());  // next_file_id becomes 4
    }
    // Covered by next_file_id 4: an obsolete WAL, an unreferenced SST, and
    // temporaries. MANIFEST.tmp has no ID and is always removable.
    for (const char* name : {"wal-000001.log", "sst-000001.sst",
                             "sst-000002.sst.tmp", "wal-000003.log.tmp",
                             "MANIFEST.tmp"}) {
        ASSERT_TRUE(WriteBytes(Db() / name, "orphan"));
    }
    fs::create_directory(Db() / "wal-000002.log");  // non-regular: kept
    ASSERT_TRUE(WriteBytes(Db() / "notes.tmp", "keep"));

    auto db = MustOpen();
    EXPECT_EQ(Entries(Db()),
              (std::vector<std::string>{"LOCK", "MANIFEST", "notes.tmp",
                                        "sst-000002.sst", "wal-000002.log",
                                        "wal-000003.log"}));
    EXPECT_EQ(Lookup(*db, "a"), "1");
    EXPECT_EQ(Lookup(*db, "b"), "2");
}

TEST_F(DBM2Test, HighIdOrphansAreKeptUntilAPublishedHighWaterCoversThem) {
    {
        auto db = MustOpen();
        ASSERT_TRUE(db->Put("a", "1").ok());
    }
    // next_file_id is 2. These IDs were observed but never published.
    ASSERT_TRUE(WriteBytes(Db() / "sst-000009.sst", "orphan"));
    ASSERT_TRUE(WriteBytes(Db() / "wal-000010.log.tmp", "orphan"));

    // Two reopens without a publication keep the evidence.
    for (int round = 0; round < 2; ++round) {
        auto db = MustOpen(1 << 20);
        EXPECT_EQ(Lookup(*db, "a"), "1");
        db.reset();
        EXPECT_TRUE(fs::exists(Db() / "sst-000009.sst"));
        EXPECT_TRUE(fs::exists(Db() / "wal-000010.log.tmp"));
        EXPECT_EQ(Manifest().next_file_id, 2u);
    }

    // The next flush allocates above them, publishes a covering high-water
    // mark, and only then removes them.
    auto db = MustOpen();
    ASSERT_TRUE(db->Put("b", "2").ok());
    EXPECT_EQ(Manifest().next_file_id, 13u);
    EXPECT_EQ(Entries(Db()),
              (std::vector<std::string>{"LOCK", "MANIFEST", "sst-000011.sst",
                                        "wal-000012.log"}));
    db.reset();
    db = MustOpen();
    EXPECT_EQ(Lookup(*db, "a"), "1");
    EXPECT_EQ(Lookup(*db, "b"), "2");
}

TEST_F(DBM2Test, FailedOrphanRemovalAtOpenIsRetriedAfterPublication) {
    {
        auto db = MustOpen();
        ASSERT_TRUE(db->Put("a", "1").ok());
        ASSERT_TRUE(db->Put("b", "2").ok());
    }
    ASSERT_TRUE(WriteBytes(Db() / "sst-000001.sst", "orphan"));

    test::FaultSyscalls sys;
    sys.armed = true;
    const std::size_t rule = sys.FailOn("unlink", "sst-000001.sst", EACCES);
    Options options;
    options.memtable_bytes = 1;
    std::unique_ptr<DB> db;
    ASSERT_TRUE(DBOpener::Open(Db(), options, sys, db).ok());
    EXPECT_EQ(sys.rules[rule].fired, 1u);
    EXPECT_TRUE(fs::exists(Db() / "sst-000001.sst"));

    ASSERT_TRUE(db->Put("c", "3").ok());  // publication retries cleanup
    EXPECT_FALSE(fs::exists(Db() / "sst-000001.sst"));
    EXPECT_EQ(sys.Count("unlink", "sst-000001.sst"), 2u);
}

TEST_F(DBM2Test, StaleManifestTempThatOpenCouldNotRemoveDoesNotBlockFlush) {
    {
        auto db = MustOpen();
        ASSERT_TRUE(db->Put("a", "1").ok());
    }
    ASSERT_TRUE(WriteBytes(Db() / "MANIFEST.tmp", "stale"));

    test::FaultSyscalls sys;
    sys.armed = true;
    const std::size_t rule = sys.FailOn("unlink", "MANIFEST.tmp", EACCES);
    Options options;
    options.memtable_bytes = 1;
    std::unique_ptr<DB> db;
    ASSERT_TRUE(DBOpener::Open(Db(), options, sys, db).ok());
    EXPECT_EQ(sys.rules[rule].fired, 1u);
    EXPECT_EQ(ReadBytesOrEmpty(Db() / "MANIFEST.tmp"), "stale");

    ASSERT_TRUE(db->Put("b", "2").ok());  // removes it, then publishes
    EXPECT_EQ(sys.Count("unlink", "MANIFEST.tmp"), 2u);
    EXPECT_EQ(Manifest().generation, 2u);
    EXPECT_FALSE(fs::exists(Db() / "MANIFEST.tmp"));
}

}  // namespace
}  // namespace kv
