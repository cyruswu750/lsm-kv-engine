#pragma once

#include <filesystem>
#include <memory>

#include "db.h"
#include "options.h"
#include "posix_file.h"
#include "status.h"

namespace kv {

// Internal construction path for DB-level fault injection (design.md
// section 10). It is not part of the public API and is not reachable
// through Options; DB::Open calls it with DefaultPosixSyscalls().
class DBOpener {
public:
    // Same contract as DB::Open. Every filesystem operation of the
    // database goes through syscalls: file open, read, pread, write,
    // fstat, ftruncate, fsync, close, rename, and unlink; lstat
    // inspection, directory listing, mkdir, directory fsync; and the LOCK
    // file's open, flock, and close. The engine also reports named
    // non-syscall boundaries through syscalls.Checkpoint.
    //
    // syscalls is not owned. It must outlive the returned DB, because the
    // DB keeps using it for WAL appends, flushes, SST reads, and releasing
    // the lock until it is destroyed.
    static Status Open(const std::filesystem::path& directory,
                       const Options& options,
                       PosixSyscalls& syscalls,
                       std::unique_ptr<DB>& out);
};

}  // namespace kv
