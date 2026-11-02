#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "core/db.h"
#include "core/db_internal.h"
#include "core/file_lock.h"
#include "testing/fault_syscalls.h"
#include "testing/manifest_builder.h"
#include "testing/temp_dir.h"
#include "testing/wal_builder.h"

namespace kv {
namespace {

namespace fs = std::filesystem;

using test::FaultSyscalls;
using test::RawManifest;

// Publication and cleanup faults injected through the internal syscall
// seam (docs/sst-format.md section 10). These check control flow and the
// state visible through the filesystem afterwards. They do not model
// power loss: the page cache survives every injected failure, and a
// scripted error is not a real device writeback error.

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

// Largest engine-file ID present in dir (0 if none).
std::uint64_t MaxEngineId(const fs::path& dir) {
    std::uint64_t max_id = 0;
    for (const std::string& name : Entries(dir)) {
        if (name.rfind("wal-", 0) == 0 || name.rfind("sst-", 0) == 0) {
            max_id = std::max<std::uint64_t>(
                max_id, std::stoull(name.substr(4, name.find('.') - 4)));
        }
    }
    return max_id;
}

class PublishFaultTest : public ::testing::Test {
protected:
    fs::path Db() const { return tmp_.path() / "db"; }

    Status OpenWith(PosixSyscalls& sys, std::unique_ptr<DB>& db) {
        Options options;
        options.memtable_bytes = 1;  // every later mutation flushes first
        return DBOpener::Open(Db(), options, sys, db);
    }

