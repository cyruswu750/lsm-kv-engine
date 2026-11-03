#pragma once

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/posix_file.h"

namespace kv::test {

// Deterministic in-memory filesystem for crash testing (docs/m3-design.md
// section 6). It implements the whole PosixSyscalls seam, so an engine
// running on it never touches the real filesystem.
//
// Every inode keeps volatile state (what reads observe) and durable state
// (what survives the loss of unsynced state) separately. fsync on a file
// makes its contents durable and nothing else; fsync on a directory makes
// that directory's entries durable and nothing else. Paths are absolute.
// There are no symbolic links, FIFOs, or hard links.
//
// Crash points are completed calls that change state (open that creates
// or truncates, write, ftruncate, fsync, rename, unlink, mkdir) and every
// Checkpoint. When the armed point completes, the whole state is copied
// into a snapshot, the instance becomes dead, and SimulatedCrash is
// thrown. A dead instance never changes the snapshot: every later call
// fails with EIO (close and unlock succeed without effect) and nothing
// throws, so destructors and error paths cannot alter what was captured.

// Thrown at the armed crash point. Deliberately not a std::exception, so
// generic handlers in tests cannot mistake it for an engine error.
struct SimulatedCrash {
    std::string point;
    std::uint64_t index = 0;
};

enum class CrashModel {
    kProcess,       // completed calls survive (the page cache is kept)
    kUnsyncedLoss,  // only fsynced contents and directory entries survive
};

inline const char* ModelName(CrashModel model) {
    return model == CrashModel::kProcess ? "process" : "unsynced-loss";
}

struct SimInode {
    bool dir = false;
    std::string data;
    std::string durable_data;
    std::map<std::string, std::uint64_t> entries;
    std::map<std::string, std::uint64_t> durable_entries;
};

// A whole filesystem state. Copying it is how snapshots are taken.
struct SimFs {
    static constexpr std::uint64_t kRoot = 1;

    SimFs() {
        inodes[kRoot].dir = true;
    }

    std::map<std::uint64_t, SimInode> inodes;
    std::uint64_t next_inode = kRoot + 1;
};

class CrashSimSyscalls : public PosixSyscalls {
public:
    CrashSimSyscalls() = default;
    explicit CrashSimSyscalls(SimFs fs) : fs_(std::move(fs)) {}

    // The state a new process sees after a crash with this snapshot, under
    // model. No descriptor or lock survives either model.
    static SimFs Recover(const SimFs& snapshot, CrashModel model) {
        if (model == CrashModel::kProcess) {
            return snapshot;
        }
        SimFs out;
        out.inodes.clear();
        out.next_inode = snapshot.next_inode;
        std::vector<std::uint64_t> pending = {SimFs::kRoot};
        while (!pending.empty()) {
            const std::uint64_t ino = pending.back();
            pending.pop_back();
            if (out.inodes.count(ino) != 0) {
                continue;
            }
            const SimInode& old = snapshot.inodes.at(ino);
            SimInode& now = out.inodes[ino];
            now.dir = old.dir;
            now.data = old.durable_data;
            now.durable_data = old.durable_data;
            now.entries = old.durable_entries;
            now.durable_entries = old.durable_entries;
            for (const auto& [name, child] : old.durable_entries) {
                pending.push_back(child);
            }
        }
        return out;
    }

    // ---- Crash control -------------------------------------------------------

    // Crash when the k-th crash point completes (1-based); 0 never crashes.
    void CrashAt(std::uint64_t k) {
        crash_at_ = k;
    }

    std::uint64_t points() const {
        return points_;
    }

    // One label per completed crash point, in order.
    const std::vector<std::string>& point_log() const {
        return point_log_;
    }

    bool dead() const {
        return dead_;
    }

    const SimFs& snapshot() const {
        return snapshot_;
    }

    const SimFs& state() const {
        return fs_;
    }

    // Mutation hook for the missing-sync check: an fsync of a file whose
    // name at open ends with this suffix reports success without making
    // the contents durable. Empty disables it.
    std::string skip_file_sync_suffix;

    // ---- Setup and inspection (not crash points; never throw) ------------------

    // Creates every missing directory on path, durably.
    void MakeDirs(std::string_view path) {
        std::uint64_t ino = SimFs::kRoot;
        for (const std::string& part : Split(path)) {
            SimInode& parent = fs_.inodes[ino];
            auto it = parent.entries.find(part);
            if (it == parent.entries.end()) {
                const std::uint64_t child = fs_.next_inode++;
                fs_.inodes[child].dir = true;
                parent.entries[part] = child;
                parent.durable_entries[part] = child;
                ino = child;
            } else {
                ino = it->second;
            }
        }
    }

