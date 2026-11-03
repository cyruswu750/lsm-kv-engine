#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>

#include "db.h"
#include "manifest.h"
#include "options.h"
#include "posix_file.h"
#include "sst.h"
#include "status.h"

namespace kv {

// Engine parameters that are deliberately not public Options
// (docs/m3-design.md section 2). Tests and the M4 benchmarks set them
// through DBOpener.
struct InternalOptions {
    // Logical run count (docs/sst-format.md section 4.1) at which a
    // mutation first compacts every live SST into one base run. 0 disables
    // compaction; 1, or a value above max_manifest_ssts, is rejected as
    // InvalidArgument.
    std::size_t compaction_trigger = 4;
    // Writer limits for flush and compaction output. Values above the SST
    // format limits are clamped to them (SstLimits).
    SstLimits sst_limits;
    // The manifest's SST-entry limit. Tests lower it; values above
    // kMaxManifestSsts are clamped.
    std::uint32_t max_manifest_ssts = kMaxManifestSsts;
};

// Internal construction path for DB-level fault injection (design.md
// section 10). It is not part of the public API and is not reachable
// through Options; DB::Open calls it with DefaultPosixSyscalls().
class DBOpener {
public:
    // Same contract as DB::Open, with default InternalOptions. Every
    // filesystem operation of the database goes through syscalls: file
    // open, read, pread, write, fstat, ftruncate, fsync, close, rename, and
    // unlink; lstat inspection, directory listing, mkdir, directory fsync;
    // and the LOCK file's open, flock, and close. The engine also reports
    // named non-syscall boundaries through syscalls.Checkpoint.
    //
    // syscalls is not owned. It must outlive the returned DB, because the
    // DB keeps using it for WAL appends, flushes, compactions, SST reads,
    // and releasing the lock until it is destroyed.
    static Status Open(const std::filesystem::path& directory,
                       const Options& options,
                       PosixSyscalls& syscalls,
                       std::unique_ptr<DB>& out);

    static Status Open(const std::filesystem::path& directory,
                       const Options& options,
                       const InternalOptions& internal,
                       PosixSyscalls& syscalls,
                       std::unique_ptr<DB>& out);
};

}  // namespace kv
