#include "posix_file.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace kv {

namespace {

// Linux transfers at most 0x7ffff000 bytes per write(2) call, and this also
// keeps each request within SSIZE_MAX on 32-bit systems.
constexpr std::size_t kMaxWriteSize = 0x7ffff000;

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
        const int fd = ::open(path_.c_str(), flags, 0644);
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

    // Allocate before opening so an allocation failure cannot leak the
    // descriptor or leave a newly created file behind.
    auto file = std::make_unique<PosixWritableFile>(path.string(), syscalls);
    Status s = file->Open(flags, operation);
    if (!s.ok()) {
        return s;
    }

    out = std::move(file);
    return Status::Ok();
}

}  // namespace

ssize_t PosixSyscalls::Write(int fd, const void* buf, std::size_t count) {
    return ::write(fd, buf, count);
}

int PosixSyscalls::Fsync(int fd) {
    return ::fsync(fd);
}

int PosixSyscalls::Close(int fd) {
    return ::close(fd);
}

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

}  // namespace kv
