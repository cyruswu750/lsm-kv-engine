#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>

#include "bloom.h"
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
    // Bloom filter bits per key for new SSTs (version 2); 0 writes
    // filterless version 1 SSTs. Values above kMaxFilterBitsPerKey are
    // clamped.
    std::uint32_t bloom_bits_per_key = kDefaultBloomBitsPerKey;
    // Benchmark-only: Get ignores filters (they are still written and
    // validated). Lookups stay exact either way.
    bool bypass_bloom_queries = false;
};

// Internal counters (docs/m4-design.md section 4). Byte counts include
// only successful work: a WAL record after its sync, an SST or manifest
// after the publication that made it durable.
struct DbStats {
    std::uint64_t gets = 0;              // Get calls that searched
    std::uint64_t candidate_ssts = 0;    // SSTs whose lookup was consulted
    std::uint64_t range_rejections = 0;  // candidates outside the key range
    std::uint64_t filter_rejections = 0; // candidates the filter excluded
    std::uint64_t data_block_reads = 0;  // data blocks requested
    std::uint64_t data_block_bytes = 0;  // bytes requested for them
    std::uint64_t mutations = 0;         // successful Put and Delete calls
    std::uint64_t user_bytes = 0;        // Put: key + value; Delete: key
    // WAL record bytes, each record's 28-byte header included. WAL file
    // headers are not WAL records; they count in metadata_bytes.
    std::uint64_t wal_bytes = 0;
    std::uint64_t flush_sst_bytes = 0;
    std::uint64_t compaction_sst_bytes = 0;
    std::uint64_t metadata_bytes = 0;    // manifests and WAL file headers
    std::uint64_t flushes = 0;
    std::uint64_t compactions = 0;
    std::uint64_t compaction_outputs = 0;
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

    // The handle's counters since Open or the last ResetStats.
    static DbStats Stats(const DB& db);
    static void ResetStats(DB& db);

    // Logical runs and SSTs currently live (for benchmarks and tests).
    static std::size_t LiveRuns(const DB& db);
    static std::size_t LiveSsts(const DB& db);
};

}  // namespace kv
