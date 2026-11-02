#pragma once

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "core/posix_file.h"

namespace kv::test {

// Per-object fault injector for the PosixSyscalls seam. While disarmed,
// every call goes straight to the real syscall and is not recorded, so a
// test can open a database normally and arm faults afterwards. While
// armed, calls are counted and consume scripted results; unscripted calls
// still reach the real syscall, so production loops run against a real
// file. The object must outlive every file or DB that uses it.
//
// Armed calls are also recorded in trace as "<op> <file name>", and can
// be failed by rules that match an operation and a target file name
// (the last path component; for rename, either the source or the
// destination). Rules are checked before the older per-call scripts.
class FaultSyscalls : public PosixSyscalls {
public:
    struct Rule {
        std::string op;    // open, write, read, pread, fstat, ftruncate,
                           // fsync, rename, unlink, close
        std::string name;  // target file name; empty matches every target
        int err = EIO;
        bool throws = false;
        std::size_t skip = 0;   // matching calls let through first
        std::size_t times = 1;  // failures before the rule is spent
        std::size_t fired = 0;  // failures injected so far
    };

    // Adds a rule and returns its index in rules.
    std::size_t FailOn(std::string op,
                       std::string name,
                       int err = EIO,
                       std::size_t skip = 0,
                       std::size_t times = 1) {
        rules.push_back({std::move(op), std::move(name), err, false, skip,
                         times, 0});
        return rules.size() - 1;
    }

    std::size_t ThrowOn(std::string op, std::string name) {
        rules.push_back({std::move(op), std::move(name), 0, true, 0, 1, 0});
        return rules.size() - 1;
    }

    int Open(const char* path, int flags, mode_t mode) override {
        if (!armed) {
            return ::open(path, flags, mode);
        }
        ++opens;
        if (Inject("open", {Name(path)})) {
            return -1;
        }
        return ::open(path, flags, mode);
    }

    ssize_t Pread(int fd, void* buf, std::size_t count,
                  off_t offset) override {
        if (!armed) {
            return ::pread(fd, buf, count, offset);
        }
        ++preads;
        pread_bytes += count;
        if (Inject("pread", {Name(FdTarget(fd))})) {
            return -1;
        }
        std::size_t request = count;
        if (!pread_script.empty()) {
            const Step step = Pop(pread_script);
            if (step.kind == Step::kError) {
                errno = step.err;
                return -1;
            }
            if (step.kind == Step::kThrow) {
                throw std::runtime_error("injected pread exception");
            }
            request = std::min(step.bytes, count);
        }
        return ::pread(fd, buf, request, offset);
    }

    int Fstat(int fd, struct stat* st) override {
        if (!armed) {
            return ::fstat(fd, st);
        }
        ++fstats;
        if (Inject("fstat", {Name(FdTarget(fd))})) {
            return -1;
        }
        return ::fstat(fd, st);
    }

    int Rename(const char* from, const char* to) override {
        if (!armed) {
            return ::rename(from, to);
        }
        ++renames;
        if (Inject("rename", {Name(from), Name(to)})) {
            return -1;
        }
        return ::rename(from, to);
    }

    int Unlink(const char* path) override {
        if (!armed) {
            return ::unlink(path);
        }
        ++unlinks;
        if (Inject("unlink", {Name(path)})) {
            return -1;
        }
        return ::unlink(path);
    }

    struct Step {
        enum Kind { kShort, kError, kThrow } kind;
        std::size_t bytes = 0;  // kShort: bytes really transferred
        int err = 0;            // kError: errno to report
    };

    static Step Short(std::size_t n) { return {Step::kShort, n, 0}; }
    static Step Error(int err) { return {Step::kError, 0, err}; }
    static Step Throw() { return {Step::kThrow, 0, 0}; }

    ssize_t Write(int fd, const void* buf, std::size_t count) override {
        if (!armed) {
            return ::write(fd, buf, count);
        }
        ++writes;
        events.push_back("write");
        if (Inject("write", {Name(FdTarget(fd))})) {
            return -1;
        }
        std::size_t request = count;
        if (!write_script.empty()) {
            const Step step = Pop(write_script);
            if (step.kind == Step::kError) {
                errno = step.err;
                return -1;
            }
            if (step.kind == Step::kThrow) {
                throw std::runtime_error("injected write exception");
            }
            request = std::min(step.bytes, count);
        }
        return ::write(fd, buf, request);
    }

    ssize_t Read(int fd, void* buf, std::size_t count) override {
        if (!armed) {
            return ::read(fd, buf, count);
        }
        ++reads;
        events.push_back("read");
        if (Inject("read", {Name(FdTarget(fd))})) {
            return -1;
        }
        std::size_t request = count;
        if (!read_script.empty()) {
            const Step step = Pop(read_script);
            if (step.kind == Step::kError) {
                errno = step.err;
                return -1;
            }
            if (step.kind == Step::kThrow) {
                throw std::runtime_error("injected read exception");
            }
            request = std::min(step.bytes, count);
        }
        return ::read(fd, buf, request);
    }

