#include <gtest/gtest.h>

#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "core/file_lock.h"
#include "core/posix_dir.h"

namespace kv {
namespace {

namespace fs = std::filesystem;

// These tests use real files in a private temporary directory, plus a
// scripted fsync/close seam for directory sync failures. They check
// syscall usage and error handling, not durability under power loss.

bool Contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

std::string ErrnoText(int err) {
    return std::generic_category().message(err);
}

std::string ReadFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    EXPECT_TRUE(in.good()) << path;
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

void WriteFileDirectly(const fs::path& path, const std::string& contents) {
    std::ofstream out(path, std::ios::binary);
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    ASSERT_TRUE(out.good()) << path;
}

bool FdIsOpen(int fd) {
    return ::fcntl(fd, F_GETFD) != -1;
}

std::size_t OpenFdCount() {
    std::size_t n = 0;
    for (auto it = fs::directory_iterator("/proc/self/fd");
         it != fs::directory_iterator(); ++it) {
        ++n;
    }
    return n;
}

// Descriptors of this process whose target is path.
std::vector<int> FdsReferring(const fs::path& path) {
    const fs::path target = fs::weakly_canonical(path);
    std::vector<int> fds;
    for (const auto& entry : fs::directory_iterator("/proc/self/fd")) {
        std::error_code ec;
        const fs::path link = fs::read_symlink(entry.path(), ec);
        if (!ec && link == target) {
            fds.push_back(std::stoi(entry.path().filename().string()));
        }
    }
    return fds;
}

fs::path WithNul(const fs::path& dir, const char* prefix) {
    return dir / std::string(std::string(prefix) + '\0' + "suffix");
}

std::vector<std::string> Sorted(std::vector<std::string> v) {
    std::sort(v.begin(), v.end());
    return v;
}

class PosixDirTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::string pattern =
            (fs::temp_directory_path() / "kv_posix_dir_test.XXXXXX").string();
        ASSERT_NE(::mkdtemp(pattern.data()), nullptr) << ErrnoText(errno);
        dir_ = pattern;
    }

    void TearDown() override {
        std::error_code ec;
        fs::permissions(dir_, fs::perms::owner_all, ec);
        fs::remove_all(dir_, ec);
    }

    fs::path Path(const char* name) const {
        return dir_ / name;
    }

    fs::path dir_;
};

class DirSyncSyscalls : public PosixSyscalls {
public:
    int Fsync(int fd) override {
        fsync_fds.push_back(fd);
        if (fsync_errno != 0) {
            errno = fsync_errno;
            return -1;
        }
        return ::fsync(fd);
    }

    int Close(int fd) override {
        close_fds.push_back(fd);
        // Like Linux, release the descriptor even when reporting an error.
        const int result = ::close(fd);
        if (close_errno != 0) {
            errno = close_errno;
            return -1;
        }
        return result;
    }

    int fsync_errno = 0;
    int close_errno = 0;
    std::vector<int> fsync_fds;
    std::vector<int> close_fds;
};

// ---- Path inspection -------------------------------------------------------

TEST_F(PosixDirTest, InspectClassifiesPathKinds) {
    WriteFileDirectly(Path("file"), "x");
    fs::create_directory(Path("dir"));
    fs::create_symlink(Path("nowhere"), Path("dangling"));
    fs::create_symlink(Path("file"), Path("file_link"));
    fs::create_directory_symlink(Path("dir"), Path("dir_link"));
    ASSERT_EQ(::mkfifo(Path("fifo").c_str(), 0600), 0) << ErrnoText(errno);

    struct Case {
        const char* name;
        PathKind expected;
    };
    const Case cases[] = {
        {"missing", PathKind::kMissing},
        {"file", PathKind::kRegularFile},
        {"dir", PathKind::kDirectory},
        {"dangling", PathKind::kOther},
        {"file_link", PathKind::kOther},
        {"dir_link", PathKind::kOther},
        {"fifo", PathKind::kOther},
    };
    for (const Case& c : cases) {
        PathKind kind = PathKind::kMissing;
        if (c.expected == PathKind::kMissing) {
            kind = PathKind::kOther;  // make sure the result is written
        }
        ASSERT_TRUE(InspectPath(Path(c.name), kind).ok()) << c.name;
        EXPECT_EQ(kind, c.expected) << c.name;
    }
}