    // Creates or replaces a regular file with bytes, durably.
    void PutFile(std::string_view path, const std::string& bytes) {
        std::uint64_t parent = 0;
        std::string name;
        std::uint64_t ino = 0;
        if (Resolve(path, parent, name, ino) != 0) {
            return;
        }
        if (ino == 0) {
            ino = fs_.next_inode++;
            SimInode& dir = fs_.inodes[parent];
            dir.entries[name] = ino;
            dir.durable_entries[name] = ino;
        }
        fs_.inodes[ino].data = bytes;
        fs_.inodes[ino].durable_data = bytes;
    }

    // Copies the volatile contents of a regular file into out; false if
    // there is no regular file at path.
    bool ReadFile(std::string_view path, std::string& out) const {
        std::uint64_t parent = 0;
        std::string name;
        std::uint64_t ino = 0;
        if (ResolveConst(path, parent, name, ino) != 0 || ino == 0 ||
            fs_.inodes.at(ino).dir) {
            return false;
        }
        out = fs_.inodes.at(ino).data;
        return true;
    }

    bool Exists(std::string_view path) const {
        std::uint64_t parent = 0;
        std::string name;
        std::uint64_t ino = 0;
        return ResolveConst(path, parent, name, ino) == 0 && ino != 0;
    }

    // Volatile entry names of a directory, sorted.
    std::vector<std::string> Names(std::string_view path) const {
        std::vector<std::string> out;
        std::uint64_t parent = 0;
        std::string name;
        std::uint64_t ino = 0;
        if (ResolveConst(path, parent, name, ino) == 0 && ino != 0) {
            for (const auto& [entry, child] : fs_.inodes.at(ino).entries) {
                out.push_back(entry);
            }
        }
        return out;
    }

    // ---- PosixSyscalls --------------------------------------------------------------

    int Open(const char* path, int flags, mode_t) override {
        if (dead_) {
            return Fail(EIO);
        }
        std::uint64_t parent = 0;
        std::string name;
        std::uint64_t ino = 0;
        if (const int err = Resolve(path, parent, name, ino); err != 0) {
            return Fail(err);
        }
        const int access = flags & O_ACCMODE;
        const bool writable = access == O_WRONLY || access == O_RDWR;
        bool changed = false;
        if (ino != 0) {
            if ((flags & O_CREAT) && (flags & O_EXCL)) {
                return Fail(EEXIST);
            }
            SimInode& node = fs_.inodes[ino];
            if (node.dir && (writable || (flags & O_TRUNC))) {
                return Fail(EISDIR);
            }
            if (!node.dir && (flags & O_DIRECTORY)) {
                return Fail(ENOTDIR);
            }
            if ((flags & O_TRUNC) && writable && !node.data.empty()) {
                node.data.clear();
                changed = true;
            }
        } else {
            if (!(flags & O_CREAT) || name.empty()) {
                return Fail(ENOENT);
            }
            if (flags & O_DIRECTORY) {
                return Fail(ENOENT);
            }
            ino = fs_.next_inode++;
            fs_.inodes[ino];
            fs_.inodes[parent].entries[name] = ino;
            changed = true;
        }
        const int fd = next_fd_++;
        fds_[fd] = {ino, 0, access != O_WRONLY, writable,
                    (flags & O_APPEND) != 0, name};
        if (changed) {
            Completed("open " + name);
        }
        return fd;
    }

    ssize_t Write(int fd, const void* buf, std::size_t count) override {
        OpenFile* f = Get(fd);
        if (f == nullptr || !f->writable || fs_.inodes[f->ino].dir) {
            return Fail(dead_ ? EIO : EBADF);
        }
        std::string& data = fs_.inodes[f->ino].data;
        if (f->append) {
            f->offset = data.size();
        }
        if (data.size() < f->offset + count) {
            data.resize(f->offset + count, '\0');
        }
        std::memcpy(data.data() + f->offset, buf, count);
        f->offset += count;
        if (count > 0) {
            Completed("write " + f->name);
        }
        return static_cast<ssize_t>(count);
    }

    ssize_t Read(int fd, void* buf, std::size_t count) override {
        OpenFile* f = Get(fd);
        if (f == nullptr || !f->readable || fs_.inodes[f->ino].dir) {
            return Fail(dead_ ? EIO : EBADF);
        }
        const std::size_t got = Copy(fs_.inodes[f->ino].data, f->offset, buf,
                                     count);
        f->offset += got;
        return static_cast<ssize_t>(got);
    }

    ssize_t Pread(int fd, void* buf, std::size_t count,
                  off_t offset) override {
        OpenFile* f = Get(fd);
        if (f == nullptr || !f->readable || fs_.inodes[f->ino].dir ||
            offset < 0) {
            return Fail(dead_ ? EIO : EBADF);
        }
        return static_cast<ssize_t>(
            Copy(fs_.inodes[f->ino].data, static_cast<std::size_t>(offset),
                 buf, count));
    }