    int Ftruncate(int fd, off_t length) override {
        if (!armed) {
            return ::ftruncate(fd, length);
        }
        ++ftruncates;
        events.push_back("ftruncate");
        if (Inject("ftruncate", {Name(FdTarget(fd))})) {
            return -1;
        }
        if (!ftruncate_errnos.empty()) {
            errno = Pop(ftruncate_errnos);
            return -1;
        }
        return ::ftruncate(fd, length);
    }

    int Fsync(int fd) override {
        if (!armed) {
            return ::fsync(fd);
        }
        ++fsyncs;
        events.push_back("fsync");
        const std::string target = FdTarget(fd);
        fsync_targets.push_back(target);
        if (Inject("fsync", {Name(target)})) {
            return -1;
        }
        if (fsync_throw) {
            throw std::runtime_error("injected fsync exception");
        }
        if (fsync_errno != 0 &&
            (fsync_only_target.empty() || target == fsync_only_target)) {
            errno = fsync_errno;
            return -1;
        }
        return ::fsync(fd);
    }

    int Close(int fd) override {
        if (armed) {
            ++closes;
            events.push_back("close");
            if (Inject("close", {Name(FdTarget(fd))})) {
                // Like Linux, the descriptor is released even on error.
                const int err = errno;
                ::close(fd);
                errno = err;
                return -1;
            }
        }
        return ::close(fd);
    }

    std::size_t TotalCalls() const {
        return writes + reads + ftruncates + fsyncs + closes + opens + preads +
               fstats + renames + unlinks;
    }

    // Number of trace entries "<op> <name>".
    std::size_t Count(const std::string& op, const std::string& name) const {
        return static_cast<std::size_t>(
            std::count(trace.begin(), trace.end(), op + " " + name));
    }

    // Index of the first trace entry "<op> <name>", or trace.size().
    std::size_t IndexOf(const std::string& op, const std::string& name) const {
        return static_cast<std::size_t>(
            std::find(trace.begin(), trace.end(), op + " " + name) -
            trace.begin());
    }

    // Absolute path a descriptor refers to, as the kernel reports it.
    static std::string FdTarget(int fd) {
        std::error_code ec;
        const auto target = std::filesystem::read_symlink(
            "/proc/self/fd/" + std::to_string(fd), ec);
        return ec ? std::string() : target.string();
    }

    bool armed = false;

    std::deque<Step> write_script;
    std::deque<Step> read_script;
    std::deque<Step> pread_script;
    std::deque<int> ftruncate_errnos;

    // Armed fsyncs fail with fsync_errno (if nonzero), optionally only for
    // the descriptor whose target equals fsync_only_target.
    int fsync_errno = 0;
    std::string fsync_only_target;
    bool fsync_throw = false;

    std::size_t writes = 0;
    std::size_t reads = 0;
    std::size_t ftruncates = 0;
    std::size_t fsyncs = 0;
    std::size_t closes = 0;
    std::size_t opens = 0;
    std::size_t preads = 0;
    std::uint64_t pread_bytes = 0;  // bytes requested by armed preads
    std::size_t fstats = 0;
    std::size_t renames = 0;
    std::size_t unlinks = 0;
    std::vector<std::string> fsync_targets;
    std::vector<std::string> events;  // armed calls, in order
    std::vector<std::string> trace;   // armed calls as "<op> <name>"
    std::vector<Rule> rules;

private:
    static std::string Name(const std::string& path) {
        return std::filesystem::path(path).filename().string();
    }

    // Records the call and applies the first matching live rule. Returns
    // true (with errno set) if the call must fail.
    bool Inject(const std::string& op, std::vector<std::string> names) {
        trace.push_back(op + " " + names.front());
        if (names.size() > 1) {
            trace.back() += " -> " + names[1];
        }
        for (Rule& rule : rules) {
            if (rule.op != op || rule.fired >= rule.times) {
                continue;
            }
            const bool matches =
                rule.name.empty() ||
                std::find(names.begin(), names.end(), rule.name) !=
                    names.end();
            if (!matches) {
                continue;
            }
            if (rule.skip > 0) {
                --rule.skip;
                return false;
            }
            ++rule.fired;
            if (rule.throws) {
                throw std::runtime_error("injected " + op + " exception");
            }
            errno = rule.err;
            return true;
        }
        return false;
    }

    template <typename T>
    static T Pop(std::deque<T>& queue) {
        T value = queue.front();
        queue.pop_front();
        return value;
    }
};

}  // namespace kv::test
