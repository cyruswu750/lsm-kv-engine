#include "db.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "compaction.h"
#include "db_internal.h"
#include "file_lock.h"
#include "file_names.h"
#include "manifest.h"
#include "memtable.h"
#include "posix_dir.h"
#include "posix_file.h"
#include "sst.h"
#include "types.h"
#include "wal_codec.h"
#include "writable_file.h"

namespace kv {

namespace {

namespace fs = std::filesystem;

constexpr std::uint64_t kMaxU64 = std::numeric_limits<std::uint64_t>::max();

// "db/" -> "db", so that parent_path() names the real parent directory.
fs::path StripTrailingSeparators(fs::path path) {
    while (!path.has_filename() && path.has_relative_path()) {
        path = path.parent_path();
    }
    return path;
}

fs::path ParentDirectory(const fs::path& dir) {
    fs::path parent = dir.parent_path();
    return parent.empty() ? fs::path(".") : parent;
}

std::string JoinNames(const std::vector<std::string>& names) {
    std::string joined;
    for (const std::string& name : names) {
        joined += joined.empty() ? "" : ", ";
        joined += "\"" + name + "\"";
    }
    return joined;
}

struct EngineFile {
    std::string name;
    std::uint64_t id = 0;
    bool regular = false;
};

// Classification of a database directory's entries (docs/sst-format.md
// section 8). Kinds come from lstat, so symlinks are never engine files.
struct DirectoryState {
    bool has_manifest = false;  // any kind; validated during recovery
    bool has_lock = false;
    bool lock_is_regular = false;
    bool has_m1 = false;
    bool has_manifest_temp = false;      // a regular MANIFEST.tmp
    std::vector<EngineFile> engine_files;  // every kind, for ID high-water
    std::vector<std::string> residue;      // regular engine files
    std::vector<std::string> unrelated;
};

Status ScanDirectory(const fs::path& dir,
                     DirectoryState& state,
                     PosixSyscalls& syscalls) {
    std::vector<std::string> names;
    Status s = ListDirectory(dir, names, syscalls);
    if (!s.ok()) {
        return s;
    }

    DirectoryState result;
    for (const std::string& name : names) {
        if (name == kManifestFileName) {
            result.has_manifest = true;
            continue;
        }
        if (name == kM1WalFileName) {
            result.has_m1 = true;
            continue;
        }
        FileKind file_kind;
        std::uint64_t id = 0;
        const bool engine = ParseFileName(name, file_kind, id);
        if (!engine && name != kLockFileName &&
            name != kManifestTempFileName && name != kM1WalTempFileName) {
            result.unrelated.push_back(name);
            continue;
        }

        PathKind kind;
        s = InspectPath(dir / name, kind, syscalls);
        if (!s.ok()) {
            return s;
        }
        const bool regular = (kind == PathKind::kRegularFile);
        if (name == kLockFileName) {
            result.has_lock = true;
            result.lock_is_regular = regular;
        } else if (!regular) {
            // Only regular files are engine residue, as in M1.
            result.unrelated.push_back(name);
            if (engine) {
                result.engine_files.push_back({name, id, false});
            }
        } else if (name == kM1WalTempFileName) {
            result.has_m1 = true;
        } else if (name == kManifestTempFileName) {
            result.has_manifest_temp = true;
            result.residue.push_back(name);
        } else {
            result.engine_files.push_back({name, id, true});
            result.residue.push_back(name);
        }
    }

    state = std::move(result);
    return Status::Ok();
}

// Applies the directory-state rules that do not depend on file contents.
Status CheckDirectoryState(const fs::path& dir,
                           const DirectoryState& state,
                           bool create_if_missing) {
    if (state.has_lock && !state.lock_is_regular) {
        return Status::InvalidArgument((dir / kLockFileName).string() +
                                       ": not a regular file");
    }
    if (state.has_manifest) {
        return Status::Ok();
    }
    if (state.has_m1) {
        return Status::UnsupportedFormat(
            dir.string() +
            ": M1 database directory (000001.wal); this version does not "
            "open or migrate M1 databases");
    }
    if (!state.residue.empty()) {
        return Status::Corruption(
            dir.string() +
            ": no MANIFEST, but files from an interrupted initialization "
            "remain: " +
            JoinNames(state.residue) +
            "; they are preserved and the directory is not reinitialized");
    }
    if (!state.unrelated.empty()) {
        return Status::InvalidArgument(
            dir.string() + ": not a database directory; unrelated entries: " +
            JoinNames(state.unrelated));
    }
    if (!create_if_missing) {
        return Status::NotFound(dir.string() +
                                ": database is not initialized");
    }
    return Status::Ok();
}

// Creates temp with exactly bytes, syncs and closes it, then renames it
// to final. The rename is not durable until the directory is synced.
Status PublishFile(const fs::path& temp,
                   const fs::path& final_path,
                   std::string_view bytes,
                   PosixSyscalls& syscalls) {
    std::unique_ptr<WritableFile> file;
    Status s = NewWritableFile(temp, file, syscalls);
    if (!s.ok()) {
        return s;
    }
    s = file->Append(bytes);
    if (s.ok()) {
        s = file->Sync();
    }
    if (s.ok()) {
        s = file->Close();
    }
    if (!s.ok()) {
        return s;
    }
    return RenameFile(temp, final_path, syscalls);
}

// Checks that a file named by the manifest exists as a regular file.
Status CheckReferencedFile(const fs::path& path, PosixSyscalls& syscalls) {
    PathKind kind;
    Status s = InspectPath(path, kind, syscalls);
    if (!s.ok()) {
        return s;
    }
    if (kind == PathKind::kMissing) {
        return Status::Corruption(path.string() +
                                  ": referenced by MANIFEST but missing");
    }
    if (kind != PathKind::kRegularFile) {
        return Status::Corruption(path.string() + ": not a regular file");
    }
    return Status::Ok();
}

// Replays the active WAL into memtable (design.md section 7). The whole
// file is read into memory first (D-007). Every record must exceed
// checkpoint; passing it as the previous sequence makes the decoder reject
// a complete header at or below it as Corruption, never as a torn tail.
// An incomplete trailing record is truncated and synced before return.
Status RecoverWal(const fs::path& wal,
                  std::uint64_t checkpoint,
                  PosixSyscalls& syscalls,
                  Memtable& memtable,
                  std::uint64_t& last_sequence) {
    const std::string wal_name = wal.string();

    std::string data;
    Status s = ReadFileToString(wal, data, syscalls);
    if (!s.ok()) {
        return s;
    }

    s = ValidateWalFileHeader(data);
    if (!s.ok()) {
        return Status{s.code, wal_name + ": " + s.message};
    }

    std::size_t offset = kWalFileHeaderSize;
    std::uint64_t sequence = checkpoint;
    while (offset < data.size()) {
        const WalDecodeResult r = DecodeWalRecord(data, offset, sequence);
        if (r.kind() == WalDecodeKind::kIncomplete) {
            break;
        }
        if (r.kind() == WalDecodeKind::kCorruption) {
            std::string message = wal_name + ": " + r.reason();
            if (offset == kWalFileHeaderSize && checkpoint != 0) {
                message += " (the previous sequence is the manifest "
                           "checkpoint)";
            }
            return Status::Corruption(message);
        }

        const WalRecord& record = r.record();
        memtable.Apply(record.sequence, record.type, record.key, record.value);
        sequence = record.sequence;
        offset = r.next_offset();
    }

    if (offset < data.size()) {
        // Never scan ahead: everything after the last complete record is
        // an interrupted append and must be gone before the next append.
        s = TruncateAndSyncFile(wal, offset, syscalls);
        if (!s.ok()) {
            return s;
        }
    }

    last_sequence = sequence;
    return Status::Ok();
}

}  // namespace

struct DB::Impl {
    // An engine file not referenced by the manifest, kept for a later
    // cleanup attempt. id is 0 for MANIFEST.tmp.
    struct Orphan {
        std::string name;
        std::uint64_t id = 0;
    };

