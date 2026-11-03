#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>

#include <cerrno>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/file_lock.h"
#include "core/posix_dir.h"
#include "core/posix_file.h"
#include "testing/crash_sim.h"

namespace kv::test {
namespace {

// Simulator semantics, checked with raw seam calls only (no engine code),
// so that the engine and the simulator cannot share one wrong assumption.

constexpr int kCreate = O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC;

int OpenDir(CrashSimSyscalls& sim, const char* path) {
    return sim.Open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
}

void WriteAll(CrashSimSyscalls& sim, int fd, const std::string& bytes) {
    ASSERT_EQ(sim.Write(fd, bytes.data(), bytes.size()),
              static_cast<ssize_t>(bytes.size()));
}

void SyncDir(CrashSimSyscalls& sim, const char* path) {
    const int fd = OpenDir(sim, path);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(sim.Fsync(fd), 0);
    ASSERT_EQ(sim.Close(fd), 0);
}

std::string Contents(const SimFs& fs, const std::string& path) {
    CrashSimSyscalls view(fs);
    std::string out;
    return view.ReadFile(path, out) ? out : "<missing>";
}

class CrashSimTest : public ::testing::Test {
protected:
    void SetUp() override {
        sim_.MakeDirs("/d");
    }

    // Creates /d/<name> with bytes; optionally syncs the file.
    void Create(const std::string& name, const std::string& bytes,
                bool sync_file) {
        const int fd = sim_.Open(("/d/" + name).c_str(), kCreate, 0644);
        ASSERT_GE(fd, 0);
        WriteAll(sim_, fd, bytes);
        if (sync_file) {
            ASSERT_EQ(sim_.Fsync(fd), 0);
        }
        ASSERT_EQ(sim_.Close(fd), 0);
    }

    SimFs Lost() const {
        return CrashSimSyscalls::Recover(sim_.state(), CrashModel::kUnsyncedLoss);
    }

    SimFs Kept() const {
        return CrashSimSyscalls::Recover(sim_.state(), CrashModel::kProcess);
    }

