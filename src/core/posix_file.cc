#include "posix_file.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "posix_dir.h"

namespace kv {

namespace {

// Linux transfers at most 0x7ffff000 bytes per write(2) call, and this also
// keeps each request within SSIZE_MAX on 32-bit systems.
constexpr std::size_t kMaxWriteSize = 0x7ffff000;

constexpr std::size_t kReadChunkSize = 64 * 1024;

std::string ErrnoText(int err) {
    return std::generic_category().message(err);
}

class PosixWritableFile final : public WritableFile {
public:
    PosixWritableFile(std::string path, PosixSyscalls& syscalls)
        : path_(std::move(path)), syscalls_(&syscalls) {}

    ~PosixWritableFile() override {
        if (fd_ >= 0) {
            // Errors are unreportable here and the fd is released either way.
            syscalls_->Close(fd_);
        }
    }

    // Opens path with flags; called once, before the file is published.
    Status Open(int flags, const char* operation) {
        const int fd = syscalls_->Open(path_.c_str(), flags, 0644);
        if (fd < 0) {
            const int err = errno;
            return Status::IOError(path_ + ": " + operation + ": " +
                                   ErrnoText(err));
        }
        fd_ = fd;
        return Status::Ok();
    }

    Status Append(std::string_view data) override {
        if (fd_ < 0) {
            return Status::IOError(path_ + ": append: file is closed");
        }

        const char* p = data.data();
        std::size_t remaining = data.size();

        while (remaining > 0) {
            const std::size_t request = std::min(remaining, kMaxWriteSize);
            const ssize_t n = syscalls_->Write(fd_, p, request);

            if (n < 0) {
                const int err = errno;
                if (err == EINTR) {
                    continue;
                }
                return AppendError(data.size(), remaining, ErrnoText(err));
            }
            if (n == 0) {
                // Retrying would loop forever without progress.
                return AppendError(data.size(), remaining,
                                   "write made no progress");
            }
            if (static_cast<std::size_t>(n) > request) {
                return AppendError(data.size(), remaining,
                                   "write returned " + std::to_string(n) +
                                       " for a request of " +
                                       std::to_string(request) + " bytes");
            }

            p += n;
            remaining -= static_cast<std::size_t>(n);
        }

        return Status::Ok();
    }

    Status Sync() override {
        if (fd_ < 0) {
            return Status::IOError(path_ + ": fsync: file is closed");
        }

        // Never retried, even on EINTR: after a failed fsync the kernel may
        // already have marked failed dirty pages clean, so a later success
        // would not prove the data reached storage (design.md section 5).
        if (syscalls_->Fsync(fd_) != 0) {
            const int err = errno;
            return Status::IOError(path_ + ": fsync: " + ErrnoText(err));
        }
        return Status::Ok();
    }

    Status Close() override {
        if (fd_ < 0) {
            return Status::Ok();
        }

        // Linux releases the descriptor even when close() fails, and the
        // number may be reused at once, so mark it closed first and never
        // retry.
        const int fd = fd_;
        fd_ = -1;
        if (syscalls_->Close(fd) != 0) {
            const int err = errno;
            return Status::IOError(path_ + ": close: " + ErrnoText(err));
        }
        return Status::Ok();
    }

private:
    Status AppendError(std::size_t total,
                       std::size_t remaining,
                       const std::string& reason) const {
        return Status::IOError(path_ + ": append failed after " +
                               std::to_string(total - remaining) + " of " +
                               std::to_string(total) + " bytes: " + reason);
    }

    std::string path_;
    PosixSyscalls* syscalls_;
    int fd_ = -1;
};

Status OpenPosixFile(const std::filesystem::path& path,
                     int flags,
                     const char* operation,
                     std::unique_ptr<WritableFile>& out,
                     PosixSyscalls& syscalls) {
    if (out) {
        return Status::InvalidArgument(
            path.string() + ": " + operation + ": output file is not empty");
    }

    // open(2) would stop at an embedded NUL and act on the prefix path.
    Status s = CheckSyscallPath(path, operation);
    if (!s.ok()) {
        return s;
    }

    // Allocate before opening so an allocation failure cannot leak the
    // descriptor or leave a newly created file behind.
    auto file = std::make_unique<PosixWritableFile>(path.string(), syscalls);
    s = file->Open(flags, operation);
    if (!s.ok()) {
        return s;
    }

    out = std::move(file);
    return Status::Ok();
}

// Owns a descriptor for one operation. The destructor closes it on early
// returns and exceptions; Close() closes it explicitly so the result can
// be reported. Either way close runs at most once and is never retried.
class ScopedFd {
public:
    ScopedFd(int fd, PosixSyscalls& syscalls) : fd_(fd), syscalls_(syscalls) {}

    ~ScopedFd() {
        if (fd_ >= 0) {
            syscalls_.Close(fd_);
        }
    }

    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;

    int get() const {
        return fd_;
    }

