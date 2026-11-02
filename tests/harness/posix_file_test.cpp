#include <gtest/gtest.h>

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "core/posix_file.h"

namespace kv {
namespace {

namespace fs = std::filesystem;

// These tests use real files plus scripted syscall results. They verify
// the user-space write/sync/close logic, not behavior under power loss.

std::string Bytes(const char* data, std::size_t n) {
    return std::string(data, n);
}

const std::string kBinary = Bytes("\x00\x01\x7f\x80\xfe\xff\x00tail", 11);

bool Contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

std::string ErrnoText(int err) {
    return std::generic_category().message(err);
}

// Reads a file through iostreams, independently of the code under test.
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

// Creates a private directory and removes only that directory on teardown.
class PosixFileTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::string pattern =
            (fs::temp_directory_path() / "kv_posix_file_test.XXXXXX").string();
        ASSERT_NE(::mkdtemp(pattern.data()), nullptr) << ErrnoText(errno);
        dir_ = pattern;
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    fs::path Path(const char* name) const {
        return dir_ / name;
    }

    fs::path dir_;
};

// Scripted syscall seam. Each Write consumes one scripted step; once the
// script is empty, writes go straight to the real file. Short steps also
// perform a real write of fewer bytes, so the file shows exactly which
// bytes the production Append loop handed to the kernel. Read works the
// same way with read_script, and Ftruncate consumes ftruncate_script
// errnos before calling the real ftruncate.
class ScriptedSyscalls : public PosixSyscalls {
public:
    struct Step {
        enum Kind { kShort, kZero, kError, kOverlong, kThrow } kind;
        std::size_t bytes = 0;  // kShort: bytes to really write or read
        int err = 0;            // kError: errno to report
    };

    static Step Short(std::size_t n) { return {Step::kShort, n, 0}; }
    static Step Zero() { return {Step::kZero, 0, 0}; }
    static Step Error(int err) { return {Step::kError, 0, err}; }
    static Step Overlong() { return {Step::kOverlong, 0, 0}; }
    static Step Throw() { return {Step::kThrow, 0, 0}; }

    ssize_t Write(int fd, const void* buf, std::size_t count) override {
        write_requests.emplace_back(static_cast<const char*>(buf), count);
        if (script.empty()) {
            return ::write(fd, buf, count);
        }
        const Step step = script.front();
        script.pop_front();
        switch (step.kind) {
            case Step::kShort:
                return ::write(fd, buf, std::min(step.bytes, count));
            case Step::kZero:
                return 0;
            case Step::kError:
                errno = step.err;
                return -1;
            case Step::kOverlong:
                return static_cast<ssize_t>(count + 1);
            case Step::kThrow:
                throw std::runtime_error("scripted write exception");
        }
        return -1;
    }

    ssize_t Read(int fd, void* buf, std::size_t count) override {
        ++read_calls;
        std::size_t request = count;
        if (!read_script.empty()) {
            const Step step = read_script.front();
            read_script.pop_front();
            switch (step.kind) {
                case Step::kShort:
                    request = std::min(step.bytes, count);
                    break;
                case Step::kZero:
                    return 0;
                case Step::kError:
                    errno = step.err;
                    return -1;
                case Step::kOverlong:
                    return static_cast<ssize_t>(count + 1);
                case Step::kThrow:
                    throw std::runtime_error("scripted read exception");
            }
        }
        const ssize_t n = ::read(fd, buf, request);
        if (n > 0) {
            read_chunks.emplace_back(static_cast<const char*>(buf),
                                     static_cast<std::size_t>(n));
        }
        return n;
    }

    int Ftruncate(int fd, off_t length) override {
        ftruncate_lengths.push_back(length);
        if (!ftruncate_script.empty()) {
            const int err = ftruncate_script.front();
            ftruncate_script.pop_front();
            errno = err;
            return -1;
        }
        return ::ftruncate(fd, length);
    }

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

    std::deque<Step> script;
    std::deque<Step> read_script;
    std::deque<int> ftruncate_script;  // errnos returned before real calls
    int fsync_errno = 0;
    int close_errno = 0;

