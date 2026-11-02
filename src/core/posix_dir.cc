#include "posix_dir.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <system_error>
#include <utility>

namespace kv {

namespace {

std::string ErrnoText(int err) {
    return std::generic_category().message(err);
}

Status PathError(const std::filesystem::path& path,
                 const char* operation,
                 int err) {
    return Status::IOError(path.string() + ": " + operation + ": " +
                           ErrnoText(err));
}

}  // namespace

Status CheckSyscallPath(const std::filesystem::path& path,
                        const char* operation) {
    const auto& native = path.native();
    if (native.empty()) {
        return Status::InvalidArgument(std::string(operation) +
                                       ": empty path");
    }
    if (native.find('\0') != std::string::npos) {
        return Status::InvalidArgument(std::string(operation) +
                                       ": path contains a NUL byte");
    }
    return Status::Ok();
}

Status InspectPath(const std::filesystem::path& path, PathKind& out_kind) {
    Status s = CheckSyscallPath(path, "inspect");
    if (!s.ok()) {
        return s;
    }

    struct stat st;
    if (::lstat(path.c_str(), &st) != 0) {
        const int err = errno;
        if (err == ENOENT) {
            out_kind = PathKind::kMissing;
            return Status::Ok();
        }
        return PathError(path, "inspect", err);
    }

    if (S_ISREG(st.st_mode)) {
        out_kind = PathKind::kRegularFile;
    } else if (S_ISDIR(st.st_mode)) {
        out_kind = PathKind::kDirectory;
    } else {
        out_kind = PathKind::kOther;
    }
    return Status::Ok();
}

Status ListDirectory(const std::filesystem::path& dir,
                     std::vector<std::string>& out_names) {
    Status s = CheckSyscallPath(dir, "list directory");
    if (!s.ok()) {
        return s;
    }

    DIR* stream = ::opendir(dir.c_str());
    if (stream == nullptr) {
        return PathError(dir, "list directory", errno);
    }

    std::vector<std::string> names;
    int read_err = 0;
    for (;;) {
        // readdir signals both end-of-stream and error with nullptr; only
        // a changed errno distinguishes them.
        errno = 0;
        const dirent* entry = ::readdir(stream);
        if (entry == nullptr) {
            read_err = errno;
            break;
        }
        if (std::strcmp(entry->d_name, ".") == 0 ||
            std::strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        names.emplace_back(entry->d_name);
    }

    const int close_rc = ::closedir(stream);
    const int close_err = errno;

    if (read_err != 0) {
        return PathError(dir, "read directory", read_err);
    }
    if (close_rc != 0) {
        return PathError(dir, "close directory", close_err);
    }

    out_names = std::move(names);
    return Status::Ok();
}

Status CreateDirectory(const std::filesystem::path& dir, bool& out_created) {
    Status s = CheckSyscallPath(dir, "create directory");
    if (!s.ok()) {
        return s;
    }

    if (::mkdir(dir.c_str(), 0755) == 0) {
        out_created = true;
        return Status::Ok();
    }

    const int err = errno;
    if (err != EEXIST) {
        return PathError(dir, "create directory", err);
    }

    // EEXIST covers any kind of entry, including a dangling symlink.
    PathKind kind;
    s = InspectPath(dir, kind);
    if (!s.ok()) {
        return s;
    }
    if (kind != PathKind::kDirectory) {
        return Status::IOError(dir.string() +
                               ": create directory: exists and is not a "
                               "directory");
    }

    out_created = false;
    return Status::Ok();
}

Status RenameFile(const std::filesystem::path& from,
                  const std::filesystem::path& to) {
    Status s = CheckSyscallPath(from, "rename");
    if (!s.ok()) {
        return s;
    }
    s = CheckSyscallPath(to, "rename");
    if (!s.ok()) {
        return s;
    }

    if (::rename(from.c_str(), to.c_str()) != 0) {
        const int err = errno;
        return Status::IOError(from.string() + ": rename to " + to.string() +
                               ": " + ErrnoText(err));
    }
    return Status::Ok();
}

Status RemoveFile(const std::filesystem::path& path) {
    Status s = CheckSyscallPath(path, "remove");
    if (!s.ok()) {
        return s;
    }

    if (::unlink(path.c_str()) != 0) {
        return PathError(path, "remove", errno);
    }
    return Status::Ok();
}

Status SyncDirectory(const std::filesystem::path& dir,
                     PosixSyscalls& syscalls) {
    Status s = CheckSyscallPath(dir, "sync directory");
    if (!s.ok()) {
        return s;
    }

    const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        return PathError(dir, "open directory for sync", errno);
    }

    // Never retried, for the same reason as WritableFile::Sync.
    const int sync_rc = syscalls.Fsync(fd);
    const int sync_err = (sync_rc != 0) ? errno : 0;

    // Close exactly once on every path; Linux releases the fd even on error.
    const int close_rc = syscalls.Close(fd);
    const int close_err = (close_rc != 0) ? errno : 0;

    if (sync_rc != 0) {
        std::string message = dir.string() + ": fsync directory: " +
                              ErrnoText(sync_err);
        if (close_rc != 0) {
            message += " (close also failed: " + ErrnoText(close_err) + ")";
        }
        return Status::IOError(std::move(message));
    }
    if (close_rc != 0) {
        return PathError(dir, "close directory after sync", close_err);
    }
    return Status::Ok();
}

}  // namespace kv