    int Fstat(int fd, struct stat* st) override {
        OpenFile* f = Get(fd);
        if (f == nullptr) {
            return Fail(dead_ ? EIO : EBADF);
        }
        FillStat(f->ino, st);
        return 0;
    }

    int Ftruncate(int fd, off_t length) override {
        OpenFile* f = Get(fd);
        if (f == nullptr || !f->writable || length < 0) {
            return Fail(dead_ ? EIO : EINVAL);
        }
        fs_.inodes[f->ino].data.resize(static_cast<std::size_t>(length), '\0');
        Completed("ftruncate " + f->name);
        return 0;
    }

    int Fsync(int fd) override {
        OpenFile* f = Get(fd);
        if (f == nullptr) {
            return Fail(dead_ ? EIO : EBADF);
        }
        SimInode& node = fs_.inodes[f->ino];
        if (node.dir) {
            node.durable_entries = node.entries;
        } else if (skip_file_sync_suffix.empty() ||
                   !EndsWith(f->name, skip_file_sync_suffix)) {
            node.durable_data = node.data;
        }
        Completed("fsync " + f->name);
        return 0;
    }

    int Rename(const char* from, const char* to) override {
        if (dead_) {
            return Fail(EIO);
        }
        std::uint64_t from_parent = 0, to_parent = 0, ino = 0, target = 0;
        std::string from_name, to_name;
        if (const int err = Resolve(from, from_parent, from_name, ino);
            err != 0) {
            return Fail(err);
        }
        if (ino == 0) {
            return Fail(ENOENT);
        }
        if (const int err = Resolve(to, to_parent, to_name, target); err != 0) {
            return Fail(err);
        }
        if (fs_.inodes[ino].dir || (target != 0 && fs_.inodes[target].dir)) {
            return Fail(target != 0 && fs_.inodes[target].dir ? EISDIR
                                                              : EINVAL);
        }
        // Atomic in the model: one step replaces the target.
        fs_.inodes[from_parent].entries.erase(from_name);
        fs_.inodes[to_parent].entries[to_name] = ino;
        Completed("rename " + from_name + " -> " + to_name);
        return 0;
    }

    int Unlink(const char* path) override {
        if (dead_) {
            return Fail(EIO);
        }
        std::uint64_t parent = 0, ino = 0;
        std::string name;
        if (const int err = Resolve(path, parent, name, ino); err != 0) {
            return Fail(err);
        }
        if (ino == 0) {
            return Fail(ENOENT);
        }
        if (fs_.inodes[ino].dir) {
            return Fail(EISDIR);
        }
        // The inode itself is kept: durable entries may still refer to it.
        fs_.inodes[parent].entries.erase(name);
        Completed("unlink " + name);
        return 0;
    }

    int Lstat(const char* path, struct stat* st) override {
        if (dead_) {
            return Fail(EIO);
        }
        std::uint64_t parent = 0, ino = 0;
        std::string name;
        if (const int err = Resolve(path, parent, name, ino); err != 0) {
            return Fail(err);
        }
        if (ino == 0) {
            return Fail(ENOENT);
        }
        FillStat(ino, st);
        return 0;
    }

    int Mkdir(const char* path, mode_t) override {
        if (dead_) {
            return Fail(EIO);
        }
        std::uint64_t parent = 0, ino = 0;
        std::string name;
        if (const int err = Resolve(path, parent, name, ino); err != 0) {
            return Fail(err);
        }
        if (ino != 0) {
            return Fail(EEXIST);
        }
        if (name.empty()) {
            return Fail(EEXIST);  // the root
        }
        const std::uint64_t child = fs_.next_inode++;
        fs_.inodes[child].dir = true;
        fs_.inodes[parent].entries[name] = child;
        Completed("mkdir " + name);
        return 0;
    }

    int Flock(int fd, int operation) override {
        if (dead_) {
            return (operation & LOCK_UN) ? 0 : Fail(EIO);
        }
        OpenFile* f = Get(fd);
        if (f == nullptr) {
            return Fail(EBADF);
        }
        if (operation & LOCK_UN) {
            auto it = locks_.find(f->ino);
            if (it != locks_.end() && it->second == fd) {
                locks_.erase(it);
            }
            return 0;
        }
        auto it = locks_.find(f->ino);
        if (it != locks_.end() && it->second != fd) {
            return Fail(EWOULDBLOCK);
        }
        locks_[f->ino] = fd;
        return 0;
    }

    int ListDirectory(const char* path,
                      std::vector<std::string>& names) override {
        if (dead_) {
            return Fail(EIO);
        }
        std::uint64_t parent = 0, ino = 0;
        std::string name;
        if (const int err = Resolve(path, parent, name, ino); err != 0) {
            return Fail(err);
        }
        if (ino == 0) {
            return Fail(ENOENT);
        }
        if (!fs_.inodes[ino].dir) {
            return Fail(ENOTDIR);
        }
        std::vector<std::string> out;
        for (const auto& [entry, child] : fs_.inodes[ino].entries) {
            out.push_back(entry);
        }
        names = std::move(out);
        return 0;
    }

