#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "core/db.h"
#include "core/db_internal.h"
#include "core/file_lock.h"
#include "testing/fault_syscalls.h"
#include "testing/temp_dir.h"
#include "testing/wal_builder.h"

namespace kv {
namespace {

namespace fs = std::filesystem;

using test::FaultSyscalls;
using test::kRawPut;
using test::RawRecord;
using test::ReadBytesOrEmpty;
using test::WalFileHeaderBytes;
using test::WriteBytes;

// These tests inject syscall results through the internal construction
// path and check the engine's control flow and recovered state. They do
// not model device power loss: the kernel page cache survives every
// "failure" here.

bool Contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

std::string ErrnoText(int err) {
    return std::generic_category().message(err);
}

using Model = std::map<std::string, std::string>;

struct Mutation {
    bool is_put;
    std::string key;
    std::string value;
};

Model Apply(Model model, const Mutation& m) {
    if (m.is_put) {
        model[m.key] = m.value;
    } else {
        model.erase(m.key);
    }
    return model;
}

// Reads every key of interest; absent keys are omitted from the result.
Model Snapshot(const DB& db, const std::vector<std::string>& keys) {
    Model model;
    for (const std::string& key : keys) {
        std::string out;
        const Status s = db.Get(key, out);
        if (s.ok()) {
            model[key] = out;
        } else {
            EXPECT_EQ(s.code, Code::NotFound) << s.ToString();
        }
    }
    return model;
}

class DBFaultTest : public ::testing::Test {
protected:
    fs::path Db() const { return tmp_.path() / "db"; }
    fs::path Wal() const { return Db() / "000001.wal"; }
    fs::path Temp() const { return Db() / "000001.wal.tmp"; }

    static std::string Canonical(const fs::path& p) {
        return fs::weakly_canonical(p).string();
    }

    Status OpenWith(FaultSyscalls& sys,
                    std::unique_ptr<DB>& db,
                    bool create = true) {
        Options options;
        options.create_if_missing = create;
        return DBOpener::Open(Db(), options, sys, db);
    }

    Status OpenReal(std::unique_ptr<DB>& db, bool create = true) {
        Options options;
        options.create_if_missing = create;
        return DB::Open(Db(), options, db);
    }

    // After a failed Open, the lock must have been released.
    void ExpectLockReleased() {
        std::unique_ptr<FileLock> lock;
        EXPECT_TRUE(LockFile(Db() / "LOCK", lock).ok());
    }

    // After the first failure: every call returns NeedsRecovery, Get clears
    // its output, and no syscall reaches the file.
    void ExpectInvalidated(DB& db, const FaultSyscalls& sys) {
        const std::size_t calls = sys.TotalCalls();
        EXPECT_EQ(db.Put("other", "v").code, Code::NeedsRecovery);
        EXPECT_EQ(db.Delete("k").code, Code::NeedsRecovery);
        std::string out = "stale";
        EXPECT_EQ(db.Get("k", out).code, Code::NeedsRecovery);
        EXPECT_TRUE(out.empty());
        out = "stale";
        EXPECT_EQ(db.Get("never", out).code, Code::NeedsRecovery);
        EXPECT_TRUE(out.empty());
        EXPECT_EQ(sys.TotalCalls(), calls);
    }