    // Returns 0, or the errno of a failed close.
    int Close() {
        const int fd = fd_;
        fd_ = -1;
        return syscalls_.Close(fd) == 0 ? 0 : errno;
    }

private:
    int fd_;
    PosixSyscalls& syscalls_;
};

// Closes fd and returns IOError(message), noting a close failure without
// replacing the primary error.
Status FailAndClose(ScopedFd& fd, std::string message) {
    const int close_err = fd.Close();
    if (close_err != 0) {
        message += " (close also failed: " + ErrnoText(close_err) + ")";
    }
    return Status::IOError(std::move(message));
}

}  // namespace

int PosixSyscalls::Open(const char* path, int flags, mode_t mode) {
    return ::open(path, flags, mode);
}

ssize_t PosixSyscalls::Write(int fd, const void* buf, std::size_t count) {
    return ::write(fd, buf, count);
}

ssize_t PosixSyscalls::Read(int fd, void* buf, std::size_t count) {
    return ::read(fd, buf, count);
}

ssize_t PosixSyscalls::Pread(int fd,
                             void* buf,
                             std::size_t count,
                             off_t offset) {
    return ::pread(fd, buf, count, offset);
}

int PosixSyscalls::Fstat(int fd, struct stat* st) {
    return ::fstat(fd, st);
}

int PosixSyscalls::Ftruncate(int fd, off_t length) {
    return ::ftruncate(fd, length);
}

int PosixSyscalls::Fsync(int fd) {
    return ::fsync(fd);
}

int PosixSyscalls::Rename(const char* from, const char* to) {
    return ::rename(from, to);
}

int PosixSyscalls::Unlink(const char* path) {
    return ::unlink(path);
}

int PosixSyscalls::Close(int fd) {
    return ::close(fd);
}

int PosixSyscalls::Lstat(const char* path, struct stat* st) {
    return ::lstat(path, st);
}

int PosixSyscalls::Mkdir(const char* path, mode_t mode) {
    return ::mkdir(path, mode);
}

void PosixSyscalls::Checkpoint(std::string_view) {}

PosixSyscalls& DefaultPosixSyscalls() {
    static PosixSyscalls syscalls;
    return syscalls;
}

Status NewWritableFile(const std::filesystem::path& path,
                       std::unique_ptr<WritableFile>& out,
                       PosixSyscalls& syscalls) {
    return OpenPosixFile(path,
                         O_WRONLY | O_CREAT | O_EXCL | O_APPEND | O_CLOEXEC,
                         "create", out, syscalls);
}

Status OpenAppendableFile(const std::filesystem::path& path,
                          std::unique_ptr<WritableFile>& out,
                          PosixSyscalls& syscalls) {
    return OpenPosixFile(path, O_WRONLY | O_APPEND | O_CLOEXEC,
                         "open for append", out, syscalls);
}

Status ReadFileToString(const std::filesystem::path& path,
                        std::string& out,
                        PosixSyscalls& syscalls) {
    Status s = CheckSyscallPath(path, "read");
    if (!s.ok()) {
        return s;
    }
    const std::string name = path.string();

    const int raw_fd = syscalls.Open(path.c_str(), O_RDONLY | O_CLOEXEC, 0);
    if (raw_fd < 0) {
        const int err = errno;
        return Status::IOError(name + ": open for read: " + ErrnoText(err));
    }
    ScopedFd fd(raw_fd, syscalls);

    struct stat st;
    if (syscalls.Fstat(fd.get(), &st) != 0) {
        const int err = errno;
        return FailAndClose(fd, name + ": fstat: " + ErrnoText(err));
    }
    if (!S_ISREG(st.st_mode)) {
        return FailAndClose(fd, name + ": read: not a regular file");
    }

    std::string result;
    // The size is only a capacity hint; reading continues until EOF.
    if (static_cast<std::uint64_t>(st.st_size) <= result.max_size()) {
        result.reserve(static_cast<std::size_t>(st.st_size));
    }

    std::string buffer(kReadChunkSize, '\0');
    for (;;) {
        const ssize_t n = syscalls.Read(fd.get(), buffer.data(), buffer.size());
        if (n < 0) {
            const int err = errno;
            if (err == EINTR) {
                continue;
            }
            return FailAndClose(fd, name + ": read failed after " +
                                        std::to_string(result.size()) +
                                        " bytes: " + ErrnoText(err));
        }
        if (n == 0) {
            break;
        }
        if (static_cast<std::size_t>(n) > buffer.size()) {
            return FailAndClose(fd, name + ": read returned " +
                                        std::to_string(n) +
                                        " for a request of " +
                                        std::to_string(buffer.size()) +
                                        " bytes");
        }
        result.append(buffer.data(), static_cast<std::size_t>(n));
    }

    const int close_err = fd.Close();
    if (close_err != 0) {
        return Status::IOError(name + ": close after read: " +
                               ErrnoText(close_err));
    }

    out = std::move(result);
    return Status::Ok();
}

Status TruncateAndSyncFile(const std::filesystem::path& path,
                           std::uint64_t length,
                           PosixSyscalls& syscalls) {
    Status s = CheckSyscallPath(path, "truncate");
    if (!s.ok()) {
        return s;
    }
    const std::string name = path.string();
    const std::string length_text = std::to_string(length);

    if (length >
        static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        return Status::InvalidArgument(name + ": truncate: length " +
                                       length_text +
                                       " is not representable as off_t");
    }
    const off_t target = static_cast<off_t>(length);

    const int raw_fd = syscalls.Open(path.c_str(), O_WRONLY | O_CLOEXEC, 0);
    if (raw_fd < 0) {
        const int err = errno;
        return Status::IOError(name + ": open for truncate: " +
                               ErrnoText(err));
    }
    ScopedFd fd(raw_fd, syscalls);

    struct stat st;
    if (syscalls.Fstat(fd.get(), &st) != 0) {
        const int err = errno;
        return FailAndClose(fd, name + ": fstat: " + ErrnoText(err));
    }
    if (!S_ISREG(st.st_mode)) {
        return FailAndClose(fd, name + ": truncate: not a regular file");
    }
    if (target > st.st_size) {
        return Status::InvalidArgument(
            name + ": truncate: length " + length_text +
            " exceeds current size " + std::to_string(st.st_size) +
            "; refusing to extend the file");
    }

    while (syscalls.Ftruncate(fd.get(), target) != 0) {
        const int err = errno;
        if (err == EINTR) {
            continue;
        }
        return FailAndClose(fd, name + ": ftruncate to " + length_text +
                                    ": " + ErrnoText(err));
    }

    // Never retried: see PosixWritableFile::Sync.
    if (syscalls.Fsync(fd.get()) != 0) {
        const int err = errno;
        return FailAndClose(fd, name + ": fsync after truncate to " +
                                    length_text + ": " + ErrnoText(err) +
                                    "; truncation may already have taken "
                                    "effect");
    }

    const int close_err = fd.Close();
    if (close_err != 0) {
        return Status::IOError(name + ": close after truncate: " +
                               ErrnoText(close_err));
    }
    return Status::Ok();
}

RandomAccessFile::~RandomAccessFile() {
    if (fd_ >= 0) {
        // Errors are unreportable here and the fd is released either way.
        syscalls_->Close(fd_);
    }
}

Status RandomAccessFile::Open(const std::filesystem::path& path,
                              PosixSyscalls& syscalls,
                              std::unique_ptr<RandomAccessFile>& out) {
    if (out) {
        return Status::InvalidArgument(path.string() +
                                       ": open for read: output is not empty");
    }
    Status s = CheckSyscallPath(path, "open for read");
    if (!s.ok()) {
        return s;
    }

    // Allocate before opening so an allocation failure cannot leak the fd.
    std::unique_ptr<RandomAccessFile> file(
        new RandomAccessFile(path.string(), syscalls));
    const std::string& name = file->path_;

    file->fd_ = syscalls.Open(
        name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0);
    if (file->fd_ < 0) {
        const int err = errno;
        return Status::IOError(name + ": open for read: " + ErrnoText(err));
    }

    struct stat st;
    if (syscalls.Fstat(file->fd_, &st) != 0) {
        const int err = errno;
        return Status::IOError(name + ": fstat: " + ErrnoText(err));
    }
    if (!S_ISREG(st.st_mode)) {
        return Status::IOError(name + ": read: not a regular file");
    }
    file->size_ = static_cast<std::uint64_t>(st.st_size);

    out = std::move(file);
    return Status::Ok();
}

Status RandomAccessFile::Read(std::uint64_t offset,
                              std::size_t n,
                              char* dst) const {
    if (fd_ < 0) {
        return Status::IOError(path_ + ": read: file is closed");
    }
    const auto max_off =
        static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
    if (offset > max_off || n > max_off - offset) {
        return Status::IOError(path_ + ": read of " + std::to_string(n) +
                               " bytes at offset " + std::to_string(offset) +
                               " is not representable as off_t");
    }

    std::size_t done = 0;
    while (done < n) {
        const std::size_t request = std::min(n - done, kMaxWriteSize);
        const ssize_t got = syscalls_->Pread(
            fd_, dst + done, request, static_cast<off_t>(offset + done));
        if (got < 0) {
            const int err = errno;
            if (err == EINTR) {
                continue;
            }
            return Status::IOError(path_ + ": pread at offset " +
                                   std::to_string(offset + done) + ": " +
                                   ErrnoText(err));
        }
        if (got == 0) {
            return Status::IOError(
                path_ + ": unexpected end of file at offset " +
                std::to_string(offset + done) + " while reading " +
                std::to_string(n) + " bytes at offset " +
                std::to_string(offset));
        }
        if (static_cast<std::size_t>(got) > request) {
            return Status::IOError(path_ + ": pread returned " +
                                   std::to_string(got) + " for a request of " +
                                   std::to_string(request) + " bytes");
        }
        done += static_cast<std::size_t>(got);
    }
    return Status::Ok();
}

Status RandomAccessFile::Close() {
    if (fd_ < 0) {
        return Status::Ok();
    }
    // As in PosixWritableFile::Close: mark closed first, never retry.
    const int fd = fd_;
    fd_ = -1;
    if (syscalls_->Close(fd) != 0) {
        const int err = errno;
        return Status::IOError(path_ + ": close: " + ErrnoText(err));
    }
    return Status::Ok();
}

}  // namespace kv
