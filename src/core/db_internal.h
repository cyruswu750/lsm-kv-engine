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
    // Same contract as DB::Open. Every open, read, pread, write, fstat,
    // ftruncate, fsync, close, rename, and unlink on WAL, SST, and manifest
    // files (and their temporary files), and every directory fsync, goes
    // through syscalls. Listing, lstat inspection, mkdir, and the LOCK
    // file's open and flock always use the real system calls
    // (docs/sst-format.md section 12).
    //
    // syscalls is not owned. It must outlive the returned DB, because the
    // DB keeps using it for WAL appends, flushes, and SST reads until it
    // is destroyed.
    static Status Open(const std::filesystem::path& directory,
                       const Options& options,
                       PosixSyscalls& syscalls,
                       std::unique_ptr<DB>& out);
};

}  // namespace kv
