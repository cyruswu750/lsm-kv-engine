#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <utility>

#include "posix_file.h"
#include "status.h"

namespace kv {

// Holds an exclusive flock on a lock file for the lifetime of the object.
// Destruction releases the lock and closes the descriptor without
// throwing. The lock file is never unlinked, so its pathname stays stable
// for every process that contends on it.
class FileLock {
public:
    ~FileLock();

    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;

    const std::string& path() const {
        return path_;
    }

private:
    friend Status LockFile(const std::filesystem::path& path,
                           std::unique_ptr<FileLock>& out,
                           PosixSyscalls& syscalls);

    FileLock(std::string path, PosixSyscalls& syscalls)
        : path_(std::move(path)), syscalls_(&syscalls) {}

    std::string path_;
    PosixSyscalls* syscalls_;
    int fd_ = -1;
};

// Opens or creates path (never truncating it; O_CLOEXEC) and acquires
// flock(LOCK_EX | LOCK_NB). Returns Busy if another open file description
// holds the lock, including one in this process; IOError for other
// failures. out must be empty (otherwise InvalidArgument, out unchanged);
// it is set only on success, and on failure no descriptor is left open.
//
// flock locks belong to the open file description: a child created by
// fork shares it, but O_CLOEXEC keeps it out of exec'd programs.
//
// The open, flock, unlock, and close go through syscalls, which must
// outlive out.
Status LockFile(const std::filesystem::path& path,
                std::unique_ptr<FileLock>& out,
                PosixSyscalls& syscalls = DefaultPosixSyscalls());

}  // namespace kv