    // Declared first so it is destroyed last, after every file is closed.
    std::unique_ptr<FileLock> lock;
    fs::path dir;
    PosixSyscalls* syscalls = nullptr;
    std::size_t memtable_bytes = 0;
    std::size_t compaction_trigger = 0;
    SstLimits sst_limits;
    std::uint32_t max_manifest_ssts = kMaxManifestSsts;

    std::unique_ptr<WritableFile> wal;
    Memtable memtable;
    std::uint64_t last_sequence = 0;

    Manifest manifest;  // the durable, installed generation
    std::vector<std::unique_ptr<SstTable>> tables;  // parallel to ssts
    // Next ID to allocate: at least manifest.next_file_id and above every
    // engine-file ID observed at Open (docs/sst-format.md section 7).
    std::uint64_t next_file_id = 0;
    std::vector<Orphan> deferred_orphans;

    // Set after any failure once persistence has begun. The in-memory
    // state may then disagree with durable state, so every later call
    // returns NeedsRecovery without I/O (design.md section 5, D-002).
    bool needs_recovery = false;

    Status Initialize();
    Status Recover(const DirectoryState& state);
    void RemoveOrphansAtOpen(const DirectoryState& state);
    Status Write(ValueType type, std::string_view key, std::string_view value);
    Status Flush();
    Status PublishFlush();
    Status WriteSst(std::uint64_t id,
                    ManifestSst& meta,
                    std::unique_ptr<SstTable>& table);
    Status Compact();
    Status PublishCompaction();
    Status PublishManifest(const Manifest& next);
    Status RemoveDeferredOrphans();
};

// docs/sst-format.md section 8: WAL first, then the generation-1
// manifest, each published through a temporary file and a directory sync.
Status DB::Impl::Initialize() {
    const std::string wal_name = WalFileName(1);
    std::string header;
    AppendWalFileHeader(&header);
    Status s = PublishFile(dir / TempFileName(wal_name), dir / wal_name,
                           header, *syscalls);
    if (s.ok()) {
        s = SyncDirectory(dir, *syscalls);
    }
    if (!s.ok()) {
        return s;
    }

    Manifest initial;
    initial.generation = 1;
    initial.next_file_id = 2;
    initial.checkpoint = 0;
    initial.active_wal_id = 1;
    std::string bytes;
    EncodeManifest(initial, &bytes);
    s = PublishFile(dir / kManifestTempFileName, dir / kManifestFileName,
                    bytes, *syscalls);
    if (s.ok()) {
        s = SyncDirectory(dir, *syscalls);
    }
    if (s.ok()) {
        s = OpenAppendableFile(dir / wal_name, wal, *syscalls);
    }
    if (!s.ok()) {
        return s;
    }

    manifest = std::move(initial);
    next_file_id = manifest.next_file_id;
    last_sequence = 0;
    return Status::Ok();
}

// docs/sst-format.md section 9.
Status DB::Impl::Recover(const DirectoryState& state) {
    const fs::path manifest_path = dir / kManifestFileName;
    PathKind kind;
    Status s = InspectPath(manifest_path, kind, *syscalls);
    if (!s.ok()) {
        return s;
    }
    if (kind != PathKind::kRegularFile) {
        return Status::Corruption(manifest_path.string() +
                                  ": not a regular file");
    }
    Manifest m;
    s = ReadManifestFile(manifest_path, *syscalls, m);
    if (!s.ok()) {
        return s;
    }

    // Every reference must exist before any content is trusted.
    const fs::path wal_path = dir / WalFileName(m.active_wal_id);
    s = CheckReferencedFile(wal_path, *syscalls);
    if (!s.ok()) {
        return s;
    }
    for (const ManifestSst& sst : m.ssts) {
        s = CheckReferencedFile(dir / SstFileName(sst.file_id), *syscalls);
        if (!s.ok()) {
            return s;
        }
    }

    // Reserve room for the next flush so installing it cannot allocate.
    tables.reserve(m.ssts.size() + 1);
    for (const ManifestSst& sst : m.ssts) {
        const fs::path path = dir / SstFileName(sst.file_id);
        std::unique_ptr<SstTable> table;
        s = SstTable::Open(path, sst.file_size, *syscalls, table);
        if (!s.ok()) {
            return s;
        }
        const SstSummary& summary = table->summary();
        if (summary.min_sequence != sst.min_sequence ||
            summary.max_sequence != sst.max_sequence) {
            return Status::Corruption(
                path.string() + ": sequence range [" +
                std::to_string(summary.min_sequence) + ", " +
                std::to_string(summary.max_sequence) +
                "] differs from the manifest's [" +
                std::to_string(sst.min_sequence) + ", " +
                std::to_string(sst.max_sequence) + "]");
        }
        tables.push_back(std::move(table));
    }
    // Base SSTs must be key-ordered and key-disjoint; their key ranges come
    // from the validated SSTs (docs/sst-format.md section 4.1).
    for (std::size_t i = 1; i < m.base_run_count; ++i) {
        if (!BytewiseLess{}(tables[i - 1]->summary().largest_key(),
                            tables[i]->summary().smallest_key)) {
            return Status::Corruption(
                tables[i]->path() +
                ": base run SST does not start after the previous base SST's "
                "largest key");
        }
    }

    std::uint64_t replayed = 0;
    s = RecoverWal(wal_path, m.checkpoint, *syscalls, memtable, replayed);
    if (!s.ok()) {
        return s;
    }
    s = OpenAppendableFile(wal_path, wal, *syscalls);
    if (!s.ok()) {
        return s;
    }

    last_sequence = std::max(m.checkpoint, replayed);
    manifest = std::move(m);

    next_file_id = manifest.next_file_id;
    for (const EngineFile& file : state.engine_files) {
        next_file_id =
            file.id == kMaxU64 ? kMaxU64 : std::max(next_file_id, file.id + 1);
    }

    // The visible MANIFEST may come from a rename whose directory sync never
    // completed (a crash, or a failed sync before the last handle was
    // invalidated). Make it durable before retiring files it superseded or
    // acknowledging writes that depend on it (design.md section 8, step 4).
    s = SyncDirectory(dir, *syscalls);
    if (!s.ok()) {
        return s;
    }
    RemoveOrphansAtOpen(state);
    return Status::Ok();
}

// Runs after Open's directory sync, so the manifest that covers these
// orphans is durable. The removals themselves are best effort and not
// synced: a removed orphan that reappears after a crash still has an ID
// below the durable next_file_id, so the next Open removes it again.
// Orphans at or above that high-water mark are the only durable evidence
// of their IDs and are kept until a published manifest covers them.
void DB::Impl::RemoveOrphansAtOpen(const DirectoryState& state) {
    std::unordered_set<std::string> referenced;
    referenced.insert(WalFileName(manifest.active_wal_id));
    for (const ManifestSst& sst : manifest.ssts) {
        referenced.insert(SstFileName(sst.file_id));
    }

    for (const EngineFile& file : state.engine_files) {
        if (!file.regular || referenced.count(file.name) != 0) {
            continue;
        }
        if (file.id >= manifest.next_file_id ||
            !RemoveFile(dir / file.name, *syscalls).ok()) {
            deferred_orphans.push_back({file.name, file.id});
        }
    }
    if (state.has_manifest_temp &&
        !RemoveFile(dir / kManifestTempFileName, *syscalls).ok()) {
        deferred_orphans.push_back({std::string(kManifestTempFileName), 0});
    }
}

Status DB::Impl::Write(ValueType type,
                       std::string_view key,
                       std::string_view value) {
    if (needs_recovery) {
        return Status::NeedsRecovery(
            "a previous write failed; reopen the database");
    }

    Status s = ValidateKeyValue(key, value);
    if (!s.ok()) {
        return s;
    }

    // A recovered UINT64_MAX is valid, but no mutation can follow it.
    if (last_sequence == kMaxU64) {
        return Status::InvalidArgument("sequence numbers are exhausted");
    }
    const std::uint64_t sequence = last_sequence + 1;

    std::string record;
    s = AppendWalRecord(&record, sequence, type, key, value);
    if (!s.ok()) {
        return s;
    }

    // Flush only after this mutation is known to be valid, so a flush
    // error is never reported for a mutation that could not have run.
    if (!memtable.empty() && memtable.ApproximateBytes() >= memtable_bytes) {
        s = Flush();
        if (!s.ok()) {
            return s;
        }
    }
    // After any flush, so a flush that reaches the trigger is compacted
    // before this mutation (docs/m3-design.md section 3).
    // The base run counts once, so a multi-SST base is not compacted again
    // until trigger - 1 new flushes exist (docs/m3-design.md section 3).
    if (compaction_trigger != 0 && RunCount(manifest) >= compaction_trigger) {
        s = Compact();
        if (!s.ok()) {
            return s;
        }
    }

    try {
        s = wal->Append(record);
        if (s.ok()) {
            s = wal->Sync();
        }
        if (!s.ok()) {
            // Never retry: the record may or may not be durable.
            needs_recovery = true;
            return Status::IOError(s.message);
        }
        memtable.Apply(sequence, type, key, value);
        last_sequence = sequence;
        syscalls->Checkpoint("write.applied");
    } catch (...) {
        needs_recovery = true;
        throw;
    }
    return Status::Ok();
}

// Counter checks need no I/O, so their failures leave the handle usable.
// Any failure after the first flush I/O invalidates it.
Status DB::Impl::Flush() {
    if (manifest.generation == kMaxU64) {
        return Status::InvalidArgument("manifest generations are exhausted");
    }
    if (next_file_id > kMaxU64 - 2) {
        return Status::InvalidArgument("file IDs are exhausted");
    }
    if (manifest.ssts.size() >= max_manifest_ssts) {
        return Status::InvalidArgument(
            "the manifest already lists the maximum number of SSTs");
    }

    try {
        Status s = PublishFlush();
        if (!s.ok()) {
            needs_recovery = true;
        }
        return s;
    } catch (...) {
        needs_recovery = true;
        throw;
    }
}

// Steps F1-F7 of docs/sst-format.md section 10.
Status DB::Impl::PublishFlush() {
    const std::uint64_t sst_id = next_file_id;
    const std::uint64_t wal_id = sst_id + 1;
    // Never reused, even if this publication fails.
    next_file_id = wal_id + 1;

    // Prepare memory for installation before any I/O.
    Manifest next = manifest;
    next.generation += 1;
    next.next_file_id = wal_id + 1;
    next.checkpoint = last_sequence;
    next.active_wal_id = wal_id;
    next.ssts.reserve(next.ssts.size() + 1);
    tables.reserve(tables.size() + 1);
    deferred_orphans.reserve(deferred_orphans.size() + 1);
    std::string header;
    AppendWalFileHeader(&header);

    // F1
    ManifestSst meta;
    std::unique_ptr<SstTable> table;
    Status s = WriteSst(sst_id, meta, table);
    if (!s.ok()) {
        return s;
    }
    next.ssts.push_back(meta);

    // F2
    const std::string wal_name = WalFileName(wal_id);
    std::unique_ptr<WritableFile> new_wal;
    s = PublishFile(dir / TempFileName(wal_name), dir / wal_name, header,
                    *syscalls);
    if (s.ok()) {
        s = OpenAppendableFile(dir / wal_name, new_wal, *syscalls);
    }
    // F3: one sync makes both new names durable before any manifest
    // refers to them.
    if (s.ok()) {
        s = SyncDirectory(dir, *syscalls);
    }
    if (!s.ok()) {
        return s;
    }

    // F4, F5
    s = PublishManifest(next);
    if (!s.ok()) {
        return s;
    }

    // F6: the new generation is durable. None of these steps allocates.
    const std::uint64_t old_wal_id = manifest.active_wal_id;
    std::unique_ptr<WritableFile> old_wal = std::move(wal);
    wal = std::move(new_wal);
    manifest = std::move(next);
    tables.push_back(std::move(table));
    memtable = Memtable();
    syscalls->Checkpoint("flush.installed");

    // F7. The old WAL was synced before publication, so its close result
    // does not affect durability; the descriptor is released either way.
    old_wal->Close();
    old_wal.reset();
    deferred_orphans.push_back({WalFileName(old_wal_id), old_wal_id});
    return RemoveDeferredOrphans();
}

// F4 and F5 (C3 and C4 for compaction). A MANIFEST.tmp that Open failed to
// remove would make the exclusive create fail, so its removal is retried
// first; a failure there is a publication failure.
Status DB::Impl::PublishManifest(const Manifest& next) {
    const auto stale = std::find_if(
        deferred_orphans.begin(), deferred_orphans.end(),
        [](const Orphan& o) { return o.name == kManifestTempFileName; });
    if (stale != deferred_orphans.end()) {
        Status s = RemoveFile(dir / kManifestTempFileName, *syscalls);
        if (!s.ok()) {
            return s;
        }
        deferred_orphans.erase(stale);
    }
    std::string bytes;
    EncodeManifest(next, &bytes);
    Status s = PublishFile(dir / kManifestTempFileName,
                           dir / kManifestFileName, bytes, *syscalls);
    if (s.ok()) {
        s = SyncDirectory(dir, *syscalls);
    }
    return s;
}

// Counter checks need no I/O, so their failures leave the handle usable.
// Any failure after the first compaction I/O invalidates it.
Status DB::Impl::Compact() {
    if (manifest.generation == kMaxU64) {
        return Status::InvalidArgument("manifest generations are exhausted");
    }
    // An output needs one ID, and next_file_id must stay representable.
    if (next_file_id > kMaxU64 - 1) {
        return Status::InvalidArgument("file IDs are exhausted");
    }

    try {
        Status s = PublishCompaction();
        if (!s.ok()) {
            needs_recovery = true;
        }
        return s;
    } catch (...) {
        needs_recovery = true;
        throw;
    }
}

// Steps C1-C6 of docs/m3-design.md section 4.
Status DB::Impl::PublishCompaction() {
    // Prepare memory for installation and cleanup before any I/O.
    Manifest next = manifest;
    next.generation += 1;
    next.ssts.clear();
    std::vector<std::unique_ptr<SstTable>> next_tables;
    deferred_orphans.reserve(deferred_orphans.size() + tables.size());
    // The inputs become orphans once the new manifest is durable; their
    // names are built now so that installing them (C5) only moves them.
    std::vector<Orphan> retired;
    retired.reserve(manifest.ssts.size());
    for (const ManifestSst& input : manifest.ssts) {
        retired.push_back({SstFileName(input.file_id), input.file_id});
    }
    std::vector<const SstTable*> inputs;
    inputs.reserve(tables.size());
    for (const auto& table : tables) {
        inputs.push_back(table.get());
    }

    // C1. Every live SST is an input and every newer version is in the
    // memtable or WAL, so a winning tombstone shadows nothing that could
    // resurface: it is dropped together with its key.
    std::unique_ptr<MergingIterator> merge;
    Status s = MergingIterator::Open(inputs, merge);
    if (!s.ok()) {
        return s;
    }
    // Outputs are split before either SST limit would be exceeded, so they
    // are key-ordered and key-disjoint. Their count leaves room in the
    // manifest for the flushes that lead to the next compaction.
    const std::size_t max_outputs =
        max_manifest_ssts - (compaction_trigger - 1);
    std::uint64_t out_id = 0;
    std::unique_ptr<WritableFile> file;
    std::unique_ptr<SstBuilder> builder;
    auto finish_output = [&]() -> Status {
        const std::string name = SstFileName(out_id);
        Status r = builder->Finish();
        if (r.ok()) {
            r = file->Sync();
        }
        if (r.ok()) {
            r = file->Close();
        }
        if (r.ok()) {
            r = RenameFile(dir / TempFileName(name), dir / name, *syscalls);
        }
        if (!r.ok()) {
            return r;
        }
        SstSummary& summary = builder->summary();
        next.ssts.push_back({out_id, summary.file_size, summary.min_sequence,
                             summary.max_sequence});
        next_tables.push_back(std::make_unique<SstTable>(
            (dir / name).string(), *syscalls, std::move(summary)));
        builder.reset();
        file.reset();
        return Status::Ok();
    };
    while (merge->Valid()) {
        const SstEntry& e = merge->entry();
        if (e.type == ValueType::kPut) {
            if (builder && !builder->CanAdd(e.key, e.value)) {
                s = finish_output();
                if (!s.ok()) {
                    return s;
                }
            }
            if (!builder) {
                if (next.ssts.size() >= max_outputs) {
                    return Status::InvalidArgument(
                        "compaction output needs more than " +
                        std::to_string(max_outputs) + " SSTs");
                }
                if (next_file_id > kMaxU64 - 1) {
                    return Status::InvalidArgument("file IDs are exhausted");
                }
                // Never reused, even if this compaction fails.
                out_id = next_file_id;
                next_file_id = out_id + 1;
                s = NewWritableFile(dir / TempFileName(SstFileName(out_id)),
                                    file, *syscalls);
                if (!s.ok()) {
                    return s;
                }
                builder = std::make_unique<SstBuilder>(*file, sst_limits);
            }
            s = builder->Add(e.key, e.sequence, e.type, e.value);
            if (!s.ok()) {
                return s;
            }
        }
        s = merge->Next();
        if (!s.ok()) {
            return s;
        }
    }
    s = merge->Close();
    // The merge refers to the input tables, which installation releases.
    merge.reset();
    if (!s.ok()) {
        return s;
    }
    if (builder) {
        s = finish_output();
        if (!s.ok()) {
            return s;
        }
    }
    // C2: every output's contents were synced above; one directory sync
    // makes all their names durable before a manifest names them.
    if (!next.ssts.empty()) {
        s = SyncDirectory(dir, *syscalls);
        if (!s.ok()) {
            return s;
        }
    }
    next.base_run_count = static_cast<std::uint32_t>(next.ssts.size());
    // Covers the outputs and any high-ID orphan seen at Open (D-013).
    next.next_file_id = next_file_id;

    // C3, C4
    s = PublishManifest(next);
    if (!s.ok()) {
        return s;
    }

    // C5: the new generation is durable. None of these steps allocates.
    for (Orphan& input : retired) {
        deferred_orphans.push_back(std::move(input));
    }
    manifest = std::move(next);
    tables.swap(next_tables);
    syscalls->Checkpoint("compaction.installed");

    // C6. The inputs are no longer referenced by the durable manifest.
    return RemoveDeferredOrphans();
}

Status DB::Impl::WriteSst(std::uint64_t id,
                          ManifestSst& meta,
                          std::unique_ptr<SstTable>& table) {
    const std::string name = SstFileName(id);
    const fs::path temp = dir / TempFileName(name);
    const fs::path final_path = dir / name;

    std::unique_ptr<WritableFile> file;
    Status s = NewWritableFile(temp, file, *syscalls);
    if (!s.ok()) {
        return s;
    }
    SstBuilder builder(*file, sst_limits);
    s = BuildSstFromMemtable(memtable, builder);
    if (s.ok()) {
        s = file->Sync();
    }
    if (s.ok()) {
        s = file->Close();
    }
    if (s.ok()) {
        s = RenameFile(temp, final_path, *syscalls);
    }
    if (!s.ok()) {
        return s;
    }

    SstSummary& summary = builder.summary();
    meta.file_id = id;
    meta.file_size = summary.file_size;
    meta.min_sequence = summary.min_sequence;
    meta.max_sequence = summary.max_sequence;
    table = std::make_unique<SstTable>(final_path.string(), *syscalls,
                                       std::move(summary));
    return Status::Ok();
}

// Removes every deferred orphan below the durable next_file_id. Unlink
// failures leave garbage for a later attempt and are not errors. A failed
// directory sync is reported and invalidates the handle; it is not retried
// (design.md section 8).
Status DB::Impl::RemoveDeferredOrphans() {
    std::vector<Orphan> kept;
    bool removed = false;
    for (Orphan& orphan : deferred_orphans) {
        if (orphan.id >= manifest.next_file_id ||
            !RemoveFile(dir / orphan.name, *syscalls).ok()) {
            kept.push_back(std::move(orphan));
            continue;
        }
        removed = true;
    }
    deferred_orphans = std::move(kept);

    if (removed) {
        Status s = SyncDirectory(dir, *syscalls);
        if (!s.ok()) {
            return s;
        }
    }
    return Status::Ok();
}

DB::DB(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

DB::~DB() = default;

Status DB::Open(const std::filesystem::path& directory,
                const Options& options,
                std::unique_ptr<DB>& out) {
    return DBOpener::Open(directory, options, DefaultPosixSyscalls(), out);
}

Status DBOpener::Open(const std::filesystem::path& directory,
                      const Options& options,
                      PosixSyscalls& syscalls,
                      std::unique_ptr<DB>& out) {
    return Open(directory, options, InternalOptions{}, syscalls, out);
}

Status DBOpener::Open(const std::filesystem::path& directory,
                      const Options& options,
                      const InternalOptions& internal,
                      PosixSyscalls& syscalls,
                      std::unique_ptr<DB>& out) {
    if (out) {
        return Status::InvalidArgument("Open requires an empty output pointer");
    }
    if (options.memtable_bytes == 0) {
        return Status::InvalidArgument("memtable_bytes must be positive");
    }
    const std::uint32_t max_manifest_ssts =
        std::min(internal.max_manifest_ssts, kMaxManifestSsts);
    if (internal.compaction_trigger == 1 ||
        internal.compaction_trigger > max_manifest_ssts) {
        return Status::InvalidArgument(
            "compaction_trigger must be 0 (disabled) or at least 2, and at "
            "most the manifest SST limit");
    }
    Status s = CheckSyscallPath(directory, "open database");
    if (!s.ok()) {
        return s;
    }

    const fs::path dir = StripTrailingSeparators(directory);

    PathKind kind;
    s = InspectPath(dir, kind, syscalls);
    if (!s.ok()) {
        return s;
    }
    if (kind == PathKind::kMissing) {
        if (!options.create_if_missing) {
            return Status::NotFound(dir.string() + ": does not exist");
        }
        // One level only; a missing parent is an error.
        bool created = false;
        s = CreateDirectory(dir, created, syscalls);
        if (!s.ok()) {
            return s;
        }
        if (created) {
            s = SyncDirectory(ParentDirectory(dir), syscalls);
            if (!s.ok()) {
                return s;
            }
        }
    } else if (kind != PathKind::kDirectory) {
        return Status::InvalidArgument(
            dir.string() +
            ": not a directory (symbolic links are not followed)");
    }

    // First pass, before the lock: decides every rejecting state without
    // creating LOCK in a directory this call must not touch.
    DirectoryState state;
    s = ScanDirectory(dir, state, syscalls);
    if (!s.ok()) {
        return s;
    }
    s = CheckDirectoryState(dir, state, options.create_if_missing);
    if (!s.ok()) {
        return s;
    }

    auto impl = std::make_unique<DB::Impl>();
    impl->dir = dir;
    impl->syscalls = &syscalls;
    impl->memtable_bytes = options.memtable_bytes;
    impl->compaction_trigger = internal.compaction_trigger;
    impl->max_manifest_ssts = max_manifest_ssts;
    impl->sst_limits = internal.sst_limits;
    s = LockFile(dir / kLockFileName, impl->lock, syscalls);
    if (!s.ok()) {
        return s;
    }

    // Second pass under the lock: another process may have changed the
    // directory between the first scan and lock acquisition.
    s = ScanDirectory(dir, state, syscalls);
    if (!s.ok()) {
        return s;
    }
    s = CheckDirectoryState(dir, state, options.create_if_missing);
    if (!s.ok()) {
        return s;
    }

    s = state.has_manifest ? impl->Recover(state) : impl->Initialize();
    if (!s.ok()) {
        return s;
    }

    out.reset(new DB(std::move(impl)));
    return Status::Ok();
}

Status DB::Put(std::string_view key, std::string_view value) {
    return impl_->Write(ValueType::kPut, key, value);
}

Status DB::Delete(std::string_view key) {
    return impl_->Write(ValueType::kDelete, key, std::string_view());
}

Status DB::Get(std::string_view key, std::string& out_value) const {
    if (impl_->needs_recovery) {
        out_value.clear();
        return Status::NeedsRecovery(
            "a previous write failed; reopen the database");
    }
    if (key.size() > kMaxKeyLength) {
        out_value.clear();
        return Status::InvalidArgument(
            "key length " + std::to_string(key.size()) + " exceeds limit " +
            std::to_string(kMaxKeyLength));
    }

    // Look up into a local first so out_value is not modified while key
    // is still being read. The memtable holds the newest versions. Flush
    // runs have disjoint, ordered sequence intervals, all newer than the
    // base run. Base SSTs may have overlapping sequence intervals, but
    // their key ranges are disjoint, so at most one of them holds key.
    // Stopping at the first value or tombstone is therefore exact.
    std::string value;
    LookupResult result = impl_->memtable.Lookup(key, &value);
    const auto& tables = impl_->tables;
    const std::size_t base = impl_->manifest.base_run_count;
    // Flush runs, newest first; then the base run, which is older than all
    // of them and holds each key in at most one SST. A read error changes
    // nothing durable, so the handle stays usable.
    for (std::size_t i = tables.size();
         result == LookupResult::kAbsent && i > base; --i) {
        const Status s = tables[i - 1]->Get(key, result, &value);
        if (!s.ok()) {
            out_value.clear();
            return s;
        }
    }
    if (result == LookupResult::kAbsent && base > 0) {
        const auto end = tables.begin() + static_cast<std::ptrdiff_t>(base);
        const auto it = std::lower_bound(
            tables.begin(), end, key,
            [](const std::unique_ptr<SstTable>& t, std::string_view k) {
                return BytewiseLess{}(t->summary().largest_key(), k);
            });
        if (it != end) {
            const Status s = (*it)->Get(key, result, &value);
            if (!s.ok()) {
                out_value.clear();
                return s;
            }
        }
    }
    if (result != LookupResult::kFound) {
        out_value.clear();
        return Status::NotFound();
    }
    out_value = std::move(value);
    return Status::Ok();
}

}  // namespace kv