TEST_F(PosixDirTest, InspectNonDirectoryComponentIsErrorNotMissing) {
    WriteFileDirectly(Path("file"), "x");
    PathKind kind = PathKind::kDirectory;
    Status s = InspectPath(Path("file") / "child", kind);
    EXPECT_EQ(s.code, Code::IOError);
    EXPECT_TRUE(Contains(s.message, ErrnoText(ENOTDIR))) << s.message;
    EXPECT_EQ(kind, PathKind::kDirectory);  // unchanged on failure
}

TEST_F(PosixDirTest, InspectPermissionDeniedIsErrorNotMissing) {
    if (::geteuid() == 0) {
        GTEST_SKIP() << "root bypasses directory permissions";
    }
    fs::create_directory(Path("locked"));
    WriteFileDirectly(Path("locked") / "child", "x");
    fs::permissions(Path("locked"), fs::perms::none);

    PathKind kind = PathKind::kDirectory;
    Status s = InspectPath(Path("locked") / "child", kind);
    fs::permissions(Path("locked"), fs::perms::owner_all);

    EXPECT_EQ(s.code, Code::IOError);
    EXPECT_TRUE(Contains(s.message, ErrnoText(EACCES))) << s.message;
    EXPECT_EQ(kind, PathKind::kDirectory);
}

// ---- Listing ---------------------------------------------------------------

TEST_F(PosixDirTest, ListIncludesHiddenAndUnrelatedEntriesOnly) {
    WriteFileDirectly(Path("000001.wal"), "");
    WriteFileDirectly(Path("000001.wal.tmp"), "");
    WriteFileDirectly(Path("unrelated.tmp"), "");
    WriteFileDirectly(Path(".hidden"), "");
    WriteFileDirectly(Path("LOCK"), "");
    fs::create_directory(Path("sub"));
    WriteFileDirectly(Path("sub") / "nested", "");
    fs::create_symlink(Path("nowhere"), Path("dangling"));

    std::vector<std::string> names = {"stale"};
    ASSERT_TRUE(ListDirectory(dir_, names).ok());

    const std::vector<std::string> expected = {
        ".hidden", "000001.wal", "000001.wal.tmp", "LOCK",
        "dangling", "sub", "unrelated.tmp"};
    EXPECT_EQ(Sorted(names), Sorted(expected));
}

TEST_F(PosixDirTest, ListEmptyDirectoryReplacesOutput) {
    std::vector<std::string> names = {"stale"};
    ASSERT_TRUE(ListDirectory(dir_, names).ok());
    EXPECT_TRUE(names.empty());
}

TEST_F(PosixDirTest, ListFailureLeavesOutputUnchanged) {
    WriteFileDirectly(Path("file"), "x");
    const std::vector<std::string> original = {"keep"};

    std::vector<std::string> names = original;
    Status s = ListDirectory(Path("missing"), names);
    EXPECT_EQ(s.code, Code::IOError);
    EXPECT_TRUE(Contains(s.message, ErrnoText(ENOENT))) << s.message;
    EXPECT_EQ(names, original);

    s = ListDirectory(Path("file"), names);
    EXPECT_EQ(s.code, Code::IOError);
    EXPECT_TRUE(Contains(s.message, ErrnoText(ENOTDIR))) << s.message;
    EXPECT_EQ(names, original);
}

// ---- Directory creation ----------------------------------------------------