    test::TempDir tmp_;
};

const std::vector<std::string> kKeys = {"k", "x", "other"};

// ---- Write loop through the DB path -----------------------------------------

TEST_F(DBFaultTest, ShortWritesAndEintrStillWriteExactRecord) {
    FaultSyscalls sys;
    std::unique_ptr<DB> db;
    ASSERT_TRUE(OpenWith(sys, db).ok());

    sys.armed = true;
    sys.write_script = {FaultSyscalls::Short(3), FaultSyscalls::Error(EINTR),
                        FaultSyscalls::Short(10), FaultSyscalls::Error(EINTR)};
    ASSERT_TRUE(db->Put("key", "value").ok());
    EXPECT_GE(sys.writes, 5u);
    EXPECT_EQ(sys.fsyncs, 1u);
    db.reset();

    EXPECT_EQ(ReadBytesOrEmpty(Wal()),
              WalFileHeaderBytes() + RawRecord(1, kRawPut, "key", "value"));
    ASSERT_TRUE(OpenReal(db).ok());
    EXPECT_EQ(Snapshot(*db, {"key"}), (Model{{"key", "value"}}));
}

// ---- Append and Sync failures invalidate the handle -------------------------

struct AppendFault {
    const char* name;
    std::vector<FaultSyscalls::Step> script;
    int err;
};

TEST_F(DBFaultTest, AppendFailureInvalidatesHandleAndRecoversPrefix) {
    const AppendFault faults[] = {
        {"partial then EIO",
         {FaultSyscalls::Short(5), FaultSyscalls::Error(EIO)}, EIO},
        {"ENOSPC before progress", {FaultSyscalls::Error(ENOSPC)}, ENOSPC},
        {"EIO before progress", {FaultSyscalls::Error(EIO)}, EIO},
    };
    for (const AppendFault& f : faults) {
        SCOPED_TRACE(f.name);
        fs::remove_all(Db());
        const Mutation failing{true, "k", "new"};
        Model before;
        std::size_t size_before = 0;
        {
            FaultSyscalls sys;
            std::unique_ptr<DB> db;
            ASSERT_TRUE(OpenWith(sys, db).ok());
            ASSERT_TRUE(db->Put("k", "old").ok());
            ASSERT_TRUE(db->Put("x", "1").ok());
            before = {{"k", "old"}, {"x", "1"}};
            size_before = fs::file_size(Wal());

            sys.armed = true;
            sys.write_script.assign(f.script.begin(), f.script.end());
            const Status s = db->Put(failing.key, failing.value);
            EXPECT_EQ(s.code, Code::IOError) << s.ToString();
            EXPECT_TRUE(Contains(s.message, ErrnoText(f.err))) << s.message;
            EXPECT_EQ(sys.fsyncs, 0u);  // no sync after a failed append
            ExpectInvalidated(*db, sys);
        }

        // Reopen with the real filesystem. Any torn bytes form an
        // incomplete tail, which recovery truncates.
        std::unique_ptr<DB> db;
        ASSERT_TRUE(OpenReal(db).ok());
        EXPECT_EQ(Snapshot(*db, kKeys), before);
        EXPECT_EQ(fs::file_size(Wal()), size_before);
        ASSERT_TRUE(db->Put("after", "ok").ok());
    }
}

TEST_F(DBFaultTest, SyncFailureInvalidatesHandleWithoutRetry) {
    for (int err : {EIO, ENOSPC, EINTR}) {
        SCOPED_TRACE(ErrnoText(err));
        fs::remove_all(Db());
        // A Delete after a Put of the same key: judged in log order, the
        // recovered state is either before or after the in-flight Delete.
        const Mutation failing{false, "k", ""};
        Model before;
        {
            FaultSyscalls sys;
            std::unique_ptr<DB> db;
            ASSERT_TRUE(OpenWith(sys, db).ok());
            ASSERT_TRUE(db->Put("k", "old").ok());
            ASSERT_TRUE(db->Put("x", "1").ok());
            before = {{"k", "old"}, {"x", "1"}};

            sys.armed = true;
            sys.fsync_errno = err;
            const Status s = db->Delete(failing.key);
            EXPECT_EQ(s.code, Code::IOError) << s.ToString();
            EXPECT_TRUE(Contains(s.message, "fsync")) << s.message;
            EXPECT_EQ(sys.fsyncs, 1u);  // never retried
            ExpectInvalidated(*db, sys);
        }

        std::unique_ptr<DB> db;
        ASSERT_TRUE(OpenReal(db).ok());
        const Model recovered = Snapshot(*db, kKeys);
        const Model with_failed = Apply(before, failing);
        EXPECT_TRUE(recovered == before || recovered == with_failed);
    }
}

TEST_F(DBFaultTest, OverwriteWithFailedSyncRecoversOldOrNewValue) {
    {
        FaultSyscalls sys;
        std::unique_ptr<DB> db;
        ASSERT_TRUE(OpenWith(sys, db).ok());
        ASSERT_TRUE(db->Put("k", "old").ok());
        sys.armed = true;
        sys.fsync_errno = EIO;
        EXPECT_EQ(db->Put("k", "new").code, Code::IOError);
        ExpectInvalidated(*db, sys);
    }
    std::unique_ptr<DB> db;
    ASSERT_TRUE(OpenReal(db).ok());
    const Model recovered = Snapshot(*db, {"k"});
    EXPECT_TRUE(recovered == (Model{{"k", "old"}}) ||
                recovered == (Model{{"k", "new"}}));
}

TEST_F(DBFaultTest, ExceptionAfterPersistenceBeginsInvalidatesHandle) {
    for (bool in_sync : {false, true}) {
        SCOPED_TRACE(in_sync ? "fsync throws" : "write throws");
        fs::remove_all(Db());
        FaultSyscalls sys;
        std::unique_ptr<DB> db;
        ASSERT_TRUE(OpenWith(sys, db).ok());
        ASSERT_TRUE(db->Put("k", "old").ok());

        sys.armed = true;
        if (in_sync) {
            sys.fsync_throw = true;
        } else {
            sys.write_script = {FaultSyscalls::Throw()};
        }
        EXPECT_THROW(db->Put("k", "new"), std::runtime_error);
        sys.fsync_throw = false;
        ExpectInvalidated(*db, sys);
    }
}

// ---- Initialization failures expose no handle -------------------------------

TEST_F(DBFaultTest, HeaderWriteFailureDuringInitialization) {
    FaultSyscalls sys;
    sys.armed = true;
    sys.write_script = {FaultSyscalls::Short(4), FaultSyscalls::Error(ENOSPC)};
    std::unique_ptr<DB> db;
    const Status s = OpenWith(sys, db);
    EXPECT_EQ(s.code, Code::IOError) << s.ToString();
    EXPECT_EQ(db, nullptr);
    EXPECT_FALSE(fs::exists(Wal()));
    ExpectLockReleased();

    // The partial temporary file is recognized and replaced.
    ASSERT_TRUE(OpenReal(db).ok());
    EXPECT_EQ(ReadBytesOrEmpty(Wal()), WalFileHeaderBytes());
    EXPECT_FALSE(fs::exists(Temp()));
}

TEST_F(DBFaultTest, TempFileSyncFailureDoesNotPublishWal) {
    fs::create_directory(Db());
    FaultSyscalls sys;
    sys.armed = true;
    sys.fsync_errno = EIO;
    sys.fsync_only_target = Canonical(Temp());

    std::unique_ptr<DB> db;
    const Status s = OpenWith(sys, db);
    EXPECT_EQ(s.code, Code::IOError) << s.ToString();
    EXPECT_EQ(db, nullptr);
    EXPECT_EQ(sys.fsyncs, 1u);
    EXPECT_FALSE(fs::exists(Wal()));
    EXPECT_TRUE(fs::exists(Temp()));
    ExpectLockReleased();

    // Uninitialized with only LOCK and tmp: create_if_missing decides.
    EXPECT_EQ(OpenReal(db, false).code, Code::NotFound);
    EXPECT_TRUE(fs::exists(Temp()));
    ASSERT_TRUE(OpenReal(db, true).ok());
    EXPECT_FALSE(fs::exists(Temp()));
}

TEST_F(DBFaultTest, DirectorySyncFailureAfterRenameExposesNoHandle) {
    fs::create_directory(Db());
    FaultSyscalls sys;
    sys.armed = true;
    sys.fsync_errno = EIO;
    sys.fsync_only_target = Canonical(Db());

    std::unique_ptr<DB> db;
    const Status s = OpenWith(sys, db);
    EXPECT_EQ(s.code, Code::IOError) << s.ToString();
    EXPECT_TRUE(Contains(s.message, "fsync directory")) << s.message;
    EXPECT_EQ(db, nullptr);
    ExpectLockReleased();

    // The rename already happened; an error is not a rollback.
    EXPECT_EQ(ReadBytesOrEmpty(Wal()), WalFileHeaderBytes());
    ASSERT_TRUE(OpenReal(db, false).ok());
    ASSERT_TRUE(db->Put("k", "v").ok());
}

TEST_F(DBFaultTest, ParentSyncFailureAfterCreatingDirectory) {
    FaultSyscalls sys;
    sys.armed = true;
    sys.fsync_errno = EIO;
    sys.fsync_only_target = Canonical(tmp_.path());

    std::unique_ptr<DB> db;
    const Status s = OpenWith(sys, db);
    EXPECT_EQ(s.code, Code::IOError) << s.ToString();
    EXPECT_EQ(db, nullptr);
    EXPECT_TRUE(fs::is_directory(Db()));
    EXPECT_TRUE(fs::is_empty(Db()));  // failed before LOCK or WAL

    EXPECT_EQ(OpenReal(db, false).code, Code::NotFound);
    ASSERT_TRUE(OpenReal(db, true).ok());
}

// ---- Tail repair failures expose no handle ----------------------------------

class TornTail : public DBFaultTest {
protected:
    void SetUp() override {
        good_ = WalFileHeaderBytes() + RawRecord(1, kRawPut, "k", "v");
        torn_ = good_ + RawRecord(2, kRawPut, "k", "w").substr(0, 20);
        fs::create_directory(Db());
        ASSERT_TRUE(WriteBytes(Wal(), torn_));
    }

