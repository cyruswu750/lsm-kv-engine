#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "posix_file.h"
#include "status.h"

namespace kv {

// Directory-entry primitives over the PosixSyscalls seam (the real calls
// by default). None of these operations syncs a directory; callers decide
// which directories need SyncDirectory to make namespace changes durable.

// Returns InvalidArgument for an empty path or one containing a NUL byte,
// which C APIs would silently truncate. Every function below checks this
// before making any syscall.
Status CheckSyscallPath(const std::filesystem::path& path,
                        const char* operation);

enum class PathKind {
    kMissing,
    kRegularFile,
    kDirectory,
    kOther  // includes every symbolic link, dangling or not
};

// Classifies path with lstat, so symbolic links are not followed: a
// dangling symlink is kOther, not kMissing. Only ENOENT maps to kMissing;
// other failures (EACCES, ENOTDIR, EIO, ...) return IOError and leave
// out_kind unchanged.
Status InspectPath(const std::filesystem::path& path,
                   PathKind& out_kind,
                   PosixSyscalls& syscalls = DefaultPosixSyscalls());

// Replaces out_names with the immediate entry names of dir, excluding "."
// and "..", in unspecified order. Hidden and unrelated entries are
// included. On failure out_names is unchanged; a partial listing is never
// returned as success.
Status ListDirectory(const std::filesystem::path& dir,
                     std::vector<std::string>& out_names,
                     PosixSyscalls& syscalls = DefaultPosixSyscalls());

// Creates dir (one level; parents are not created). Sets out_created to
// true if this call created it, or false if a directory already existed
// there (checked with lstat, so a symlink to a directory is an error).
// An existing nondirectory or a missing parent returns IOError. On
// failure out_created is unchanged. Creation is not durable until the
// parent directory is synced.
Status CreateDirectory(const std::filesystem::path& dir,
                       bool& out_created,
                       PosixSyscalls& syscalls = DefaultPosixSyscalls());

// rename(2): if to exists it is atomically replaced, so observers see
// either the old or the new file at to, never neither. If from is a
// symlink the link itself is renamed. Not durable until the containing
// directory (both directories, if they differ) is synced.
Status RenameFile(const std::filesystem::path& from,
                  const std::filesystem::path& to,
                  PosixSyscalls& syscalls = DefaultPosixSyscalls());

// unlink(2): removes exactly one non-directory entry. A directory is
// never removed (IOError). A symlink is removed, not its target. Not
// durable until the containing directory is synced.
Status RemoveFile(const std::filesystem::path& path,
                  PosixSyscalls& syscalls = DefaultPosixSyscalls());

// Opens dir with O_RDONLY | O_DIRECTORY | O_CLOEXEC, calls fsync once, and
// closes it. A failed fsync (including EINTR) returns IOError and is not
// retried. The descriptor is released on every path; if both fsync and
// close fail, the fsync error is reported. Like open(2), this follows a
// symlink to a directory. syscalls must outlive the call.
Status SyncDirectory(const std::filesystem::path& dir,
                     PosixSyscalls& syscalls = DefaultPosixSyscalls());

}  // namespace kv