TEST_F(PosixDirTest, CreateNewThenExistingDirectory) {
    bool created = false;
    ASSERT_TRUE(CreateDirectory(Path("db"), created).ok());
    EXPECT_TRUE(created);
    EXPECT_TRUE(fs::is_directory(Path("db")));

    WriteFileDirectly(Path("db") / "keep", "x");
    created = true;
    ASSERT_TRUE(CreateDirectory(Path("db"), created).ok());
    EXPECT_FALSE(created);
    EXPECT_EQ(ReadFile(Path("db") / "keep"), "x");
}

TEST_F(PosixDirTest, CreateOverExistingNonDirectoryFails) {
    WriteFileDirectly(Path("file"), "contents");
    fs::create_symlink(Path("nowhere"), Path("dangling"));
    fs::create_directory(Path("real_dir"));
    fs::create_directory_symlink(Path("real_dir"), Path("dir_link"));

    for (const char* name : {"file", "dangling", "dir_link"}) {
        bool created = true;
        Status s = CreateDirectory(Path(name), created);
        EXPECT_EQ(s.code, Code::IOError) << name;
        EXPECT_TRUE(Contains(s.message, "not a directory")) << s.message;
        EXPECT_TRUE(created) << name;  // unchanged on failure
    }
    EXPECT_EQ(ReadFile(Path("file")), "contents");
    EXPECT_TRUE(fs::is_symlink(Path("dangling")));
}

TEST_F(PosixDirTest, CreateDoesNotCreateMissingParent) {
    bool created = true;
    Status s = CreateDirectory(Path("parent") / "child", created);
    EXPECT_EQ(s.code, Code::IOError);
    EXPECT_TRUE(Contains(s.message, ErrnoText(ENOENT))) << s.message;
    EXPECT_TRUE(created);
    EXPECT_FALSE(fs::exists(Path("parent")));
}

// ---- Rename ----------------------------------------------------------------

TEST_F(PosixDirTest, RenamePreservesBytes) {
    const std::string bytes("\x00\x01\x80\xff data", 9);
    WriteFileDirectly(Path("from"), bytes);
    ASSERT_TRUE(RenameFile(Path("from"), Path("to")).ok());
    EXPECT_FALSE(fs::exists(Path("from")));
    EXPECT_EQ(ReadFile(Path("to")), bytes);
}

TEST_F(PosixDirTest, RenameReplacesExistingDestination) {
    WriteFileDirectly(Path("from"), "new");
    WriteFileDirectly(Path("to"), "old contents");
    ASSERT_TRUE(RenameFile(Path("from"), Path("to")).ok());
    EXPECT_FALSE(fs::exists(Path("from")));
    EXPECT_EQ(ReadFile(Path("to")), "new");
}

TEST_F(PosixDirTest, RenameMissingSourceLeavesDestination) {
    WriteFileDirectly(Path("to"), "keep");
    Status s = RenameFile(Path("missing"), Path("to"));
    EXPECT_EQ(s.code, Code::IOError);
    EXPECT_TRUE(Contains(s.message, "missing")) << s.message;
    EXPECT_TRUE(Contains(s.message, ErrnoText(ENOENT))) << s.message;
    EXPECT_EQ(ReadFile(Path("to")), "keep");
}

// ---- Removal ---------------------------------------------------------------

TEST_F(PosixDirTest, RemoveOneFileLeavesNeighbors) {
    WriteFileDirectly(Path("a"), "A");
    WriteFileDirectly(Path("b.tmp"), "B");
    WriteFileDirectly(Path("c"), "C");
    ASSERT_TRUE(RemoveFile(Path("b.tmp")).ok());

    std::vector<std::string> names;
    ASSERT_TRUE(ListDirectory(dir_, names).ok());
    EXPECT_EQ(Sorted(names), (std::vector<std::string>{"a", "c"}));
    EXPECT_EQ(ReadFile(Path("a")), "A");
    EXPECT_EQ(ReadFile(Path("c")), "C");
}