    std::string good_;
    std::string torn_;
};

TEST_F(TornTail, SyncFailureDuringRepairExposesNoHandle) {
    for (int err : {EIO, EINTR}) {
        SCOPED_TRACE(ErrnoText(err));
        ASSERT_TRUE(WriteBytes(Wal(), torn_));
        FaultSyscalls sys;
        sys.armed = true;
        sys.fsync_errno = err;

        std::unique_ptr<DB> db;
        const Status s = OpenWith(sys, db, false);
        EXPECT_EQ(s.code, Code::IOError) << s.ToString();
        EXPECT_TRUE(Contains(s.message, "fsync after truncate")) << s.message;
        EXPECT_EQ(db, nullptr);
        EXPECT_EQ(sys.fsyncs, 1u);
        EXPECT_EQ(sys.writes, 0u);  // nothing was appended
        ExpectLockReleased();
        // Truncation already took effect; it is not rolled back.
        EXPECT_EQ(ReadBytesOrEmpty(Wal()), good_);
    }

    std::unique_ptr<DB> db;
    ASSERT_TRUE(OpenReal(db, false).ok());
    EXPECT_EQ(Snapshot(*db, {"k"}), (Model{{"k", "v"}}));
}

TEST_F(TornTail, TruncateFailureLeavesWalAndExposesNoHandle) {
    FaultSyscalls sys;
    sys.armed = true;
    sys.ftruncate_errnos = {EIO};

    std::unique_ptr<DB> db;
    const Status s = OpenWith(sys, db, false);
    EXPECT_EQ(s.code, Code::IOError) << s.ToString();
    EXPECT_EQ(db, nullptr);
    EXPECT_EQ(sys.writes, 0u);
    EXPECT_EQ(sys.fsyncs, 0u);
    EXPECT_EQ(ReadBytesOrEmpty(Wal()), torn_);
    ExpectLockReleased();

    ASSERT_TRUE(OpenReal(db, false).ok());
    EXPECT_EQ(ReadBytesOrEmpty(Wal()), good_);
}

TEST_F(TornTail, ShortReadsAndEintrDuringRecovery) {
    FaultSyscalls sys;
    sys.armed = true;
    sys.read_script = {FaultSyscalls::Short(1), FaultSyscalls::Error(EINTR),
                       FaultSyscalls::Short(7), FaultSyscalls::Error(EINTR),
                       FaultSyscalls::Short(30)};
    std::unique_ptr<DB> db;
    ASSERT_TRUE(OpenWith(sys, db, false).ok());
    // Recovery really read through the seam and consumed the script.
    EXPECT_TRUE(sys.read_script.empty());
    EXPECT_GE(sys.reads, 6u);
    EXPECT_EQ(Snapshot(*db, {"k"}), (Model{{"k", "v"}}));
    EXPECT_EQ(ReadBytesOrEmpty(Wal()), good_);
}

TEST_F(TornTail, RepairIsTruncatedAndSyncedBeforeFirstAppend) {
    FaultSyscalls sys;
    sys.armed = true;
    std::unique_ptr<DB> db;
    ASSERT_TRUE(OpenWith(sys, db, false).ok());
    ASSERT_TRUE(db->Put("k", "w").ok());

    const auto& ev = sys.events;
    const auto at = [&](const char* name) {
        return std::find(ev.begin(), ev.end(), name) - ev.begin();
    };
    const auto truncate_at = at("ftruncate");
    const auto sync_at = at("fsync");
    const auto write_at = at("write");
    ASSERT_LT(truncate_at, static_cast<std::ptrdiff_t>(ev.size()));
    ASSERT_LT(write_at, static_cast<std::ptrdiff_t>(ev.size()));
    EXPECT_LT(truncate_at, sync_at);
    EXPECT_LT(sync_at, write_at);
    EXPECT_EQ(sys.fsync_targets.front(), Canonical(Wal()));
    db.reset();

    EXPECT_EQ(ReadBytesOrEmpty(Wal()),
              good_ + RawRecord(2, kRawPut, "k", "w"));
}

TEST_F(TornTail, ReadErrorDuringRecoveryLeavesWalUntouched) {
    FaultSyscalls sys;
    sys.armed = true;
    sys.read_script = {FaultSyscalls::Short(10), FaultSyscalls::Error(EIO)};
    std::unique_ptr<DB> db;
    const Status s = OpenWith(sys, db, false);
    EXPECT_EQ(s.code, Code::IOError) << s.ToString();
    EXPECT_EQ(db, nullptr);
    EXPECT_EQ(sys.ftruncates, 0u);
    EXPECT_EQ(ReadBytesOrEmpty(Wal()), torn_);
    ExpectLockReleased();
}

}  // namespace
}  // namespace kv
