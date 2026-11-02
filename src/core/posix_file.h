#pragma once

#include <sys/types.h>

#include <cstddef>
#include <filesystem>
#include <memory>

#include "status.h"
#include "writable_file.h"

namespace kv {

// Syscall seam for PosixWritableFile. Each method has the contract of the
// matching POSIX call: on failure return -1 and set errno. The base class
// calls the real syscalls. Tests may subclass it to script results; the
// production Append/Sync/Close logic runs unchanged on top.
class PosixSyscalls {
public:
    PosixSyscalls() = default;
    virtual ~PosixSyscalls() = default;

    PosixSyscalls(const PosixSyscalls&) = delete;
    PosixSyscalls& operator=(const PosixSyscalls&) = delete;

    virtual ssize_t Write(int fd, const void* buf, std::size_t count);
    virtual int Fsync(int fd);

    // As on Linux, the descriptor counts as released even when this fails.
    virtual int Close(int fd);
};

// Stateless real-syscall instance used when no seam is supplied.
PosixSyscalls& DefaultPosixSyscalls();

// Both factories require out to be empty; otherwise they return
// InvalidArgument without opening anything. On any failure out stays
// empty. On success out owns the descriptor (opened with O_APPEND and
// O_CLOEXEC) and keeps a reference to syscalls, which must outlive it.
//
// Opening does not durably publish a new directory entry; that requires
// a directory sync by the caller.

// Creates a new file. Fails with IOError if path already exists (including
// a symlink); an existing file is never truncated or modified.
Status NewWritableFile(const std::filesystem::path& path,
                       std::unique_ptr<WritableFile>& out,
                       PosixSyscalls& syscalls = DefaultPosixSyscalls());

// Opens an existing file for appending, preserving its contents. Fails
// with IOError if path does not exist; never creates or truncates.
Status OpenAppendableFile(const std::filesystem::path& path,
                          std::unique_ptr<WritableFile>& out,
                          PosixSyscalls& syscalls = DefaultPosixSyscalls());

}  // namespace kv
