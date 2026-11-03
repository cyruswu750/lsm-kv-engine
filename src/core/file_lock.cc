#include "file_lock.h"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <cerrno>
#include <system_error>
#include <utility>

#include "posix_dir.h"

namespace kv {

FileLock::~FileLock() {
    if (fd_ >= 0) {
        // Explicit unlock also releases the lock for any forked duplicate of
        // this descriptor. Errors are unreportable here; close releases the
        // descriptor regardless.
        syscalls_->Flock(fd_, LOCK_UN);
        syscalls_->Close(fd_);
    }
}

int PosixSyscalls::Flock(int fd, int operation) {
    return ::flock(fd, operation);
}

Status LockFile(const std::filesystem::path& path,
                std::unique_ptr<FileLock>& out,
                PosixSyscalls& syscalls) {
    if (out) {
        return Status::InvalidArgument(path.string() +
                                       ": lock: output lock is not empty");
    }

    Status s = CheckSyscallPath(path, "lock");
    if (!s.ok()) {
        return s;
    }

    // Allocate before opening so allocation failure cannot leak the fd.
    // From here on, the handle's destructor closes the fd on every failure.
    std::unique_ptr<FileLock> lock(new FileLock(path.string(), syscalls));

    lock->fd_ =
        syscalls.Open(path.c_str(), O_RDONLY | O_CREAT | O_CLOEXEC, 0644);
    if (lock->fd_ < 0) {
        const int err = errno;
        return Status::IOError(path.string() + ": open lock file: " +
                               std::generic_category().message(err));
    }

    while (syscalls.Flock(lock->fd_, LOCK_EX | LOCK_NB) != 0) {
        const int err = errno;
        if (err == EINTR) {
            continue;
        }
        if (err == EWOULDBLOCK || err == EAGAIN) {
            return Status::Busy(path.string() +
                                ": lock is held by another handle or process");
        }
        return Status::IOError(path.string() + ": flock: " +
                               std::generic_category().message(err));
    }

    out = std::move(lock);
    return Status::Ok();
}

}  // namespace kv