    std::vector<std::string> write_requests;  // bytes passed to each call
    std::size_t read_calls = 0;
    std::vector<std::string> read_chunks;     // bytes each real read returned
    std::vector<off_t> ftruncate_lengths;
    std::vector<int> fsync_fds;
    std::vector<int> close_fds;
};

// ---- Real temporary files --------------------------------------------------

TEST_F(PosixFileTest, CreateAppendSyncCloseWritesExactBytes) {
    const fs::path path = Path("data");
    std::unique_ptr<WritableFile> file;
    ASSERT_TRUE(NewWritableFile(path, file).ok());
    ASSERT_NE(file, nullptr);

    ASSERT_TRUE(file->Append(kBinary).ok());
    ASSERT_TRUE(file->Append("second").ok());
    ASSERT_TRUE(file->Sync().ok());
    ASSERT_TRUE(file->Close().ok());

    EXPECT_EQ(ReadFile(path), kBinary + "second");
}

TEST_F(PosixFileTest, OpenAppendablePreservesPrefix) {
    const fs::path path = Path("data");
    WriteFileDirectly(path, kBinary);

    std::unique_ptr<WritableFile> file;
    ASSERT_TRUE(OpenAppendableFile(path, file).ok());
    ASSERT_TRUE(file->Append("more").ok());
    ASSERT_TRUE(file->Sync().ok());
    ASSERT_TRUE(file->Close().ok());
    file.reset();

    // A second reopen still appends rather than overwriting.
    ASSERT_TRUE(OpenAppendableFile(path, file).ok());
    ASSERT_TRUE(file->Append("!").ok());
    ASSERT_TRUE(file->Close().ok());

    EXPECT_EQ(ReadFile(path), kBinary + "more!");
}

TEST_F(PosixFileTest, CreateNewDoesNotOverwriteExistingFile) {
    const fs::path path = Path("data");
    WriteFileDirectly(path, "keep me");

    std::unique_ptr<WritableFile> file;
    Status s = NewWritableFile(path, file);
    EXPECT_EQ(s.code, Code::IOError);
    EXPECT_TRUE(Contains(s.message, path.string())) << s.message;
    EXPECT_TRUE(Contains(s.message, "create")) << s.message;
    EXPECT_TRUE(Contains(s.message, ErrnoText(EEXIST))) << s.message;
    EXPECT_EQ(file, nullptr);
    EXPECT_EQ(ReadFile(path), "keep me");
}

TEST_F(PosixFileTest, CreateNewDoesNotFollowSymlink) {
    const fs::path target = Path("target");
    const fs::path link = Path("link");
    WriteFileDirectly(target, "keep me");
    fs::create_symlink(target, link);

    std::unique_ptr<WritableFile> file;
    EXPECT_EQ(NewWritableFile(link, file).code, Code::IOError);
    EXPECT_EQ(file, nullptr);
    EXPECT_EQ(ReadFile(target), "keep me");
}

TEST_F(PosixFileTest, OpenAppendableDoesNotCreateMissingFile) {
    const fs::path path = Path("missing");
    std::unique_ptr<WritableFile> file;
    Status s = OpenAppendableFile(path, file);
    EXPECT_EQ(s.code, Code::IOError);
    EXPECT_TRUE(Contains(s.message, path.string())) << s.message;
    EXPECT_TRUE(Contains(s.message, ErrnoText(ENOENT))) << s.message;
    EXPECT_EQ(file, nullptr);
    EXPECT_FALSE(fs::exists(path));
}

TEST_F(PosixFileTest, NonEmptyOutputIsRejectedWithoutOpening) {
    const fs::path first = Path("first");
    std::unique_ptr<WritableFile> file;
    ASSERT_TRUE(NewWritableFile(first, file).ok());
    WritableFile* original = file.get();

    const fs::path second = Path("second");
    EXPECT_EQ(NewWritableFile(second, file).code, Code::InvalidArgument);
    EXPECT_FALSE(fs::exists(second));
    EXPECT_EQ(file.get(), original);

    WriteFileDirectly(second, "x");
    EXPECT_EQ(OpenAppendableFile(second, file).code, Code::InvalidArgument);
    EXPECT_EQ(file.get(), original);
}

// Returns dir / (prefix + NUL + suffix). The name is built with an explicit
// length so the NUL and suffix survive, and the result is checked so the
// test cannot silently degrade to the truncated prefix path.
fs::path PathWithEmbeddedNul(const fs::path& dir,
                             const std::string& prefix,
                             const std::string& suffix) {
    const std::string name = prefix + std::string(1, '\0') + suffix;
    const fs::path path = dir / name;

    const std::string& native = path.native();
    const std::size_t nul_pos = dir.native().size() + 1 + prefix.size();
    EXPECT_EQ(name.size(), prefix.size() + 1 + suffix.size());
    EXPECT_EQ(native.size(), nul_pos + 1 + suffix.size());
    EXPECT_EQ(native.find('\0'), nul_pos);
    EXPECT_EQ(native.substr(nul_pos + 1), suffix);
    return path;
}

TEST_F(PosixFileTest, CreateNewRejectsEmbeddedNulPath) {
    const fs::path prefix = Path("abc");
    ASSERT_FALSE(fs::exists(prefix));
    const fs::path path = PathWithEmbeddedNul(dir_, "abc", "suffix");
    ASSERT_FALSE(HasFailure()) << "NUL path was not constructed as intended";

    std::unique_ptr<WritableFile> file;
    Status s = NewWritableFile(path, file);
    EXPECT_EQ(s.code, Code::InvalidArgument) << s.ToString();
    EXPECT_EQ(file, nullptr);
    EXPECT_FALSE(fs::exists(prefix));

    // If a handle was returned anyway, release it without writing.
    file.reset();
}

TEST_F(PosixFileTest, OpenAppendableRejectsEmbeddedNulPath) {
    const fs::path prefix = Path("abc");
    WriteFileDirectly(prefix, kBinary);
    const fs::path path = PathWithEmbeddedNul(dir_, "abc", "suffix");
    ASSERT_FALSE(HasFailure()) << "NUL path was not constructed as intended";

    std::unique_ptr<WritableFile> file;
    Status s = OpenAppendableFile(path, file);
    EXPECT_EQ(s.code, Code::InvalidArgument) << s.ToString();
    EXPECT_EQ(file, nullptr);

    // If a handle was returned anyway, release it without writing.
    file.reset();
    EXPECT_EQ(ReadFile(prefix), kBinary);
}

TEST_F(PosixFileTest, EmptyAppendSucceedsWithoutWriting) {
    ScriptedSyscalls sys;
    const fs::path path = Path("data");
    std::unique_ptr<WritableFile> file;
    ASSERT_TRUE(NewWritableFile(path, file, sys).ok());

    EXPECT_TRUE(file->Append("").ok());
    EXPECT_TRUE(file->Append(std::string_view()).ok());
    EXPECT_TRUE(sys.write_requests.empty());
    ASSERT_TRUE(file->Close().ok());
    EXPECT_EQ(fs::file_size(path), 0u);
}

TEST_F(PosixFileTest, DescriptorUsesAppendAndCloseOnExec) {
    ScriptedSyscalls sys;
    std::unique_ptr<WritableFile> file;
    ASSERT_TRUE(NewWritableFile(Path("data"), file, sys).ok());
    ASSERT_TRUE(file->Sync().ok());
    ASSERT_EQ(sys.fsync_fds.size(), 1u);
    const int fd = sys.fsync_fds[0];

    EXPECT_TRUE(::fcntl(fd, F_GETFD) & FD_CLOEXEC);
    EXPECT_TRUE(::fcntl(fd, F_GETFL) & O_APPEND);
}

TEST_F(PosixFileTest, RepeatedCloseAndUseAfterClose) {
    ScriptedSyscalls sys;
    const fs::path path = Path("data");
    std::unique_ptr<WritableFile> file;
    ASSERT_TRUE(NewWritableFile(path, file, sys).ok());
    ASSERT_TRUE(file->Append("abc").ok());

    EXPECT_TRUE(file->Close().ok());
    EXPECT_TRUE(file->Close().ok());
    EXPECT_EQ(sys.close_fds.size(), 1u);

    const std::size_t writes = sys.write_requests.size();
    Status s = file->Append("def");
    EXPECT_EQ(s.code, Code::IOError);
    EXPECT_TRUE(Contains(s.message, "closed")) << s.message;
    s = file->Sync();
    EXPECT_EQ(s.code, Code::IOError);
    EXPECT_TRUE(Contains(s.message, "closed")) << s.message;
    EXPECT_EQ(sys.write_requests.size(), writes);
    EXPECT_TRUE(sys.fsync_fds.empty());

    file.reset();
    EXPECT_EQ(sys.close_fds.size(), 1u);  // destructor does not close again
    EXPECT_EQ(ReadFile(path), "abc");
}

TEST_F(PosixFileTest, DestructorClosesWithoutSync) {
    ScriptedSyscalls sys;
    const fs::path path = Path("data");
    std::unique_ptr<WritableFile> file;
    ASSERT_TRUE(NewWritableFile(path, file, sys).ok());
    ASSERT_TRUE(file->Append("abc").ok());
    ASSERT_TRUE(file->Sync().ok());
    const int fd = sys.fsync_fds.at(0);
    ASSERT_TRUE(FdIsOpen(fd));
    ASSERT_TRUE(file->Append("def").ok());

    file.reset();
    ASSERT_EQ(sys.close_fds.size(), 1u);
    EXPECT_EQ(sys.close_fds[0], fd);
    EXPECT_EQ(sys.fsync_fds.size(), 1u);  // no implicit sync on destruction
    EXPECT_FALSE(FdIsOpen(fd));
    EXPECT_EQ(ReadFile(path), "abcdef");
}

// ---- Deterministic write faults --------------------------------------------

TEST_F(PosixFileTest, MultipleShortWritesWriteExactBytes) {
    ScriptedSyscalls sys;
    sys.script = {ScriptedSyscalls::Short(3), ScriptedSyscalls::Short(1),
                  ScriptedSyscalls::Short(5)};
    const fs::path path = Path("data");
    const std::string data = kBinary + "0123456789";
    std::unique_ptr<WritableFile> file;
    ASSERT_TRUE(NewWritableFile(path, file, sys).ok());

    ASSERT_TRUE(file->Append(data).ok());
    ASSERT_TRUE(file->Close().ok());

    const std::vector<std::string> expected = {
        data, data.substr(3), data.substr(4), data.substr(9)};
    EXPECT_EQ(sys.write_requests, expected);
    EXPECT_EQ(ReadFile(path), data);
}

TEST_F(PosixFileTest, EintrIsRetried) {
    ScriptedSyscalls sys;
    sys.script = {ScriptedSyscalls::Error(EINTR),
                  ScriptedSyscalls::Error(EINTR)};
    const fs::path path = Path("data");
    std::unique_ptr<WritableFile> file;
    ASSERT_TRUE(NewWritableFile(path, file, sys).ok());

    ASSERT_TRUE(file->Append(kBinary).ok());
    ASSERT_TRUE(file->Close().ok());

    const std::vector<std::string> expected = {kBinary, kBinary, kBinary};
    EXPECT_EQ(sys.write_requests, expected);
    EXPECT_EQ(ReadFile(path), kBinary);
}

TEST_F(PosixFileTest, ShortWriteThenEintrThenCompletion) {
    ScriptedSyscalls sys;
    sys.script = {ScriptedSyscalls::Short(4), ScriptedSyscalls::Error(EINTR)};
    const fs::path path = Path("data");
    std::unique_ptr<WritableFile> file;
    ASSERT_TRUE(NewWritableFile(path, file, sys).ok());

    ASSERT_TRUE(file->Append(kBinary).ok());
    ASSERT_TRUE(file->Close().ok());

    const std::vector<std::string> expected = {
        kBinary, kBinary.substr(4), kBinary.substr(4)};
    EXPECT_EQ(sys.write_requests, expected);
    EXPECT_EQ(ReadFile(path), kBinary);
}

TEST_F(PosixFileTest, ZeroProgressReturnsIOError) {
    ScriptedSyscalls sys;
    sys.script = {ScriptedSyscalls::Short(2), ScriptedSyscalls::Zero()};
    const fs::path path = Path("data");
    std::unique_ptr<WritableFile> file;
    ASSERT_TRUE(NewWritableFile(path, file, sys).ok());

    Status s = file->Append(kBinary);
    EXPECT_EQ(s.code, Code::IOError);
    EXPECT_TRUE(Contains(s.message, path.string())) << s.message;
    EXPECT_TRUE(Contains(s.message, "after 2 of 11 bytes")) << s.message;
    EXPECT_TRUE(Contains(s.message, "no progress")) << s.message;
    EXPECT_EQ(sys.write_requests.size(), 2u);
    ASSERT_TRUE(file->Close().ok());
    EXPECT_EQ(ReadFile(path), kBinary.substr(0, 2));
}

TEST_F(PosixFileTest, ErrorBeforeProgressReturnsIOError) {
    for (int err : {ENOSPC, EIO}) {
        ScriptedSyscalls sys;
        sys.script = {ScriptedSyscalls::Error(err)};
        const fs::path path = Path(err == ENOSPC ? "enospc" : "eio");
        std::unique_ptr<WritableFile> file;
        ASSERT_TRUE(NewWritableFile(path, file, sys).ok());

        Status s = file->Append(kBinary);
        EXPECT_EQ(s.code, Code::IOError);
        EXPECT_TRUE(Contains(s.message, path.string())) << s.message;
        EXPECT_TRUE(Contains(s.message, "append")) << s.message;
        EXPECT_TRUE(Contains(s.message, "after 0 of 11 bytes")) << s.message;
        EXPECT_TRUE(Contains(s.message, ErrnoText(err))) << s.message;
        EXPECT_EQ(sys.write_requests.size(), 1u);
        ASSERT_TRUE(file->Close().ok());
        EXPECT_EQ(fs::file_size(path), 0u);
    }
}

TEST_F(PosixFileTest, ErrorAfterPartialProgressKeepsPrefix) {
    ScriptedSyscalls sys;
    sys.script = {ScriptedSyscalls::Short(5), ScriptedSyscalls::Error(EIO)};
    const fs::path path = Path("data");
    std::unique_ptr<WritableFile> file;
    ASSERT_TRUE(NewWritableFile(path, file, sys).ok());

    Status s = file->Append(kBinary);
    EXPECT_EQ(s.code, Code::IOError);
    EXPECT_TRUE(Contains(s.message, "after 5 of 11 bytes")) << s.message;
    EXPECT_TRUE(Contains(s.message, ErrnoText(EIO))) << s.message;
    EXPECT_EQ(sys.write_requests.size(), 2u);
    ASSERT_TRUE(file->Close().ok());
    // No rollback: the bytes already written stay in the file.
    EXPECT_EQ(ReadFile(path), kBinary.substr(0, 5));
}

TEST_F(PosixFileTest, OverlongWriteResultReturnsIOError) {
    ScriptedSyscalls sys;
    sys.script = {ScriptedSyscalls::Overlong()};
    const fs::path path = Path("data");
    std::unique_ptr<WritableFile> file;
    ASSERT_TRUE(NewWritableFile(path, file, sys).ok());

    Status s = file->Append(kBinary);
    EXPECT_EQ(s.code, Code::IOError);
    EXPECT_TRUE(Contains(s.message, "returned 12")) << s.message;
    EXPECT_EQ(sys.write_requests.size(), 1u);
}

// ---- Deterministic sync and close faults -----------------------------------

TEST_F(PosixFileTest, SyncCallsFsyncOnce) {
    ScriptedSyscalls sys;
    std::unique_ptr<WritableFile> file;
    ASSERT_TRUE(NewWritableFile(Path("data"), file, sys).ok());
    ASSERT_TRUE(file->Append("abc").ok());
    EXPECT_TRUE(file->Sync().ok());
    EXPECT_EQ(sys.fsync_fds.size(), 1u);
}

TEST_F(PosixFileTest, SyncFailureIsNotRetried) {
    for (int err : {EIO, ENOSPC, EINTR}) {
        ScriptedSyscalls sys;
        sys.fsync_errno = err;
        const fs::path path = Path("data");
        std::unique_ptr<WritableFile> file;
        ASSERT_TRUE(NewWritableFile(path, file, sys).ok());
        ASSERT_TRUE(file->Append("abc").ok());

        Status s = file->Sync();
        EXPECT_EQ(s.code, Code::IOError) << ErrnoText(err);
        EXPECT_TRUE(Contains(s.message, path.string())) << s.message;
        EXPECT_TRUE(Contains(s.message, "fsync")) << s.message;
        EXPECT_TRUE(Contains(s.message, ErrnoText(err))) << s.message;
        EXPECT_EQ(sys.fsync_fds.size(), 1u) << ErrnoText(err);

        ASSERT_TRUE(file->Close().ok());
        file.reset();
        fs::remove(path);
    }
}

TEST_F(PosixFileTest, CloseFailureIsReportedOnceAndNotRetried) {
    ScriptedSyscalls sys;
    sys.close_errno = EIO;
    const fs::path path = Path("data");
    std::unique_ptr<WritableFile> file;
    ASSERT_TRUE(NewWritableFile(path, file, sys).ok());
    ASSERT_TRUE(file->Append("abc").ok());

    Status s = file->Close();
    EXPECT_EQ(s.code, Code::IOError);
    EXPECT_TRUE(Contains(s.message, path.string())) << s.message;
    EXPECT_TRUE(Contains(s.message, "close")) << s.message;
    EXPECT_TRUE(Contains(s.message, ErrnoText(EIO))) << s.message;
    ASSERT_EQ(sys.close_fds.size(), 1u);
    EXPECT_FALSE(FdIsOpen(sys.close_fds[0]));

    EXPECT_TRUE(file->Close().ok());
    EXPECT_EQ(file->Append("x").code, Code::IOError);
    file.reset();
    EXPECT_EQ(sys.close_fds.size(), 1u);
}

TEST_F(PosixFileTest, DestructorIgnoresCloseFailure) {
    ScriptedSyscalls sys;
    sys.close_errno = EIO;
    std::unique_ptr<WritableFile> file;
    ASSERT_TRUE(NewWritableFile(Path("data"), file, sys).ok());
    file.reset();  // must not throw or abort
    ASSERT_EQ(sys.close_fds.size(), 1u);
    EXPECT_FALSE(FdIsOpen(sys.close_fds[0]));
}

// ---- Whole-file reads ------------------------------------------------------

// Larger than one 64 KiB read chunk; covers every byte value.
std::string LargeBinary() {
    std::string s;
    for (std::size_t i = 0; i < 200 * 1024 + 17; ++i) {
        s.push_back(static_cast<char>((i * 131 + i / 251) & 0xff));
    }
    return s;
}

TEST_F(PosixFileTest, ReadEmptyFileReplacesOutput) {
    const fs::path path = Path("empty");
    WriteFileDirectly(path, "");
    std::string out = "stale";
    ASSERT_TRUE(ReadFileToString(path, out).ok());
    EXPECT_TRUE(out.empty());
}

TEST_F(PosixFileTest, ReadReturnsExactBinaryContents) {
    const fs::path path = Path("data");
    WriteFileDirectly(path, kBinary);
    std::string out = "stale";
    ASSERT_TRUE(ReadFileToString(path, out).ok());
    EXPECT_EQ(out, kBinary);

    const std::string large = LargeBinary();
    WriteFileDirectly(path, large);
    ScriptedSyscalls sys;
    ASSERT_TRUE(ReadFileToString(path, out, sys).ok());
    EXPECT_EQ(out, large);
    EXPECT_GE(sys.read_chunks.size(), 4u);  // several chunks, then EOF
    ASSERT_EQ(sys.close_fds.size(), 1u);
    EXPECT_FALSE(FdIsOpen(sys.close_fds[0]));
    EXPECT_EQ(ReadFile(path), large);  // not modified by reading
}

TEST_F(PosixFileTest, ReadMissingFileFailsWithoutCreating) {
    const fs::path path = Path("missing");
    std::string out = "keep";
    Status s = ReadFileToString(path, out);
    EXPECT_EQ(s.code, Code::IOError);
    EXPECT_TRUE(Contains(s.message, path.string())) << s.message;
    EXPECT_TRUE(Contains(s.message, ErrnoText(ENOENT))) << s.message;
    EXPECT_EQ(out, "keep");
    EXPECT_FALSE(fs::exists(path));
}

TEST_F(PosixFileTest, ReadRejectsInvalidPaths) {
    WriteFileDirectly(Path("abc"), kBinary);
    const fs::path nul = PathWithEmbeddedNul(dir_, "abc", "suffix");
    ASSERT_FALSE(HasFailure()) << "NUL path was not constructed as intended";

    ScriptedSyscalls sys;
    std::string out = "keep";
    EXPECT_EQ(ReadFileToString(nul, out, sys).code, Code::InvalidArgument);
    EXPECT_EQ(ReadFileToString("", out, sys).code, Code::InvalidArgument);
    EXPECT_EQ(out, "keep");
    EXPECT_EQ(sys.read_calls, 0u);
    EXPECT_TRUE(sys.close_fds.empty());
}

TEST_F(PosixFileTest, ReadRejectsDirectory) {
    fs::create_directory(Path("dir"));
    ScriptedSyscalls sys;
    std::string out = "keep";
    Status s = ReadFileToString(Path("dir"), out, sys);
    EXPECT_EQ(s.code, Code::IOError);
    EXPECT_TRUE(Contains(s.message, "not a regular file")) << s.message;
    EXPECT_EQ(out, "keep");
    EXPECT_EQ(sys.read_calls, 0u);
    ASSERT_EQ(sys.close_fds.size(), 1u);
    EXPECT_FALSE(FdIsOpen(sys.close_fds[0]));
}

TEST_F(PosixFileTest, ReadHandlesShortReads) {
    const fs::path path = Path("data");
    WriteFileDirectly(path, kBinary);
    ScriptedSyscalls sys;
    sys.read_script = {ScriptedSyscalls::Short(3), ScriptedSyscalls::Short(1),
                       ScriptedSyscalls::Short(5)};

    std::string out;
    ASSERT_TRUE(ReadFileToString(path, out, sys).ok());
    EXPECT_EQ(out, kBinary);
    const std::vector<std::string> expected = {
        kBinary.substr(0, 3), kBinary.substr(3, 1), kBinary.substr(4, 5),
        kBinary.substr(9)};
    EXPECT_EQ(sys.read_chunks, expected);
}

TEST_F(PosixFileTest, ReadRetriesEintrAndKeepsProgress) {
    const fs::path path = Path("data");
    WriteFileDirectly(path, kBinary);
    ScriptedSyscalls sys;
    sys.read_script = {ScriptedSyscalls::Error(EINTR),
                       ScriptedSyscalls::Short(2),
                       ScriptedSyscalls::Error(EINTR)};

    std::string out;
    ASSERT_TRUE(ReadFileToString(path, out, sys).ok());
    EXPECT_EQ(out, kBinary);
    const std::vector<std::string> expected = {kBinary.substr(0, 2),
                                               kBinary.substr(2)};
    EXPECT_EQ(sys.read_chunks, expected);
    EXPECT_EQ(sys.read_calls, 5u);  // EINTR, 2, EINTR, rest, EOF
}

TEST_F(PosixFileTest, ReadErrorAfterPartialReadLeavesOutputUnchanged) {
    for (int err : {EIO, EISDIR}) {
        const fs::path path = Path("data");
        WriteFileDirectly(path, kBinary);
        ScriptedSyscalls sys;
        sys.read_script = {ScriptedSyscalls::Short(5),
                           ScriptedSyscalls::Error(err)};

        std::string out = "keep";
        Status s = ReadFileToString(path, out, sys);
        EXPECT_EQ(s.code, Code::IOError);
        EXPECT_TRUE(Contains(s.message, path.string())) << s.message;
        EXPECT_TRUE(Contains(s.message, "after 5 bytes")) << s.message;
        EXPECT_TRUE(Contains(s.message, ErrnoText(err))) << s.message;
        EXPECT_EQ(out, "keep");
        ASSERT_EQ(sys.close_fds.size(), 1u);
        EXPECT_FALSE(FdIsOpen(sys.close_fds[0]));
    }
}

TEST_F(PosixFileTest, ReadOverlongResultIsError) {
    const fs::path path = Path("data");
    WriteFileDirectly(path, kBinary);
    ScriptedSyscalls sys;
    sys.read_script = {ScriptedSyscalls::Overlong()};
    std::string out = "keep";
    EXPECT_EQ(ReadFileToString(path, out, sys).code, Code::IOError);
    EXPECT_EQ(out, "keep");
    EXPECT_EQ(sys.close_fds.size(), 1u);
}

TEST_F(PosixFileTest, ReadExceptionReleasesDescriptor) {
    const fs::path path = Path("data");
    WriteFileDirectly(path, kBinary);
    ScriptedSyscalls sys;
    sys.read_script = {ScriptedSyscalls::Short(4), ScriptedSyscalls::Throw()};

    std::string out = "keep";
    EXPECT_THROW(ReadFileToString(path, out, sys), std::runtime_error);
    EXPECT_EQ(out, "keep");
    ASSERT_EQ(sys.close_fds.size(), 1u);
    EXPECT_FALSE(FdIsOpen(sys.close_fds[0]));
}

TEST_F(PosixFileTest, ReadCloseFailureLeavesOutputUnchanged) {
    const fs::path path = Path("data");
    WriteFileDirectly(path, kBinary);
    ScriptedSyscalls sys;
    sys.close_errno = EIO;

    std::string out = "keep";
    Status s = ReadFileToString(path, out, sys);
    EXPECT_EQ(s.code, Code::IOError);
    EXPECT_TRUE(Contains(s.message, "close after read")) << s.message;
    EXPECT_EQ(out, "keep");
    ASSERT_EQ(sys.close_fds.size(), 1u);
    EXPECT_FALSE(FdIsOpen(sys.close_fds[0]));
}

// ---- Truncate and sync -----------------------------------------------------

TEST_F(PosixFileTest, TruncatePreservesExactPrefix) {
    const fs::path path = Path("data");
    WriteFileDirectly(path, kBinary + "torn tail");
    ScriptedSyscalls sys;

    ASSERT_TRUE(TruncateAndSyncFile(path, kBinary.size(), sys).ok());
    EXPECT_EQ(ReadFile(path), kBinary);
    EXPECT_EQ(sys.ftruncate_lengths,
              std::vector<off_t>{static_cast<off_t>(kBinary.size())});
    EXPECT_EQ(sys.fsync_fds.size(), 1u);
    ASSERT_EQ(sys.close_fds.size(), 1u);
    EXPECT_FALSE(FdIsOpen(sys.close_fds[0]));
}

TEST_F(PosixFileTest, TruncateToCurrentLengthAndToZero) {
    const fs::path path = Path("data");
    WriteFileDirectly(path, kBinary);

    ASSERT_TRUE(TruncateAndSyncFile(path, kBinary.size()).ok());
    EXPECT_EQ(ReadFile(path), kBinary);

    ASSERT_TRUE(TruncateAndSyncFile(path, 0).ok());
    EXPECT_EQ(fs::file_size(path), 0u);
}

TEST_F(PosixFileTest, TruncateRefusesToExtend) {
    const fs::path path = Path("data");
    WriteFileDirectly(path, kBinary);
    ScriptedSyscalls sys;

    Status s = TruncateAndSyncFile(path, kBinary.size() + 1, sys);
    EXPECT_EQ(s.code, Code::InvalidArgument);
    EXPECT_TRUE(Contains(s.message, "refusing to extend")) << s.message;
    EXPECT_TRUE(sys.ftruncate_lengths.empty());
    EXPECT_TRUE(sys.fsync_fds.empty());
    ASSERT_EQ(sys.close_fds.size(), 1u);
    EXPECT_FALSE(FdIsOpen(sys.close_fds[0]));
    EXPECT_EQ(ReadFile(path), kBinary);
}

TEST_F(PosixFileTest, TruncateRejectsLengthNotRepresentableAsOffT) {
    const fs::path path = Path("data");
    WriteFileDirectly(path, kBinary);
    ScriptedSyscalls sys;

    Status s = TruncateAndSyncFile(
        path, std::numeric_limits<std::uint64_t>::max(), sys);
    EXPECT_EQ(s.code, Code::InvalidArgument);
    EXPECT_TRUE(Contains(s.message, "off_t")) << s.message;
    EXPECT_TRUE(sys.ftruncate_lengths.empty());
    EXPECT_TRUE(sys.close_fds.empty());  // rejected before opening
    EXPECT_EQ(ReadFile(path), kBinary);
}

TEST_F(PosixFileTest, TruncateMissingFileIsNotCreated) {
    const fs::path path = Path("missing");
    Status s = TruncateAndSyncFile(path, 0);
    EXPECT_EQ(s.code, Code::IOError);
    EXPECT_TRUE(Contains(s.message, ErrnoText(ENOENT))) << s.message;
    EXPECT_FALSE(fs::exists(path));
}

TEST_F(PosixFileTest, TruncateRejectsInvalidPathsAndDirectories) {
    WriteFileDirectly(Path("abc"), kBinary);
    const fs::path nul = PathWithEmbeddedNul(dir_, "abc", "suffix");
    ASSERT_FALSE(HasFailure()) << "NUL path was not constructed as intended";

    ScriptedSyscalls sys;
    EXPECT_EQ(TruncateAndSyncFile(nul, 0, sys).code, Code::InvalidArgument);
    EXPECT_EQ(TruncateAndSyncFile("", 0, sys).code, Code::InvalidArgument);
    EXPECT_EQ(ReadFile(Path("abc")), kBinary);

    fs::create_directory(Path("dir"));
    EXPECT_EQ(TruncateAndSyncFile(Path("dir"), 0, sys).code, Code::IOError);
    EXPECT_TRUE(sys.ftruncate_lengths.empty());
    EXPECT_TRUE(sys.fsync_fds.empty());
}

TEST_F(PosixFileTest, TruncateFailureIsReportedAndClosesDescriptor) {
    const fs::path path = Path("data");
    WriteFileDirectly(path, kBinary);
    ScriptedSyscalls sys;
    sys.ftruncate_script = {EIO};

    Status s = TruncateAndSyncFile(path, 3, sys);
    EXPECT_EQ(s.code, Code::IOError);
    EXPECT_TRUE(Contains(s.message, "ftruncate to 3")) << s.message;
    EXPECT_TRUE(Contains(s.message, ErrnoText(EIO))) << s.message;
    EXPECT_EQ(sys.ftruncate_lengths.size(), 1u);
    EXPECT_TRUE(sys.fsync_fds.empty());
    ASSERT_EQ(sys.close_fds.size(), 1u);
    EXPECT_FALSE(FdIsOpen(sys.close_fds[0]));
    EXPECT_EQ(ReadFile(path), kBinary);  // the scripted failure did nothing
}

TEST_F(PosixFileTest, TruncateRetriesEintr) {
    const fs::path path = Path("data");
    WriteFileDirectly(path, kBinary);
    ScriptedSyscalls sys;
    sys.ftruncate_script = {EINTR, EINTR};

    ASSERT_TRUE(TruncateAndSyncFile(path, 3, sys).ok());
    EXPECT_EQ(sys.ftruncate_lengths, (std::vector<off_t>{3, 3, 3}));
    EXPECT_EQ(sys.fsync_fds.size(), 1u);
    EXPECT_EQ(ReadFile(path), kBinary.substr(0, 3));
}

TEST_F(PosixFileTest, FsyncFailureAfterTruncateIsNotRetriedOrRolledBack) {
    for (int err : {EIO, EINTR}) {
        const fs::path path = Path("data");
        WriteFileDirectly(path, kBinary);
        ScriptedSyscalls sys;
        sys.fsync_errno = err;

        Status s = TruncateAndSyncFile(path, 4, sys);
        EXPECT_EQ(s.code, Code::IOError) << ErrnoText(err);
        EXPECT_TRUE(Contains(s.message, "fsync after truncate to 4"))
            << s.message;
        EXPECT_TRUE(Contains(s.message, ErrnoText(err))) << s.message;
        EXPECT_EQ(sys.fsync_fds.size(), 1u) << ErrnoText(err);
        ASSERT_EQ(sys.close_fds.size(), 1u);
        EXPECT_FALSE(FdIsOpen(sys.close_fds[0]));
        // The truncation itself already happened.
        EXPECT_EQ(ReadFile(path), kBinary.substr(0, 4));
    }
}

TEST_F(PosixFileTest, TruncateKeepsPrimaryErrorWhenCloseAlsoFails) {
    const fs::path path = Path("data");
    WriteFileDirectly(path, kBinary);
    ScriptedSyscalls sys;
    sys.fsync_errno = EIO;
    sys.close_errno = EBADF;

    Status s = TruncateAndSyncFile(path, 4, sys);
    EXPECT_EQ(s.code, Code::IOError);
    EXPECT_TRUE(Contains(s.message, "fsync after truncate to 4: " +
                                        ErrnoText(EIO)))
        << s.message;
    EXPECT_TRUE(Contains(s.message, "close also failed: " + ErrnoText(EBADF)))
        << s.message;
    EXPECT_EQ(sys.close_fds.size(), 1u);
}

TEST_F(PosixFileTest, TruncateReportsCloseFailureAfterSync) {
    const fs::path path = Path("data");
    WriteFileDirectly(path, kBinary);
    ScriptedSyscalls sys;
    sys.close_errno = EIO;

    Status s = TruncateAndSyncFile(path, 4, sys);
    EXPECT_EQ(s.code, Code::IOError);
    EXPECT_TRUE(Contains(s.message, "close after truncate")) << s.message;
    EXPECT_EQ(sys.fsync_fds.size(), 1u);
    ASSERT_EQ(sys.close_fds.size(), 1u);
    EXPECT_FALSE(FdIsOpen(sys.close_fds[0]));
    EXPECT_EQ(ReadFile(path), kBinary.substr(0, 4));
}

TEST_F(PosixFileTest, TruncateThenAppendHasNoGap) {
    const fs::path path = Path("wal");
    const std::string prefix = kBinary;
    const std::string torn = "partial record bytes";
    const std::string appended = std::string("new\x00record", 10);

    std::unique_ptr<WritableFile> file;
    ASSERT_TRUE(NewWritableFile(path, file).ok());
    ASSERT_TRUE(file->Append(prefix + torn).ok());
    ASSERT_TRUE(file->Sync().ok());
    ASSERT_TRUE(file->Close().ok());
    file.reset();

    ASSERT_TRUE(TruncateAndSyncFile(path, prefix.size()).ok());

    ASSERT_TRUE(OpenAppendableFile(path, file).ok());
    ASSERT_TRUE(file->Append(appended).ok());
    ASSERT_TRUE(file->Sync().ok());
    ASSERT_TRUE(file->Close().ok());
    file.reset();

    const std::string expected = prefix + appended;
    std::string out;
    ASSERT_TRUE(ReadFileToString(path, out).ok());
    EXPECT_EQ(out, expected);
    EXPECT_EQ(ReadFile(path), expected);  // independent iostream read
    EXPECT_EQ(fs::file_size(path), expected.size());
}

}  // namespace
}  // namespace kv