TEST_F(PosixDirTest, RemoveNeverRemovesDirectories) {
    fs::create_directory(Path("empty"));
    fs::create_directory(Path("full"));
    WriteFileDirectly(Path("full") / "child", "x");

    for (const char* name : {"empty", "full"}) {
        Status s = RemoveFile(Path(name));
        EXPECT_EQ(s.code, Code::IOError) << name;
        EXPECT_TRUE(fs::is_directory(Path(name))) << name;
    }
    EXPECT_EQ(ReadFile(Path("full") / "child"), "x");
}

TEST_F(PosixDirTest, RemoveSymlinkKeepsTarget) {
    WriteFileDirectly(Path("target"), "x");
    fs::create_symlink(Path("target"), Path("link"));
    ASSERT_TRUE(RemoveFile(Path("link")).ok());
    EXPECT_FALSE(fs::is_symlink(Path("link")));
    EXPECT_EQ(ReadFile(Path("target")), "x");
}

TEST_F(PosixDirTest, RemoveMissingFileFails) {
    Status s = RemoveFile(Path("missing"));
    EXPECT_EQ(s.code, Code::IOError);
    EXPECT_TRUE(Contains(s.message, ErrnoText(ENOENT))) << s.message;
}

// ---- Directory sync --------------------------------------------------------

TEST_F(PosixDirTest, SyncDirectorySucceeds) {
    WriteFileDirectly(Path("file"), "x");
    EXPECT_TRUE(SyncDirectory(dir_).ok());

    DirSyncSyscalls sys;
    ASSERT_TRUE(SyncDirectory(dir_, sys).ok());
    ASSERT_EQ(sys.fsync_fds.size(), 1u);
    EXPECT_EQ(sys.close_fds, sys.fsync_fds);
    EXPECT_FALSE(FdIsOpen(sys.fsync_fds[0]));
}

TEST_F(PosixDirTest, SyncDirectoryRejectsNonDirectory) {
    WriteFileDirectly(Path("file"), "x");
    DirSyncSyscalls sys;
    Status s = SyncDirectory(Path("file"), sys);
    EXPECT_EQ(s.code, Code::IOError);
    EXPECT_TRUE(Contains(s.message, ErrnoText(ENOTDIR))) << s.message;
    EXPECT_TRUE(sys.fsync_fds.empty());

    s = SyncDirectory(Path("missing"), sys);
    EXPECT_EQ(s.code, Code::IOError);
    EXPECT_TRUE(sys.fsync_fds.empty());
}

TEST_F(PosixDirTest, SyncDirectoryFailureIsNotRetriedAndClosesFd) {
    for (int err : {EIO, ENOSPC, EINTR}) {
        DirSyncSyscalls sys;
        sys.fsync_errno = err;
        const std::size_t fds_before = OpenFdCount();

        Status s = SyncDirectory(dir_, sys);
        EXPECT_EQ(s.code, Code::IOError) << ErrnoText(err);
        EXPECT_TRUE(Contains(s.message, dir_.string())) << s.message;
        EXPECT_TRUE(Contains(s.message, "fsync directory")) << s.message;
        EXPECT_TRUE(Contains(s.message, ErrnoText(err))) << s.message;
        ASSERT_EQ(sys.fsync_fds.size(), 1u) << ErrnoText(err);
        EXPECT_EQ(sys.close_fds, sys.fsync_fds);
        EXPECT_FALSE(FdIsOpen(sys.fsync_fds[0]));
        EXPECT_EQ(OpenFdCount(), fds_before);
    }
}

TEST_F(PosixDirTest, SyncDirectoryKeepsPrimaryErrorWhenCloseAlsoFails) {
    DirSyncSyscalls sys;
    sys.fsync_errno = EIO;
    sys.close_errno = EBADF;

    Status s = SyncDirectory(dir_, sys);
    EXPECT_EQ(s.code, Code::IOError);
    EXPECT_TRUE(Contains(s.message, "fsync directory: " + ErrnoText(EIO)))
        << s.message;
    EXPECT_TRUE(Contains(s.message, "close also failed")) << s.message;
    EXPECT_EQ(sys.fsync_fds.size(), 1u);
    EXPECT_EQ(sys.close_fds.size(), 1u);
}

