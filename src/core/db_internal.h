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
    // Same contract as DB::Open. Every read, write, ftruncate, fsync, and
    // close on the WAL and its temporary file, and every directory fsync,
    // goes through syscalls. Opening files, listing, inspection, mkdir,
    // rename, unlink, and flock always use the real system calls.
    //
    // syscalls is not owned. It must outlive the returned DB, because the
    // DB's WAL file keeps using it until the DB is destroyed.
    static Status Open(const std::filesystem::path& directory,
                       const Options& options,
                       PosixSyscalls& syscalls,
                       std::unique_ptr<DB>& out);
};

}  // namespace kv
