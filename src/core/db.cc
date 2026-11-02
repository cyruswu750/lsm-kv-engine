#include "db.h"

#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "db_internal.h"
#include "file_lock.h"
#include "memtable.h"
#include "posix_dir.h"
#include "posix_file.h"
#include "types.h"
#include "wal_codec.h"
#include "writable_file.h"

namespace kv {

namespace {

namespace fs = std::filesystem;

constexpr const char* kWalName = "000001.wal";
constexpr const char* kWalTempName = "000001.wal.tmp";
constexpr const char* kLockName = "LOCK";

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

// Classification of a database directory's entries (design.md section 6).
// Kinds come from lstat, so symlinks are never treated as engine files.
struct DirectoryState {
    bool has_wal = false;
    bool has_lock = false;
    bool lock_is_regular = false;
    bool has_temp = false;  // a regular-file 000001.wal.tmp
    std::vector<std::string> unrelated;
};

Status ScanDirectory(const fs::path& dir, DirectoryState& state) {
    std::vector<std::string> names;
    Status s = ListDirectory(dir, names);
    if (!s.ok()) {
        return s;
    }

    DirectoryState result;
    for (const std::string& name : names) {
        if (name == kWalName) {
            result.has_wal = true;
            continue;
        }
        if (name != kLockName && name != kWalTempName) {
            result.unrelated.push_back(name);
            continue;
        }

        PathKind kind;
        s = InspectPath(dir / name, kind);
        if (!s.ok()) {
            return s;
        }
        if (name == kLockName) {
            result.has_lock = true;
            result.lock_is_regular = (kind == PathKind::kRegularFile);
        } else if (kind == PathKind::kRegularFile) {
            result.has_temp = true;
        } else {
            // Only a regular file is the recognized initialization file.
            result.unrelated.push_back(name);
        }
    }

    state = std::move(result);
    return Status::Ok();
}

// Applies the directory-state rules that do not depend on the WAL contents.
Status CheckDirectoryState(const fs::path& dir,
                           const DirectoryState& state,
                           bool create_if_missing) {
    if (state.has_lock && !state.lock_is_regular) {
        return Status::InvalidArgument((dir / kLockName).string() +
                                       ": not a regular file");
    }
    if (state.has_wal) {
        return Status::Ok();
    }
    if (!state.unrelated.empty()) {
        std::string names;
        for (const std::string& name : state.unrelated) {
            names += names.empty() ? "" : ", ";
            names += "\"" + name + "\"";
        }
        return Status::InvalidArgument(
            dir.string() + ": not a database directory; unrelated entries: " +
            names);
    }
    if (!create_if_missing) {
        return Status::NotFound(dir.string() +
                                ": database is not initialized");
    }
    return Status::Ok();
}

// Publishes a new WAL: header -> 000001.wal.tmp -> Sync -> rename ->
// directory sync (design.md section 6, decision D-001).
Status InitializeWal(const fs::path& dir,
                     bool remove_old_temp,
                     PosixSyscalls& syscalls) {
    const fs::path temp = dir / kWalTempName;
    Status s;
    if (remove_old_temp) {
        s = RemoveFile(temp);
        if (!s.ok()) {
            return s;
        }
    }

    std::unique_ptr<WritableFile> file;
    s = NewWritableFile(temp, file, syscalls);
    if (!s.ok()) {
        return s;
    }

    std::string header;
    AppendWalFileHeader(&header);
    s = file->Append(header);
    if (s.ok()) {
        s = file->Sync();
    }
    if (s.ok()) {
        s = file->Close();
    }
    if (!s.ok()) {
        return s;
    }

    s = RenameFile(temp, dir / kWalName);
    if (!s.ok()) {
        return s;
    }
    return SyncDirectory(dir, syscalls);
}

// Replays the WAL into memtable (design.md section 7). The whole file is
// read into memory first (M1 limitation), and memtable copies every
// retained key and value, so peak memory is roughly twice the WAL size.
// An incomplete trailing record is truncated and synced before return;
// any other malformed data fails closed without modifying the file.
Status RecoverWal(const fs::path& dir,
                  PosixSyscalls& syscalls,
                  Memtable& memtable,
                  std::uint64_t& last_sequence) {
    const fs::path wal = dir / kWalName;
    const std::string wal_name = wal.string();

    // Checked under the lock before opening, so a FIFO cannot block the
    // read and a symlink is not followed. The lock does not stop external
    // processes from replacing the entry after this check.
    PathKind kind;
    Status s = InspectPath(wal, kind);
    if (!s.ok()) {
        return s;
    }
    if (kind != PathKind::kRegularFile) {
        return Status::Corruption(wal_name + ": not a regular file");
    }

    std::string data;
    s = ReadFileToString(wal, data, syscalls);
    if (!s.ok()) {
        return s;
    }

    s = ValidateWalFileHeader(data);
    if (!s.ok()) {
        return Status{s.code, wal_name + ": " + s.message};
    }

    std::size_t offset = kWalFileHeaderSize;
    std::uint64_t sequence = 0;
    while (offset < data.size()) {
        const WalDecodeResult r = DecodeWalRecord(data, offset, sequence);
        if (r.kind() == WalDecodeKind::kIncomplete) {
            break;
        }
        if (r.kind() == WalDecodeKind::kCorruption) {
            return Status::Corruption(wal_name + ": " + r.reason());
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
    // Declared first so it is destroyed last, after the WAL is closed.
    std::unique_ptr<FileLock> lock;
    std::unique_ptr<WritableFile> wal;
    Memtable memtable;
    std::uint64_t last_sequence = 0;

    // Set after any failure once WAL persistence has begun. The in-memory
    // state may then disagree with the durable WAL, so every later call
    // returns NeedsRecovery without I/O (design.md section 5, D-002).
    bool needs_recovery = false;

    Status Write(ValueType type, std::string_view key, std::string_view value);
};

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
    if (last_sequence == std::numeric_limits<std::uint64_t>::max()) {
        return Status::InvalidArgument("sequence numbers are exhausted");
    }
    const std::uint64_t sequence = last_sequence + 1;

    std::string record;
    s = AppendWalRecord(&record, sequence, type, key, value);
    if (!s.ok()) {
        return s;
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
    } catch (...) {
        needs_recovery = true;
        throw;
    }

    last_sequence = sequence;
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
    if (out) {
        return Status::InvalidArgument("Open requires an empty output pointer");
    }
    if (options.memtable_bytes == 0) {
        return Status::InvalidArgument("memtable_bytes must be positive");
    }
    Status s = CheckSyscallPath(directory, "open database");
    if (!s.ok()) {
        return s;
    }

    const fs::path dir = StripTrailingSeparators(directory);

    PathKind kind;
    s = InspectPath(dir, kind);
    if (!s.ok()) {
        return s;
    }
    if (kind == PathKind::kMissing) {
        if (!options.create_if_missing) {
            return Status::NotFound(dir.string() + ": does not exist");
        }
        // One level only; a missing parent is an error.
        bool created = false;
        s = CreateDirectory(dir, created);
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

    // First pass, before the lock: decides NotFound/InvalidArgument without
    // creating LOCK in a directory this call must not initialize.
    DirectoryState state;
    s = ScanDirectory(dir, state);
    if (!s.ok()) {
        return s;
    }
    s = CheckDirectoryState(dir, state, options.create_if_missing);
    if (!s.ok()) {
        return s;
    }

    auto impl = std::make_unique<DB::Impl>();
    s = LockFile(dir / kLockName, impl->lock);
    if (!s.ok()) {
        return s;
    }

    // Second pass under the lock: another process may have changed the
    // directory between the first scan and lock acquisition.
    s = ScanDirectory(dir, state);
    if (!s.ok()) {
        return s;
    }
    s = CheckDirectoryState(dir, state, options.create_if_missing);
    if (!s.ok()) {
        return s;
    }

    if (state.has_wal) {
        s = RecoverWal(dir, syscalls, impl->memtable, impl->last_sequence);
    } else {
        s = InitializeWal(dir, state.has_temp, syscalls);
    }
    if (!s.ok()) {
        return s;
    }

    s = OpenAppendableFile(dir / kWalName, impl->wal, syscalls);
    if (!s.ok()) {
        return s;
    }

    if (state.has_wal && state.has_temp) {
        // A leftover initialization file is non-authoritative once a valid
        // WAL exists. Its removal need not be durable: if it reappears
        // after a crash, the next Open removes it again.
        s = RemoveFile(dir / kWalTempName);
        if (!s.ok()) {
            return s;
        }
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
    // is still being read.
    std::string value;
    if (impl_->memtable.Lookup(key, &value) != LookupResult::kFound) {
        out_value.clear();
        return Status::NotFound();
    }
    out_value = std::move(value);
    return Status::Ok();
}

}  // namespace kv