    int Close(int fd) override {
        if (dead_) {
            return 0;
        }
        auto it = fds_.find(fd);
        if (it == fds_.end()) {
            return Fail(EBADF);
        }
        auto lock = locks_.find(it->second.ino);
        if (lock != locks_.end() && lock->second == fd) {
            locks_.erase(lock);
        }
        fds_.erase(it);
        return 0;
    }

    void Checkpoint(std::string_view name) override {
        if (!dead_) {
            Completed("checkpoint " + std::string(name));
        }
    }

    std::size_t open_descriptors() const {
        return fds_.size();
    }

private:
    struct OpenFile {
        std::uint64_t ino = 0;
        std::size_t offset = 0;
        bool readable = false;
        bool writable = false;
        bool append = false;
        std::string name;  // the last path component at open
    };

    static std::vector<std::string> Split(std::string_view path) {
        std::vector<std::string> parts;
        std::size_t i = 0;
        while (i < path.size()) {
            const std::size_t j = std::min(path.find('/', i), path.size());
            if (j > i && path.substr(i, j - i) != ".") {
                parts.emplace_back(path.substr(i, j - i));
            }
            i = j + 1;
        }
        return parts;
    }

    static bool EndsWith(std::string_view s, std::string_view suffix) {
        return s.size() >= suffix.size() &&
               s.substr(s.size() - suffix.size()) == suffix;
    }

    // Finds path in the volatile namespace. On success returns 0 with the
    // parent directory, the last component, and the inode (0 if missing).
    // The root resolves to itself with an empty name.
    int ResolveConst(std::string_view path,
                     std::uint64_t& parent,
                     std::string& name,
                     std::uint64_t& ino) const {
        if (path.empty() || path.front() != '/') {
            return EINVAL;
        }
        const std::vector<std::string> parts = Split(path);
        std::uint64_t dir = SimFs::kRoot;
        for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
            if (parts[i] == "..") {
                return EINVAL;
            }
            const SimInode& node = fs_.inodes.at(dir);
            auto it = node.entries.find(parts[i]);
            if (it == node.entries.end()) {
                return ENOENT;
            }
            if (!fs_.inodes.at(it->second).dir) {
                return ENOTDIR;
            }
            dir = it->second;
        }
        if (parts.empty()) {
            parent = SimFs::kRoot;
            name.clear();
            ino = SimFs::kRoot;
            return 0;
        }
        if (parts.back() == "..") {
            return EINVAL;
        }
        parent = dir;
        name = parts.back();
        const SimInode& node = fs_.inodes.at(dir);
        auto it = node.entries.find(name);
        ino = it == node.entries.end() ? 0 : it->second;
        return 0;
    }

    int Resolve(std::string_view path,
                std::uint64_t& parent,
                std::string& name,
                std::uint64_t& ino) {
        return ResolveConst(path, parent, name, ino);
    }

    OpenFile* Get(int fd) {
        if (dead_) {
            return nullptr;
        }
        auto it = fds_.find(fd);
        return it == fds_.end() ? nullptr : &it->second;
    }

    static std::size_t Copy(const std::string& data,
                            std::size_t offset,
                            void* buf,
                            std::size_t count) {
        if (offset >= data.size()) {
            return 0;
        }
        const std::size_t got = std::min(count, data.size() - offset);
        std::memcpy(buf, data.data() + offset, got);
        return got;
    }

    void FillStat(std::uint64_t ino, struct stat* st) const {
        std::memset(st, 0, sizeof(*st));
        const SimInode& node = fs_.inodes.at(ino);
        st->st_ino = static_cast<ino_t>(ino);
        st->st_nlink = 1;
        st->st_mode = node.dir ? (S_IFDIR | 0755) : (S_IFREG | 0644);
        st->st_size = static_cast<off_t>(node.data.size());
    }

    static int Fail(int err) {
        errno = err;
        return -1;
    }

    void Completed(std::string label) {
        ++points_;
        point_log_.push_back(label);
        if (crash_at_ != 0 && points_ == crash_at_) {
            snapshot_ = fs_;
            dead_ = true;
            throw SimulatedCrash{std::move(label), points_};
        }
    }

    SimFs fs_;
    SimFs snapshot_;
    std::map<int, OpenFile> fds_;
    std::map<std::uint64_t, int> locks_;  // inode -> holding descriptor
    int next_fd_ = 1000;
    std::uint64_t crash_at_ = 0;
    std::uint64_t points_ = 0;
    std::vector<std::string> point_log_;
    bool dead_ = false;
};

}  // namespace kv::test
