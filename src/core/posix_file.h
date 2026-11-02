#pragma once

#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "status.h"
#include "writable_file.h"

namespace kv {

// Syscall seam for the POSIX file operations in this header. Each method
// has the contract of the matching POSIX call: on failure return -1 and
// set errno. The base class calls the real syscalls. Tests may subclass it
// to script results; the production control flow runs unchanged on top.
class PosixSyscalls {
public:
    PosixSyscalls() = default;
    virtual ~PosixSyscalls() = default;

    PosixSyscalls(const PosixSyscalls&) = delete;
    PosixSyscalls& operator=(const PosixSyscalls&) = delete;

    virtual ssize_t Write(int fd, const void* buf, std::size_t count);
    virtual ssize_t Read(int fd, void* buf, std::size_t count);
    virtual int Ftruncate(int fd, off_t length);
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

// Reads the entire existing regular file at path. Never creates or
// modifies it. Reads until EOF, accepting short reads and retrying EINTR;
// any other read error, a non-regular file, or a failed close returns
// IOError. out is replaced only on success and unchanged on failure. The
// descriptor is closed on every path, including exceptions (allocation
// failure propagates as an exception).
//
// M1 limitation: the whole file is held in memory, so peak memory grows
// with file size. Recovery is not bounded-memory for arbitrarily large
// WAL files.
Status ReadFileToString(const std::filesystem::path& path,
                        std::string& out,
                        PosixSyscalls& syscalls = DefaultPosixSyscalls());

// Truncates the existing regular file at path to exactly length bytes,
// then fsyncs it once. Never creates the file. Returns InvalidArgument,
// without modifying the file, if length does not fit in off_t or exceeds
// the current size (ftruncate would otherwise zero-extend it). ftruncate
// is retried on EINTR because repeating it is harmless; a failed fsync is
// never retried. The descriptor is closed exactly once on every path; if
// cleanup also fails, the first error is reported.
//
// An error is not a rollback: after an fsync or close failure the file may
// already be truncated, and its durable size is unknown.
//
// The caller must hold the database lock so no other writer changes the
// file between reading it, choosing length, and truncating. Later appends
// must open the file through OpenAppendableFile, whose O_APPEND writes
// start at the new end of file rather than at a stale offset that would
// leave a zero-filled gap.
Status TruncateAndSyncFile(const std::filesystem::path& path,
                           std::uint64_t length,
                           PosixSyscalls& syscalls = DefaultPosixSyscalls());

}  // namespace kv