TEST_F(PosixDirTest, SyncDirectoryReportsCloseFailureAfterSuccessfulSync) {
    DirSyncSyscalls sys;
    sys.close_errno = EIO;

    Status s = SyncDirectory(dir_, sys);
    EXPECT_EQ(s.code, Code::IOError);
    EXPECT_TRUE(Contains(s.message, "close directory")) << s.message;
    EXPECT_TRUE(Contains(s.message, ErrnoText(EIO))) << s.message;
    EXPECT_EQ(sys.fsync_fds.size(), 1u);
    ASSERT_EQ(sys.close_fds.size(), 1u);
    EXPECT_FALSE(FdIsOpen(sys.close_fds[0]));
}

// ---- Lock ------------------------------------------------------------------

TEST_F(PosixDirTest, LockAcquireAndRelease) {
    const fs::path path = Path("LOCK");
    std::unique_ptr<FileLock> lock;
    ASSERT_TRUE(LockFile(path, lock).ok());
    ASSERT_NE(lock, nullptr);
    EXPECT_EQ(lock->path(), path.string());
    EXPECT_EQ(fs::status(path).type(), fs::file_type::regular);

    const std::vector<int> fds = FdsReferring(path);
    ASSERT_EQ(fds.size(), 1u);
    EXPECT_TRUE(::fcntl(fds[0], F_GETFD) & FD_CLOEXEC);

    lock.reset();
    EXPECT_TRUE(FdsReferring(path).empty());
    EXPECT_TRUE(fs::exists(path));  // never unlinked
}

TEST_F(PosixDirTest, SecondLockInSameProcessIsBusy) {
    const fs::path path = Path("LOCK");
    std::unique_ptr<FileLock> first;
    ASSERT_TRUE(LockFile(path, first).ok());

    std::unique_ptr<FileLock> second;
    Status s = LockFile(path, second);
    EXPECT_EQ(s.code, Code::Busy);
    EXPECT_TRUE(Contains(s.message, path.string())) << s.message;
    EXPECT_EQ(second, nullptr);
    // The failed attempt released its own descriptor only.
    EXPECT_EQ(FdsReferring(path).size(), 1u);

    // The first owner still holds the lock after the failed attempt.
    EXPECT_EQ(LockFile(path, second).code, Code::Busy);
    EXPECT_EQ(second, nullptr);

    first.reset();
    ASSERT_TRUE(LockFile(path, second).ok());
    EXPECT_NE(second, nullptr);
    EXPECT_EQ(LockFile(path, first).code, Code::Busy);
}

TEST_F(PosixDirTest, LockPreservesExistingContents) {
    const fs::path path = Path("LOCK");
    WriteFileDirectly(path, "existing contents");

    std::unique_ptr<FileLock> lock;
    ASSERT_TRUE(LockFile(path, lock).ok());
    EXPECT_EQ(ReadFile(path), "existing contents");
    lock.reset();

    EXPECT_TRUE(fs::exists(path));
    EXPECT_EQ(ReadFile(path), "existing contents");
}

TEST_F(PosixDirTest, LockFailuresDoNotLeakDescriptors) {
    const std::size_t fds_before = OpenFdCount();

    std::unique_ptr<FileLock> lock;
    Status s = LockFile(Path("missing") / "LOCK", lock);
    EXPECT_EQ(s.code, Code::IOError);
    EXPECT_TRUE(Contains(s.message, ErrnoText(ENOENT))) << s.message;
    EXPECT_EQ(lock, nullptr);
    EXPECT_FALSE(fs::exists(Path("missing")));

    fs::create_directory(Path("dir"));
    s = LockFile(Path("dir"), lock);
    EXPECT_EQ(s.code, Code::IOError);
    EXPECT_EQ(lock, nullptr);

    EXPECT_EQ(OpenFdCount(), fds_before);
}

