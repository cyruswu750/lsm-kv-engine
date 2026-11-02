#pragma once

#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <deque>
#include <filesystem>
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
class FaultSyscalls : public PosixSyscalls {
public:
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
        }
        return ::close(fd);
    }

    std::size_t TotalCalls() const {
        return writes + reads + ftruncates + fsyncs + closes;
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
    std::vector<std::string> fsync_targets;
    std::vector<std::string> events;  // armed calls, in order

private:
    template <typename T>
    static T Pop(std::deque<T>& queue) {
        T value = queue.front();
        queue.pop_front();
        return value;
    }
};

}  // namespace kv::test