    CrashSimSyscalls sim_;
};

TEST_F(CrashSimTest, FileSyncAndDirectorySyncPersistDifferentThings) {
    // Contents synced, name not: the file is lost with unsynced state.
    Create("contents_only", "abc", /*sync_file=*/true);
    EXPECT_EQ(Contents(Lost(), "/d/contents_only"), "<missing>");
    EXPECT_EQ(Contents(Kept(), "/d/contents_only"), "abc");

    // Name synced, contents not: the file survives empty.
    Create("name_only", "xyz", /*sync_file=*/false);
    SyncDir(sim_, "/d");
    EXPECT_EQ(Contents(Lost(), "/d/name_only"), "");
    EXPECT_EQ(Contents(Lost(), "/d/contents_only"), "abc");  // now named

    // A later directory sync does not substitute for a file sync.
    SyncDir(sim_, "/d");
    EXPECT_EQ(Contents(Lost(), "/d/name_only"), "");
    EXPECT_EQ(Contents(Kept(), "/d/name_only"), "xyz");
}

TEST_F(CrashSimTest, AppendAfterSyncLosesOnlyTheUnsyncedSuffix) {
    Create("f", "old", true);
    SyncDir(sim_, "/d");
    const int fd = sim_.Open("/d/f", O_WRONLY | O_APPEND, 0);
    ASSERT_GE(fd, 0);
    WriteAll(sim_, fd, "+new");
    EXPECT_EQ(Contents(Lost(), "/d/f"), "old");
    EXPECT_EQ(Contents(Kept(), "/d/f"), "old+new");
    ASSERT_EQ(sim_.Fsync(fd), 0);
    EXPECT_EQ(Contents(Lost(), "/d/f"), "old+new");
    ASSERT_EQ(sim_.Close(fd), 0);
}

TEST_F(CrashSimTest, RenameReplacesAtomicallyAndNeedsADirectorySync) {
    Create("final", "old", true);
    SyncDir(sim_, "/d");
    Create("final.tmp", "new", true);
    ASSERT_EQ(sim_.Rename("/d/final.tmp", "/d/final"), 0);
    EXPECT_EQ(sim_.Names("/d"), (std::vector<std::string>{"final"}));
    EXPECT_EQ(Contents(Kept(), "/d/final"), "new");
    // The rename is not durable yet: the old file is back after the loss,
    // and the never-durable temporary name is gone.
    EXPECT_EQ(Contents(Lost(), "/d/final"), "old");
    EXPECT_EQ(Contents(Lost(), "/d/final.tmp"), "<missing>");

    SyncDir(sim_, "/d");
    EXPECT_EQ(Contents(Lost(), "/d/final"), "new");
    EXPECT_EQ(CrashSimSyscalls(Lost()).Names("/d"),
              (std::vector<std::string>{"final"}));

    EXPECT_EQ(sim_.Rename("/d/missing", "/d/x"), -1);
    EXPECT_EQ(errno, ENOENT);
}

TEST_F(CrashSimTest, TruncateAndUnlinkAreVolatileUntilSynced) {
    Create("f", "0123456789", true);
    SyncDir(sim_, "/d");
    int fd = sim_.Open("/d/f", O_WRONLY, 0);
    ASSERT_EQ(sim_.Ftruncate(fd, 4), 0);
    EXPECT_EQ(Contents(Kept(), "/d/f"), "0123");
    EXPECT_EQ(Contents(Lost(), "/d/f"), "0123456789");
    ASSERT_EQ(sim_.Fsync(fd), 0);
    EXPECT_EQ(Contents(Lost(), "/d/f"), "0123");
    ASSERT_EQ(sim_.Close(fd), 0);

    ASSERT_EQ(sim_.Unlink("/d/f"), 0);
    EXPECT_FALSE(sim_.Exists("/d/f"));
    EXPECT_EQ(Contents(Lost(), "/d/f"), "0123");  // the unlink was not synced
    SyncDir(sim_, "/d");
    EXPECT_EQ(Contents(Lost(), "/d/f"), "<missing>");

    EXPECT_EQ(sim_.Unlink("/d"), -1);
    EXPECT_EQ(errno, EISDIR);
}

TEST_F(CrashSimTest, NewDirectoryNeedsItsParentSynced) {
    ASSERT_EQ(sim_.Mkdir("/d/sub", 0755), 0);
    EXPECT_EQ(sim_.Mkdir("/d/sub", 0755), -1);
    EXPECT_EQ(errno, EEXIST);
    const int fd = sim_.Open("/d/sub/f", kCreate, 0644);
    ASSERT_GE(fd, 0);
    WriteAll(sim_, fd, "data");
    ASSERT_EQ(sim_.Fsync(fd), 0);
    ASSERT_EQ(sim_.Close(fd), 0);
    SyncDir(sim_, "/d/sub");
    // The file and its entry are durable, but the path to them is not.
    EXPECT_FALSE(CrashSimSyscalls(Lost()).Exists("/d/sub"));
    EXPECT_EQ(Contents(Lost(), "/d/sub/f"), "<missing>");
    SyncDir(sim_, "/d");
    EXPECT_EQ(Contents(Lost(), "/d/sub/f"), "data");

    EXPECT_EQ(sim_.Mkdir("/missing/x", 0755), -1);
    EXPECT_EQ(errno, ENOENT);
}

TEST_F(CrashSimTest, LocksBelongToDescriptorsAndDoNotSurviveACrash) {
    const int a = sim_.Open("/d/LOCK", O_RDONLY | O_CREAT, 0644);
    const int b = sim_.Open("/d/LOCK", O_RDONLY | O_CREAT, 0644);
    ASSERT_EQ(sim_.Flock(a, LOCK_EX | LOCK_NB), 0);
    EXPECT_EQ(sim_.Flock(b, LOCK_EX | LOCK_NB), -1);
    EXPECT_EQ(errno, EWOULDBLOCK);

    // A new instance (after any crash) has no locks or descriptors.
    CrashSimSyscalls after(Kept());
    const int c = after.Open("/d/LOCK", O_RDONLY | O_CREAT, 0644);
    EXPECT_EQ(after.Flock(c, LOCK_EX | LOCK_NB), 0);
    EXPECT_EQ(after.open_descriptors(), 1u);

    ASSERT_EQ(sim_.Close(a), 0);  // close releases the lock
    EXPECT_EQ(sim_.Flock(b, LOCK_EX | LOCK_NB), 0);
    ASSERT_EQ(sim_.Flock(b, LOCK_UN), 0);
    ASSERT_EQ(sim_.Close(b), 0);

    // The engine's lock helper works on the simulator too.
    std::unique_ptr<FileLock> first, second;
    ASSERT_TRUE(LockFile("/d/LOCK", first, sim_).ok());
    EXPECT_EQ(LockFile("/d/LOCK", second, sim_).code, Code::Busy);
    first.reset();
    EXPECT_TRUE(LockFile("/d/LOCK", second, sim_).ok());
}

TEST_F(CrashSimTest, CrashFreezesTheSnapshotAndKillsTheInstance) {
    sim_.CrashAt(3);
    const int fd = sim_.Open("/d/f", kCreate, 0644);  // point 1
    ASSERT_GE(fd, 0);
    WriteAll(sim_, fd, "one");                        // point 2
    bool crashed = false;
    try {
        sim_.Write(fd, "two", 3);                     // point 3
    } catch (const SimulatedCrash& crash) {
        crashed = true;
        EXPECT_EQ(crash.index, 3u);
        EXPECT_EQ(crash.point, "write f");
    }
    ASSERT_TRUE(crashed);
    EXPECT_TRUE(sim_.dead());
    EXPECT_EQ(Contents(sim_.snapshot(), "/d/f"), "onetwo");

    // Everything after the crash fails without changing the snapshot or
    // throwing, the way destructors and error paths would run.
    EXPECT_EQ(sim_.Write(fd, "three", 5), -1);
    EXPECT_EQ(errno, EIO);
    EXPECT_EQ(sim_.Fsync(fd), -1);
    EXPECT_EQ(sim_.Unlink("/d/f"), -1);
    EXPECT_EQ(sim_.Open("/d/g", kCreate, 0644), -1);
    EXPECT_EQ(sim_.Close(fd), 0);
    EXPECT_EQ(sim_.Flock(fd, LOCK_UN), 0);
    sim_.Checkpoint("after");
    EXPECT_EQ(sim_.points(), 3u);
    EXPECT_EQ(Contents(sim_.snapshot(), "/d/f"), "onetwo");
    // Nothing was synced, so the loss model has neither name nor contents.
    EXPECT_EQ(Contents(CrashSimSyscalls::Recover(sim_.snapshot(),
                                                 CrashModel::kUnsyncedLoss),
                       "/d/f"),
              "<missing>");
}

TEST_F(CrashSimTest, CountsOnlyStateChangingCallsAndCheckpoints) {
    const std::uint64_t before = sim_.points();
    const int fd = sim_.Open("/d/f", kCreate, 0644);   // counted
    WriteAll(sim_, fd, "x");                          // counted
    sim_.Write(fd, "", 0);                            // empty: not counted
    struct stat st;
    sim_.Fstat(fd, &st);
    EXPECT_EQ(st.st_size, 1);
    char c;
    EXPECT_EQ(sim_.Pread(fd, &c, 1, 0), -1);  // write-only descriptor
    sim_.Close(fd);
    const int ro = sim_.Open("/d/f", O_RDONLY, 0);    // not counted
    EXPECT_EQ(sim_.Pread(ro, &c, 1, 0), 1);
    EXPECT_EQ(c, 'x');
    sim_.Close(ro);
    std::vector<std::string> names;
    EXPECT_EQ(sim_.ListDirectory("/d", names), 0);
    EXPECT_EQ(names, (std::vector<std::string>{"f"}));
    EXPECT_EQ(sim_.Lstat("/d/f", &st), 0);
    EXPECT_TRUE(S_ISREG(st.st_mode));
    sim_.Checkpoint("named");                         // counted
    EXPECT_EQ(sim_.points() - before, 3u);
    EXPECT_EQ(sim_.point_log().back(), "checkpoint named");

    EXPECT_EQ(sim_.Open("/d/missing", O_RDONLY, 0), -1);
    EXPECT_EQ(errno, ENOENT);
    EXPECT_EQ(sim_.Open("/d/f", kCreate, 0644), -1);
    EXPECT_EQ(errno, EEXIST);
    EXPECT_EQ(sim_.ListDirectory("/d/f", names), -1);
    EXPECT_EQ(errno, ENOTDIR);
    EXPECT_EQ(sim_.Open("/d", O_WRONLY, 0), -1);
    EXPECT_EQ(errno, EISDIR);
    EXPECT_EQ(sim_.points() - before, 3u);
}

TEST_F(CrashSimTest, SkippedFileSyncSuffixKeepsContentsVolatile) {
    sim_.skip_file_sync_suffix = ".sst.tmp";
    Create("x.sst.tmp", "table", true);
    Create("x.log", "wal", true);
    SyncDir(sim_, "/d");
    EXPECT_EQ(Contents(Lost(), "/d/x.sst.tmp"), "");
    EXPECT_EQ(Contents(Lost(), "/d/x.log"), "wal");
    EXPECT_EQ(Contents(Kept(), "/d/x.sst.tmp"), "table");
}

// The engine's directory helpers run unchanged on the simulator.
TEST_F(CrashSimTest, EngineDirectoryHelpersUseTheSeam) {
    bool created = false;
    ASSERT_TRUE(CreateDirectory("/d/db", created, sim_).ok());
    EXPECT_TRUE(created);
    ASSERT_TRUE(CreateDirectory("/d/db", created, sim_).ok());
    EXPECT_FALSE(created);
    PathKind kind;
    ASSERT_TRUE(InspectPath("/d/db", kind, sim_).ok());
    EXPECT_EQ(kind, PathKind::kDirectory);
    ASSERT_TRUE(InspectPath("/d/none", kind, sim_).ok());
    EXPECT_EQ(kind, PathKind::kMissing);
    ASSERT_TRUE(SyncDirectory("/d", sim_).ok());
    std::vector<std::string> names;
    ASSERT_TRUE(ListDirectory("/d", names, sim_).ok());
    EXPECT_EQ(names, (std::vector<std::string>{"db"}));
    EXPECT_TRUE(CrashSimSyscalls(Lost()).Exists("/d/db"));
}

}  // namespace
}  // namespace kv::test