TEST_F(PosixDirTest, LockRejectsNonEmptyOutput) {
    std::unique_ptr<FileLock> lock;
    ASSERT_TRUE(LockFile(Path("LOCK"), lock).ok());
    FileLock* original = lock.get();
    const std::size_t fds_before = OpenFdCount();

    Status s = LockFile(Path("OTHER"), lock);
    EXPECT_EQ(s.code, Code::InvalidArgument);
    EXPECT_EQ(lock.get(), original);
    EXPECT_FALSE(fs::exists(Path("OTHER")));
    EXPECT_EQ(OpenFdCount(), fds_before);

    // Even for the same path, the held lock is neither replaced nor dropped.
    s = LockFile(Path("LOCK"), lock);
    EXPECT_EQ(s.code, Code::InvalidArgument);
    EXPECT_EQ(lock.get(), original);
    std::unique_ptr<FileLock> other;
    EXPECT_EQ(LockFile(Path("LOCK"), other).code, Code::Busy);
}

// ---- Path validation -------------------------------------------------------

TEST_F(PosixDirTest, EmbeddedNulPathsAreRejected) {
    WriteFileDirectly(Path("abc"), "keep");
    const fs::path nul = WithNul(dir_, "abc");

    PathKind kind = PathKind::kDirectory;
    EXPECT_EQ(InspectPath(nul, kind).code, Code::InvalidArgument);
    EXPECT_EQ(kind, PathKind::kDirectory);

    EXPECT_EQ(RemoveFile(nul).code, Code::InvalidArgument);
    EXPECT_EQ(RenameFile(nul, Path("moved")).code, Code::InvalidArgument);
    EXPECT_EQ(RenameFile(Path("moved_src_missing"), nul).code,
              Code::InvalidArgument);
    WriteFileDirectly(Path("src"), "src");
    EXPECT_EQ(RenameFile(Path("src"), nul).code, Code::InvalidArgument);
    EXPECT_EQ(ReadFile(Path("abc")), "keep");
    EXPECT_EQ(ReadFile(Path("src")), "src");
    EXPECT_FALSE(fs::exists(Path("moved")));

    bool created = true;
    EXPECT_EQ(CreateDirectory(WithNul(dir_, "newdir"), created).code,
              Code::InvalidArgument);
    EXPECT_TRUE(created);
    EXPECT_FALSE(fs::exists(Path("newdir")));

    std::vector<std::string> names = {"keep"};
    EXPECT_EQ(ListDirectory(dir_.string() + std::string(1, '\0') + "x", names)
                  .code,
              Code::InvalidArgument);
    EXPECT_EQ(names, std::vector<std::string>{"keep"});

    DirSyncSyscalls sys;
    EXPECT_EQ(SyncDirectory(dir_.string() + std::string(1, '\0') + "x", sys)
                  .code,
              Code::InvalidArgument);
    EXPECT_TRUE(sys.fsync_fds.empty());

    std::unique_ptr<FileLock> lock;
    EXPECT_EQ(LockFile(WithNul(dir_, "LOCK"), lock).code,
              Code::InvalidArgument);
    EXPECT_EQ(lock, nullptr);
    EXPECT_FALSE(fs::exists(Path("LOCK")));
}

TEST_F(PosixDirTest, EmptyPathsAreRejected) {
    PathKind kind = PathKind::kDirectory;
    EXPECT_EQ(InspectPath("", kind).code, Code::InvalidArgument);
    std::vector<std::string> names;
    EXPECT_EQ(ListDirectory("", names).code, Code::InvalidArgument);
    bool created = false;
    EXPECT_EQ(CreateDirectory("", created).code, Code::InvalidArgument);
    EXPECT_EQ(RemoveFile("").code, Code::InvalidArgument);
    EXPECT_EQ(SyncDirectory("").code, Code::InvalidArgument);
    std::unique_ptr<FileLock> lock;
    EXPECT_EQ(LockFile("", lock).code, Code::InvalidArgument);
}

}  // namespace
}  // namespace kv