    Status OpenReal(std::unique_ptr<DB>& db) {
        Options options;
        options.memtable_bytes = 1;
        return DB::Open(Db(), options, db);
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

    // Every call returns NeedsRecovery and no syscall is made.
    void ExpectInvalidated(DB& db, const FaultSyscalls& sys) {
        const std::size_t calls = sys.TotalCalls();
        EXPECT_EQ(db.Put("x", "v").code, Code::NeedsRecovery);
        EXPECT_EQ(db.Delete("a").code, Code::NeedsRecovery);
        std::string out = "stale";
        EXPECT_EQ(db.Get("a", out).code, Code::NeedsRecovery);
        EXPECT_TRUE(out.empty());
        EXPECT_EQ(sys.TotalCalls(), calls);
    }

    // Reopens with real syscalls, checks the acknowledged state, then
    // checks that new IDs exceed every ID left on disk and that the
    // database keeps working across another reopen.
    void ExpectRecovered(std::uint64_t generation) {
        const std::uint64_t max_left = MaxEngineId(Db());
        std::unique_ptr<DB> db;
        ASSERT_TRUE(OpenReal(db).ok());
        EXPECT_EQ(Manifest().generation, generation);
        EXPECT_EQ(Lookup(*db, "a"), "1");
        // The failed mutation was never appended.
        EXPECT_EQ(Lookup(*db, "b"), std::nullopt);

        ASSERT_TRUE(db->Put("c", "3").ok());
        ASSERT_TRUE(db->Put("d", "4").ok());
        const RawManifest m = Manifest();
        ASSERT_FALSE(m.ssts.empty());
        EXPECT_GT(m.ssts.back().file_id, max_left);
        db.reset();
        ASSERT_TRUE(OpenReal(db).ok());
        EXPECT_EQ(Lookup(*db, "a"), "1");
        EXPECT_EQ(Lookup(*db, "c"), "3");
        EXPECT_EQ(Lookup(*db, "d"), "4");
    }

    // Opens with sys, writes a=1 (WAL only), then arms sys.
    std::unique_ptr<DB> Prepare(FaultSyscalls& sys) {
        std::unique_ptr<DB> db;
        EXPECT_TRUE(OpenWith(sys, db).ok());
        EXPECT_TRUE(db->Put("a", "1").ok());
        sys.armed = true;
        return db;
    }

    test::TempDir tmp_;
};

struct FaultPoint {
    const char* step;
    const char* op;
    const char* name;
    std::size_t skip;
    std::uint64_t generation_after;  // durable generation after the fault
};

// Every syscall of steps F1-F5 for the first flush (sst 2, wal 3).
const FaultPoint kFlushFaults[] = {
    {"F1", "open", "sst-000002.sst.tmp", 0, 1},
    {"F1", "write", "sst-000002.sst.tmp", 0, 1},
    {"F1", "fsync", "sst-000002.sst.tmp", 0, 1},
    {"F1", "close", "sst-000002.sst.tmp", 0, 1},
    {"F1", "rename", "sst-000002.sst", 0, 1},
    {"F2", "open", "wal-000003.log.tmp", 0, 1},
    {"F2", "write", "wal-000003.log.tmp", 0, 1},
    {"F2", "fsync", "wal-000003.log.tmp", 0, 1},
    {"F2", "close", "wal-000003.log.tmp", 0, 1},
    {"F2", "rename", "wal-000003.log", 0, 1},
    {"F2", "open", "wal-000003.log", 0, 1},
    {"F3", "fsync", "db", 0, 1},
    {"F4", "open", "MANIFEST.tmp", 0, 1},
    {"F4", "write", "MANIFEST.tmp", 0, 1},
    {"F4", "fsync", "MANIFEST.tmp", 0, 1},
    {"F4", "close", "MANIFEST.tmp", 0, 1},
    {"F5", "rename", "MANIFEST", 0, 1},
    // The rename already replaced MANIFEST; an error is not a rollback.
    {"F5", "fsync", "db", 1, 2},
};

TEST_F(PublishFaultTest, EveryPublicationFailureRequiresRecovery) {
    for (const FaultPoint& f : kFlushFaults) {
        for (int err : {EIO, ENOSPC}) {
            SCOPED_TRACE(std::string(f.step) + " " + f.op + " " + f.name +
                         " errno " + std::to_string(err));
            fs::remove_all(Db());
            {
                FaultSyscalls sys;
                auto db = Prepare(sys);
                ASSERT_NE(db, nullptr);
                const std::size_t rule = sys.FailOn(f.op, f.name, err, f.skip);

                const Status s = db->Put("b", "2");
                EXPECT_EQ(s.code, Code::IOError) << s.ToString();
                EXPECT_EQ(sys.rules[rule].fired, 1u);  // reached production
                // Nothing was appended to any WAL after the failure.
                EXPECT_EQ(sys.Count("write", "wal-000001.log"), 0u);
                EXPECT_EQ(sys.Count("write", "wal-000003.log"), 0u);
                ExpectInvalidated(*db, sys);
            }
            ExpectLockReleased();
            ExpectRecovered(f.generation_after);
        }
    }
}

TEST_F(PublishFaultTest, ShortWritesDuringPublicationStillSucceed) {
    FaultSyscalls sys;
    auto db = Prepare(sys);
    sys.write_script = {FaultSyscalls::Short(1), FaultSyscalls::Error(EINTR),
                        FaultSyscalls::Short(7), FaultSyscalls::Short(3),
                        FaultSyscalls::Error(EINTR), FaultSyscalls::Short(2)};
    ASSERT_TRUE(db->Put("b", "2").ok());
    EXPECT_TRUE(sys.write_script.empty());
    db.reset();
    ASSERT_TRUE(OpenReal(db).ok());
    EXPECT_EQ(Lookup(*db, "a"), "1");
    EXPECT_EQ(Lookup(*db, "b"), "2");
}

TEST_F(PublishFaultTest, ExceptionBeforeOrAfterDurablePublication) {
    const struct {
        const char* op;
        const char* name;
        std::uint64_t generation_after;
    } cases[] = {
        {"fsync", "sst-000002.sst.tmp", 1},
        {"write", "MANIFEST.tmp", 1},
        {"rename", "MANIFEST", 1},
        {"unlink", "wal-000001.log", 2},  // during cleanup (F7)
    };
    for (const auto& c : cases) {
        SCOPED_TRACE(std::string(c.op) + " " + c.name);
        fs::remove_all(Db());
        {
            FaultSyscalls sys;
            auto db = Prepare(sys);
            const std::size_t rule = sys.ThrowOn(c.op, c.name);
            EXPECT_THROW(db->Put("b", "2"), std::runtime_error);
            EXPECT_EQ(sys.rules[rule].fired, 1u);
            ExpectInvalidated(*db, sys);
        }
        ExpectRecovered(c.generation_after);
    }
}

// ---- Cleanup after durable publication (F7) -----------------------------------

TEST_F(PublishFaultTest, UnlinkFailureLeavesGarbageButKeepsTheHandle) {
    FaultSyscalls sys;
    auto db = Prepare(sys);
    const std::size_t rule = sys.FailOn("unlink", "wal-000001.log", EACCES);
    ASSERT_TRUE(db->Put("b", "2").ok());
    EXPECT_EQ(sys.rules[rule].fired, 1u);
    EXPECT_EQ(Manifest().generation, 2u);
    EXPECT_TRUE(fs::exists(Db() / "wal-000001.log"));
    // Nothing removed, so no cleanup directory sync.
    EXPECT_EQ(sys.Count("fsync", "db"), 2u);
    EXPECT_EQ(Lookup(*db, "a"), "1");
    EXPECT_EQ(Lookup(*db, "b"), "2");

    // The next publication retries the deferred removal.
    ASSERT_TRUE(db->Put("c", "3").ok());
    EXPECT_EQ(Entries(Db()),
              (std::vector<std::string>{"LOCK", "MANIFEST", "sst-000002.sst",
                                        "sst-000004.sst", "wal-000005.log"}));
    db.reset();
    ASSERT_TRUE(OpenReal(db).ok());
    EXPECT_EQ(Lookup(*db, "b"), "2");
    EXPECT_EQ(Lookup(*db, "c"), "3");
}

TEST_F(PublishFaultTest, OldWalCloseFailureIsNotFatal) {
    FaultSyscalls sys;
    auto db = Prepare(sys);
    const std::size_t rule = sys.FailOn("close", "wal-000001.log", EIO);
    ASSERT_TRUE(db->Put("b", "2").ok());
    EXPECT_EQ(sys.rules[rule].fired, 1u);
    EXPECT_FALSE(fs::exists(Db() / "wal-000001.log"));
    ASSERT_TRUE(db->Put("c", "3").ok());
}

TEST_F(PublishFaultTest, CleanupDirectorySyncFailureRequiresRecovery) {
    for (int err : {EIO, EINTR}) {
        SCOPED_TRACE(err);
        fs::remove_all(Db());
        {
            FaultSyscalls sys;
            auto db = Prepare(sys);
            // Third directory sync of the flush: after the unlink.
            const std::size_t rule = sys.FailOn("fsync", "db", err, 2);
            const Status s = db->Put("b", "2");
            EXPECT_EQ(s.code, Code::IOError) << s.ToString();
            EXPECT_EQ(sys.rules[rule].fired, 1u);
            EXPECT_EQ(sys.Count("fsync", "db"), 3u);  // never retried
            EXPECT_EQ(sys.Count("unlink", "wal-000001.log"), 1u);
            EXPECT_EQ(sys.Count("write", "wal-000003.log"), 0u);
            ExpectInvalidated(*db, sys);
        }
        EXPECT_FALSE(fs::exists(Db() / "wal-000001.log"));
        ExpectRecovered(2);
    }
}

// A flush whose MANIFEST rename happened but whose directory sync failed
// (or never ran, after a crash) leaves a visible generation that is not
// known to be durable. The next Open must make it durable before it
// retires files that generation superseded or acknowledges any write.
class UnsyncedManifestTest : public PublishFaultTest {
protected:
    void SetUp() override {
        FaultSyscalls sys;
        auto db = Prepare(sys);
        ASSERT_NE(db, nullptr);
        sys.FailOn("fsync", "db", EIO, /*skip=*/1);  // the F5 sync
        ASSERT_EQ(db->Put("b", "2").code, Code::IOError);
        db.reset();
        ASSERT_EQ(Manifest().generation, 2u);  // the rename took effect
        ASSERT_TRUE(fs::exists(Db() / "wal-000001.log"));  // not retired
    }
};

TEST_F(UnsyncedManifestTest, OpenSyncsDirectoryBeforeRetiringOrAcknowledging) {
    FaultSyscalls sys;
    sys.armed = true;
    std::unique_ptr<DB> db;
    ASSERT_TRUE(OpenWith(sys, db).ok());
    const std::size_t sync = sys.IndexOf("fsync", "db");
    ASSERT_LT(sync, sys.trace.size()) << "Open never synced the directory";
    EXPECT_LT(sync, sys.IndexOf("unlink", "wal-000001.log"));
    EXPECT_FALSE(fs::exists(Db() / "wal-000001.log"));

    ASSERT_TRUE(db->Put("c", "3").ok());
    EXPECT_LT(sync, sys.IndexOf("write", "wal-000003.log"));
}

TEST_F(UnsyncedManifestTest, FailedOpenDirectorySyncRetiresNothing) {
    for (int err : {EIO, EINTR}) {
        SCOPED_TRACE(err);
        FaultSyscalls sys;
        sys.armed = true;
        const std::size_t rule = sys.FailOn("fsync", "db", err);
        std::unique_ptr<DB> db;
        const Status s = OpenWith(sys, db);
        EXPECT_EQ(s.code, Code::IOError) << s.ToString();
        EXPECT_EQ(sys.rules[rule].fired, 1u);
        EXPECT_EQ(sys.unlinks, 0u);
        EXPECT_EQ(db, nullptr);
        EXPECT_TRUE(fs::exists(Db() / "wal-000001.log"));
        ExpectLockReleased();
    }
    ExpectRecovered(2);
}

// ---- Ordering of a successful flush --------------------------------------------

TEST_F(PublishFaultTest, FlushOrderSyncsEachDependencyOnceBeforePublication) {
    FaultSyscalls sys;
    auto db = Prepare(sys);
    ASSERT_TRUE(db->Put("b", "2").ok());

    const std::vector<std::string> expected_order = {
        "open sst-000002.sst.tmp",
        "write sst-000002.sst.tmp",
        "fsync sst-000002.sst.tmp",
        "close sst-000002.sst.tmp",
        "rename sst-000002.sst.tmp -> sst-000002.sst",
        "write wal-000003.log.tmp",
        "fsync wal-000003.log.tmp",
        "rename wal-000003.log.tmp -> wal-000003.log",
        "open wal-000003.log",
        "fsync db",
        "write MANIFEST.tmp",
        "fsync MANIFEST.tmp",
        "rename MANIFEST.tmp -> MANIFEST",
        "close wal-000001.log",
        "unlink wal-000001.log",
        "write wal-000003.log",
        "fsync wal-000003.log",
    };
    std::size_t at = 0;
    for (const std::string& step : expected_order) {
        const auto it = std::find(sys.trace.begin() + at, sys.trace.end(), step);
        ASSERT_NE(it, sys.trace.end()) << "missing or out of order: " << step;
        at = static_cast<std::size_t>(it - sys.trace.begin()) + 1;
    }
    // The second directory sync publishes MANIFEST; the third persists
    // the cleanup. Each file is synced exactly once.
    const std::size_t manifest_rename =
        sys.IndexOf("rename", "MANIFEST.tmp -> MANIFEST");
    const std::size_t unlink = sys.IndexOf("unlink", "wal-000001.log");
    std::vector<std::size_t> dir_syncs;
    for (std::size_t i = 0; i < sys.trace.size(); ++i) {
        if (sys.trace[i] == "fsync db") {
            dir_syncs.push_back(i);
        }
    }
    ASSERT_EQ(dir_syncs.size(), 3u);
    EXPECT_GT(dir_syncs[1], manifest_rename);
    EXPECT_LT(dir_syncs[1], unlink);
    EXPECT_GT(dir_syncs[2], unlink);
    EXPECT_EQ(sys.Count("fsync", "sst-000002.sst.tmp"), 1u);
    EXPECT_EQ(sys.Count("fsync", "wal-000003.log.tmp"), 1u);
    EXPECT_EQ(sys.Count("fsync", "MANIFEST.tmp"), 1u);
    EXPECT_EQ(sys.Count("fsync", "wal-000003.log"), 1u);  // the record
}

// ---- Initialization publication faults ------------------------------------------

TEST_F(PublishFaultTest, InitializationFaultsExposeNoHandle) {
    // reopen: the result of a later real Open. Before the MANIFEST rename,
    // created files are residue (Corruption, preserved). If nothing but
    // LOCK was created, the directory is still uninitialized (Ok).
    const struct {
        const char* op;
        const char* name;
        std::size_t skip;
        Code reopen;
    } cases[] = {
        {"open", "wal-000001.log.tmp", 0, Code::Ok},
        {"write", "wal-000001.log.tmp", 0, Code::Corruption},
        {"fsync", "wal-000001.log.tmp", 0, Code::Corruption},
        {"close", "wal-000001.log.tmp", 0, Code::Corruption},
        {"rename", "wal-000001.log", 0, Code::Corruption},
        {"fsync", "db", 0, Code::Corruption},
        {"open", "MANIFEST.tmp", 0, Code::Corruption},
        {"write", "MANIFEST.tmp", 0, Code::Corruption},
        {"fsync", "MANIFEST.tmp", 0, Code::Corruption},
        {"close", "MANIFEST.tmp", 0, Code::Corruption},
        {"rename", "MANIFEST", 0, Code::Corruption},
        {"fsync", "db", 1, Code::Ok},
        {"open", "wal-000001.log", 0, Code::Ok},
    };
    for (const auto& c : cases) {
        SCOPED_TRACE(std::string(c.op) + " " + c.name + " skip " +
                     std::to_string(c.skip));
        fs::remove_all(Db());
        fs::create_directory(Db());
        FaultSyscalls sys;
        sys.armed = true;
        const std::size_t rule = sys.FailOn(c.op, c.name, EIO, c.skip);
        std::unique_ptr<DB> db;
        const Status s = OpenWith(sys, db);
        EXPECT_EQ(s.code, Code::IOError) << s.ToString();
        EXPECT_EQ(sys.rules[rule].fired, 1u);
        EXPECT_EQ(db, nullptr);
        ExpectLockReleased();

        const std::vector<std::string> left = Entries(Db());
        const Status r = OpenReal(db);
        EXPECT_EQ(r.code, c.reopen) << r.ToString();
        if (r.ok()) {
            ASSERT_TRUE(db->Put("a", "1").ok());
            EXPECT_EQ(Lookup(*db, "a"), "1");
        } else {
            EXPECT_EQ(Entries(Db()), left);
        }
    }
}

// ---- SST read failures ------------------------------------------------------------

TEST_F(PublishFaultTest, SstReadFailureAtOpenExposesNoHandle) {
    {
        std::unique_ptr<DB> db;
        ASSERT_TRUE(OpenReal(db).ok());
        ASSERT_TRUE(db->Put("a", "1").ok());
        ASSERT_TRUE(db->Put("b", "2").ok());
    }
    FaultSyscalls sys;
    sys.armed = true;
    const std::size_t rule = sys.FailOn("pread", "sst-000002.sst", EIO);
    std::unique_ptr<DB> db;
    EXPECT_EQ(OpenWith(sys, db).code, Code::IOError);
    EXPECT_EQ(sys.rules[rule].fired, 1u);
    EXPECT_EQ(db, nullptr);
    ExpectLockReleased();
    ASSERT_TRUE(OpenReal(db).ok());
    EXPECT_EQ(Lookup(*db, "a"), "1");
}

TEST_F(PublishFaultTest, SstReadFailureDuringGetKeepsTheHandle) {
    FaultSyscalls sys;
    auto db = Prepare(sys);
    ASSERT_TRUE(db->Put("b", "2").ok());  // a is now only in sst 2

    const std::size_t rule = sys.FailOn("pread", "sst-000002.sst", EIO);
    std::string out = "stale";
    const Status s = db->Get("a", out);
    EXPECT_EQ(s.code, Code::IOError) << s.ToString();
    EXPECT_TRUE(out.empty());
    EXPECT_EQ(sys.rules[rule].fired, 1u);

    // Nothing durable changed, so the handle remains usable.
    EXPECT_EQ(Lookup(*db, "a"), "1");
    ASSERT_TRUE(db->Put("c", "3").ok());
    EXPECT_EQ(Lookup(*db, "c"), "3");
}

}  // namespace
}  // namespace kv
