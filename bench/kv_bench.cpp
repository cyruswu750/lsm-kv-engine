// Benchmark driver for docs/m4-design.md section 5. Release build without
// sanitizers only. Each invocation runs one configuration once and prints
// one JSON object; scripts/run_benchmarks.sh repeats and records them.
//
// Modes:
//   insert   fresh database, then a fixed mutation trace; every Put/Delete
//            is timed individually and includes any flush or compaction it
//            triggers. Afterwards (untimed) every key is verified against
//            the trace's model and the read-side probe counters are taken.
//   prepare  the common read setup: inserts keys with even ids into a fresh
//            database, then closes it.
//   read     opens a prepared database (the Open phase is measured and its
//            counters reported separately), warms up with untimed lookups,
//            resets the counters, then times existing-key and missing-key
//            lookups. Results are verified outside the timed calls.

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/db.h"
#include "core/db_internal.h"
#include "core/posix_file.h"

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

// ---- Parameters -------------------------------------------------------------

struct Params {
    std::string mode;
    std::string dir;
    std::uint64_t seed = 1;
    std::uint64_t ops = 10000;        // insert: mutations; prepare: keys
    std::uint64_t keyspace = 0;       // insert: distinct keys (0 = ops)
    std::uint64_t reads = 10000;      // read: lookups per kind
    std::uint64_t warmup = 2000;      // read: untimed lookups first
    std::uint32_t key_size = 16;
    std::uint32_t value_size = 100;
    std::uint64_t memtable_bytes = 256 * 1024;
    std::uint64_t trigger = 4;        // compaction run threshold; 0 = off
    std::uint32_t bloom_bits = 10;    // 0 writes filterless SSTs
    bool bypass_bloom = false;        // read: ignore filters in Get
    bool skip_sync = false;           // benchmark-only: fsync is a no-op
    std::uint32_t delete_percent = 0; // insert: share of Deletes
};

bool ParseFlag(const char* arg, const char* name, std::string& out) {
    const std::size_t n = std::strlen(name);
    if (std::strncmp(arg, name, n) == 0 && arg[n] == '=') {
        out = arg + n + 1;
        return true;
    }
    return false;
}

Params Parse(int argc, char** argv) {
    Params p;
    for (int i = 1; i < argc; ++i) {
        std::string v;
        auto num = [&](const char* name, auto& field) {
            if (ParseFlag(argv[i], name, v)) {
                field = static_cast<std::remove_reference_t<decltype(field)>>(
                    std::strtoull(v.c_str(), nullptr, 10));
                return true;
            }
            return false;
        };
        if (ParseFlag(argv[i], "--mode", p.mode) ||
            ParseFlag(argv[i], "--dir", p.dir) || num("--seed", p.seed) ||
            num("--ops", p.ops) || num("--keyspace", p.keyspace) ||
            num("--reads", p.reads) || num("--warmup", p.warmup) ||
            num("--key-size", p.key_size) ||
            num("--value-size", p.value_size) ||
            num("--memtable-bytes", p.memtable_bytes) ||
            num("--trigger", p.trigger) || num("--bloom-bits", p.bloom_bits) ||
            num("--delete-percent", p.delete_percent)) {
            continue;
        }
        if (ParseFlag(argv[i], "--bypass-bloom", v)) {
            p.bypass_bloom = v == "1";
        } else if (ParseFlag(argv[i], "--skip-sync", v)) {
            p.skip_sync = v == "1";
        } else {
            std::fprintf(stderr, "unknown argument %s\n", argv[i]);
            std::exit(2);
        }
    }
    if (p.keyspace == 0) {
        p.keyspace = p.ops;
    }
    return p;
}

// ---- Deterministic keys, values, and traces ------------------------------------

std::uint64_t SplitMix(std::uint64_t& state) {
    std::uint64_t z = (state += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

// "k" + zero-padded decimal id, key_size bytes in all.
std::string Key(std::uint64_t id, std::uint32_t key_size) {
    std::string digits = std::to_string(id);
    std::string key = "k";
    key.append(key_size - 1 - std::min<std::size_t>(digits.size(),
                                                     key_size - 1),
               '0');
    key += digits;
    return key;
}

std::string Value(std::uint64_t id, std::uint64_t version,
                  std::uint32_t size) {
    std::uint64_t state = id * 1000003 + version;
    std::string v(size, '\0');
    for (std::uint32_t i = 0; i < size; i += 8) {
        const std::uint64_t r = SplitMix(state);
        std::memcpy(v.data() + i, &r, std::min<std::uint32_t>(8, size - i));
    }
    return v;
}

struct Op {
    bool put;
    std::uint64_t id;
    std::uint64_t version;
};

// Insert trace: uniform ids over the keyspace; delete_percent Deletes.
std::vector<Op> InsertTrace(const Params& p) {
    std::uint64_t state = p.seed;
    std::vector<Op> ops;
    ops.reserve(p.ops);
    for (std::uint64_t i = 0; i < p.ops; ++i) {
        const std::uint64_t id = SplitMix(state) % p.keyspace;
        const bool del = SplitMix(state) % 100 < p.delete_percent;
        ops.push_back({!del, id, i});
    }
    return ops;
}

// ---- Benchmark-only syscall seam ----------------------------------------------

// Real syscalls, with optional fsync skipping (never used for correctness)
// and counts of the syncs and reads by the file they target.
class BenchSyscalls : public kv::PosixSyscalls {
public:
    explicit BenchSyscalls(bool skip_sync) : skip_sync_(skip_sync) {}

    int Fsync(int fd) override {
        const std::string kind = Kind(fd);
        ++(skip_sync_ ? skipped : performed)[kind];
        return skip_sync_ ? 0 : ::fsync(fd);
    }

    ssize_t Pread(int fd, void* buf, std::size_t count, off_t offset) override {
        ++preads;
        pread_bytes += count;
        return ::pread(fd, buf, count, offset);
    }

    ssize_t Read(int fd, void* buf, std::size_t count) override {
        read_bytes += count;
        return ::read(fd, buf, count);
    }

    void ResetIo() {
        preads = 0;
        pread_bytes = 0;
        read_bytes = 0;
    }

    std::map<std::string, std::uint64_t> skipped;
    std::map<std::string, std::uint64_t> performed;
    std::uint64_t preads = 0;
    std::uint64_t pread_bytes = 0;
    std::uint64_t read_bytes = 0;

private:
    // "directory", "wal", "sst", "manifest", or "other".
    static std::string Kind(int fd) {
        struct stat st;
        if (::fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
            return "directory";
        }
        char link[64];
        std::snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
        std::error_code ec;
        const std::string name = fs::read_symlink(link, ec).filename();
        if (name.rfind("wal-", 0) == 0) {
            return "wal";
        }
        if (name.rfind("sst-", 0) == 0) {
            return "sst";
        }
        if (name.rfind("MANIFEST", 0) == 0) {
            return "manifest";
        }
        return "other";
    }

    bool skip_sync_;
};

// ---- Measurement helpers ------------------------------------------------------------

struct Latencies {
    std::vector<std::uint64_t> ns;

    std::uint64_t Percentile(double q) {
        if (ns.empty()) {
            return 0;
        }
        std::sort(ns.begin(), ns.end());
        // Nearest rank.
        std::size_t rank = static_cast<std::size_t>(q * ns.size() + 0.999999);
        rank = std::clamp<std::size_t>(rank, 1, ns.size());
        return ns[rank - 1];
    }
};

std::uint64_t Nanos(Clock::time_point a, Clock::time_point b) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
}

struct DirSize {
    std::uint64_t bytes = 0;
    std::uint64_t files = 0;
    std::uint64_t sst_files = 0;
    std::uint64_t tmp_files = 0;
};

// Every regular file in the database directory (LOCK, MANIFEST, WALs, SSTs,
// and any leftover temporary or orphan file).
DirSize MeasureDir(const std::string& dir) {
    DirSize d;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (!e.is_regular_file()) {
            continue;
        }
        ++d.files;
        d.bytes += e.file_size();
        const std::string name = e.path().filename();
        d.sst_files += name.rfind("sst-", 0) == 0 &&
                       name.size() > 4 && name.substr(name.size() - 4) == ".sst";
        d.tmp_files += name.size() > 4 && name.substr(name.size() - 4) == ".tmp";
    }
    return d;
}

// ---- JSON output -------------------------------------------------------------------

class Json {
public:
    void Num(const char* key, double v) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.6g", v);
        Raw(key, buf);
    }
    void Int(const char* key, std::uint64_t v) {
        Raw(key, std::to_string(v));
    }
    void Str(const char* key, const std::string& v) {
        Raw(key, "\"" + v + "\"");
    }
    void Map(const char* key, const std::map<std::string, std::uint64_t>& m) {
        std::string s = "{";
        for (const auto& [k, v] : m) {
            s += (s.size() > 1 ? "," : "") + std::string("\"") + k +
                 "\":" + std::to_string(v);
        }
        Raw(key, s + "}");
    }
    void Raw(const char* key, const std::string& v) {
        out_ += (out_.empty() ? "{" : ",") + std::string("\"") + key +
                "\":" + v;
    }
    std::string Done() const {
        return out_ + "}";
    }

private:
    std::string out_;
};

void EmitParams(Json& j, const Params& p) {
    j.Str("mode", p.mode);
    j.Int("seed", p.seed);
    j.Int("ops", p.ops);
    j.Int("keyspace", p.keyspace);
    j.Int("reads", p.reads);
    j.Int("warmup", p.warmup);
    j.Int("key_size", p.key_size);
    j.Int("value_size", p.value_size);
    j.Int("memtable_bytes", p.memtable_bytes);
    j.Int("trigger", p.trigger);
    j.Int("bloom_bits", p.bloom_bits);
    j.Int("bypass_bloom", p.bypass_bloom);
    j.Int("skip_sync", p.skip_sync);
    j.Int("delete_percent", p.delete_percent);
}

void EmitStats(Json& j, const char* prefix, const kv::DbStats& s) {
    auto k = [&](const char* name) { return std::string(prefix) + name; };
    std::string sub = "{";
    auto add = [&](const char* name, std::uint64_t v) {
        sub += (sub.size() > 1 ? "," : "") + std::string("\"") + name +
               "\":" + std::to_string(v);
    };
    add("gets", s.gets);
    add("candidate_ssts", s.candidate_ssts);
    add("range_rejections", s.range_rejections);
    add("filter_rejections", s.filter_rejections);
    add("data_block_reads", s.data_block_reads);
    add("data_block_bytes", s.data_block_bytes);
    add("mutations", s.mutations);
    add("user_bytes", s.user_bytes);
    add("wal_bytes", s.wal_bytes);
    add("flush_sst_bytes", s.flush_sst_bytes);
    add("compaction_sst_bytes", s.compaction_sst_bytes);
    add("metadata_bytes", s.metadata_bytes);
    add("flushes", s.flushes);
    add("compactions", s.compactions);
    add("compaction_outputs", s.compaction_outputs);
    j.Raw(k("stats").c_str(), sub + "}");
}

// ops_per_s = calls / (sum of the per-call times): only time inside the
// DB calls counts. wall_ops_per_s = calls / (the loop's wall time), which
// also includes the harness's untimed work between calls (building values,
// updating the model). Percentiles use the nearest rank.
void EmitLatency(Json& j, const char* prefix, Latencies& l,
                 std::uint64_t wall_ns) {
    const std::string p(prefix);
    std::uint64_t call_ns = 0;
    for (const std::uint64_t ns : l.ns) {
        call_ns += ns;
    }
    j.Int((p + "_count").c_str(), l.ns.size());
    j.Int((p + "_call_ns").c_str(), call_ns);
    j.Int((p + "_wall_ns").c_str(), wall_ns);
    j.Num((p + "_ops_per_s").c_str(),
          call_ns == 0 ? 0 : l.ns.size() * 1e9 / call_ns);
    j.Num((p + "_wall_ops_per_s").c_str(),
          wall_ns == 0 ? 0 : l.ns.size() * 1e9 / wall_ns);
    j.Int((p + "_p50_ns").c_str(), l.Percentile(0.50));
    j.Int((p + "_p99_ns").c_str(), l.Percentile(0.99));
    j.Int((p + "_max_ns").c_str(), l.Percentile(1.0));
}

// The cost of the two clock reads around one call: the median of
// back-to-back steady_clock pairs. Every per-call time includes it.
std::uint64_t TimerOverheadNs() {
    Latencies l;
    for (int i = 0; i < 10001; ++i) {
        const Clock::time_point t0 = Clock::now();
        l.ns.push_back(Nanos(t0, Clock::now()));
    }
    return l.Percentile(0.50);
}

kv::Status OpenDb(const Params& p, BenchSyscalls& sys, bool create,
                  std::unique_ptr<kv::DB>& db) {
    kv::Options options;
    options.memtable_bytes = p.memtable_bytes;
    options.create_if_missing = create;
    kv::InternalOptions internal;
    internal.compaction_trigger = p.trigger;
    internal.bloom_bits_per_key = p.bloom_bits;
    internal.bypass_bloom_queries = p.bypass_bloom;
    return kv::DBOpener::Open(p.dir, options, internal, sys, db);
}

[[noreturn]] void Fail(const std::string& what) {
    std::printf("{\"ok\":false,\"error\":\"%s\"}\n", what.c_str());
    std::exit(1);
}

// ---- Modes -------------------------------------------------------------------

int RunInsert(const Params& p) {
    if (fs::exists(p.dir)) {
        Fail("directory exists: " + p.dir);
    }
    const std::vector<Op> trace = InsertTrace(p);  // built before timing
    std::vector<std::string> keys(p.keyspace);
    for (std::uint64_t id = 0; id < p.keyspace; ++id) {
        keys[id] = Key(id, p.key_size);
    }
    BenchSyscalls sys(p.skip_sync);
    std::unique_ptr<kv::DB> db;
    if (!OpenDb(p, sys, true, db).ok()) {
        Fail("open failed");
    }
    kv::DBOpener::ResetStats(*db);
    std::map<std::uint64_t, std::uint64_t> model;  // id -> version (untimed)
    Latencies lat;
    lat.ns.reserve(trace.size());
    std::uint64_t failures = 0;
    const Clock::time_point begin = Clock::now();
    for (const Op& op : trace) {
        const std::string value =
            op.put ? Value(op.id, op.version, p.value_size) : std::string();
        const Clock::time_point t0 = Clock::now();
        const kv::Status s =
            op.put ? db->Put(keys[op.id], value) : db->Delete(keys[op.id]);
        const Clock::time_point t1 = Clock::now();
        lat.ns.push_back(Nanos(t0, t1));
        if (!s.ok()) {
            ++failures;
            break;
        }
        if (op.put) {
            model[op.id] = op.version;
        } else {
            model.erase(op.id);
        }
    }
    const std::uint64_t total = Nanos(begin, Clock::now());
    const kv::DbStats writes = kv::DBOpener::Stats(*db);
    const std::uint64_t runs = kv::DBOpener::LiveRuns(*db);
    const std::uint64_t ssts = kv::DBOpener::LiveSsts(*db);

    // Untimed: every key in the keyspace has its model value; the lookups
    // also give the read-side probe counts for this final state.
    kv::DBOpener::ResetStats(*db);
    std::uint64_t mismatches = 0;
    for (std::uint64_t id = 0; id < p.keyspace; ++id) {
        std::string out;
        const kv::Status s = db->Get(keys[id], out);
        const auto it = model.find(id);
        if (it == model.end()) {
            mismatches += s.code != kv::Code::NotFound;
        } else {
            mismatches += !s.ok() ||
                          out != Value(id, it->second, p.value_size);
        }
    }
    const kv::DbStats reads = kv::DBOpener::Stats(*db);
    db.reset();
    const DirSize size = MeasureDir(p.dir);

    Json j;
    j.Raw("ok", failures == 0 && mismatches == 0 ? "true" : "false");
    EmitParams(j, p);
    EmitLatency(j, "insert", lat, total);
    j.Int("timer_overhead_ns", TimerOverheadNs());
    j.Int("failures", failures);
    j.Int("verify_mismatches", mismatches);
    EmitStats(j, "write_", writes);
    EmitStats(j, "verify_read_", reads);
    j.Int("live_runs", runs);
    j.Int("live_ssts", ssts);
    j.Int("db_bytes", size.bytes);
    j.Int("db_files", size.files);
    j.Int("db_sst_files", size.sst_files);
    j.Int("db_tmp_files", size.tmp_files);
    const std::uint64_t written =
        writes.wal_bytes + writes.flush_sst_bytes + writes.compaction_sst_bytes;
    j.Int("written_bytes", written);
    if (writes.user_bytes > 0) {
        j.Num("write_amplification",
              static_cast<double>(written) / writes.user_bytes);
        j.Num("metadata_per_user_byte",
              static_cast<double>(writes.metadata_bytes) / writes.user_bytes);
    } else {
        j.Raw("write_amplification", "null");  // zero denominator
    }
    j.Map("fsync_skipped", sys.skipped);
    j.Map("fsync_performed", sys.performed);
    std::printf("%s\n", j.Done().c_str());
    return failures == 0 && mismatches == 0 ? 0 : 1;
}

int RunPrepare(const Params& p) {
    if (fs::exists(p.dir)) {
        Fail("directory exists: " + p.dir);
    }
    BenchSyscalls sys(p.skip_sync);
    std::unique_ptr<kv::DB> db;
    if (!OpenDb(p, sys, true, db).ok()) {
        Fail("open failed");
    }
    // Even ids 0, 2, ..., 2 (ops - 1), in a seeded order.
    std::vector<std::uint64_t> ids(p.ops);
    for (std::uint64_t i = 0; i < p.ops; ++i) {
        ids[i] = 2 * i;
    }
    std::uint64_t state = p.seed;
    for (std::uint64_t i = p.ops; i > 1; --i) {
        std::swap(ids[i - 1], ids[SplitMix(state) % i]);
    }
    for (const std::uint64_t id : ids) {
        if (!db->Put(Key(id, p.key_size), Value(id, 0, p.value_size)).ok()) {
            Fail("put failed");
        }
    }
    const kv::DbStats s = kv::DBOpener::Stats(*db);
    const std::uint64_t runs = kv::DBOpener::LiveRuns(*db);
    const std::uint64_t ssts = kv::DBOpener::LiveSsts(*db);
    db.reset();
    Json j;
    j.Raw("ok", "true");
    EmitParams(j, p);
    EmitStats(j, "prepare_", s);
    j.Int("live_runs", runs);
    j.Int("live_ssts", ssts);
    j.Int("db_bytes", MeasureDir(p.dir).bytes);
    j.Map("fsync_skipped", sys.skipped);
    j.Map("fsync_performed", sys.performed);
    std::printf("%s\n", j.Done().c_str());
    return 0;
}

int RunRead(const Params& p) {
    BenchSyscalls sys(false);  // lookups do not sync
    std::unique_ptr<kv::DB> db;
    const Clock::time_point open0 = Clock::now();
    if (!OpenDb(p, sys, false, db).ok()) {
        Fail("open failed");
    }
    const std::uint64_t open_ns = Nanos(open0, Clock::now());
    const std::uint64_t open_pread_bytes = sys.pread_bytes;
    const std::uint64_t open_read_bytes = sys.read_bytes;
    const std::uint64_t n = p.ops;  // prepared keys: even ids below 2n

    // Traces built before timing: existing ids are even, missing ids odd,
    // so missing keys fall inside the populated key ranges.
    std::uint64_t state = p.seed * 7 + 3;
    std::vector<std::uint64_t> warm, present, missing;
    for (std::uint64_t i = 0; i < p.warmup; ++i) {
        warm.push_back(2 * (SplitMix(state) % n));
    }
    for (std::uint64_t i = 0; i < p.reads; ++i) {
        present.push_back(2 * (SplitMix(state) % n));
        missing.push_back(2 * (SplitMix(state) % (n - 1)) + 1);
    }
    std::uint64_t mismatches = 0;
    std::string out;
    for (const std::uint64_t id : warm) {
        mismatches += !db->Get(Key(id, p.key_size), out).ok();
    }

    Json j;
    EmitParams(j, p);
    j.Int("open_ns", open_ns);
    j.Int("open_pread_bytes", open_pread_bytes);
    j.Int("open_read_bytes", open_read_bytes);
    j.Int("live_runs", kv::DBOpener::LiveRuns(*db));
    j.Int("live_ssts", kv::DBOpener::LiveSsts(*db));

    for (const bool want_present : {true, false}) {
        const std::vector<std::uint64_t>& ids = want_present ? present : missing;
        std::vector<std::string> keys;
        keys.reserve(ids.size());
        for (const std::uint64_t id : ids) {
            keys.push_back(Key(id, p.key_size));
        }
        kv::DBOpener::ResetStats(*db);
        sys.ResetIo();
        Latencies lat;
        lat.ns.reserve(ids.size());
        std::vector<kv::Code> codes(ids.size());
        std::vector<std::string> values(ids.size());
        const Clock::time_point begin = Clock::now();
        for (std::size_t i = 0; i < ids.size(); ++i) {
            const Clock::time_point t0 = Clock::now();
            const kv::Status s = db->Get(keys[i], values[i]);
            lat.ns.push_back(Nanos(t0, Clock::now()));
            codes[i] = s.code;
        }
        const std::uint64_t total = Nanos(begin, Clock::now());
        const kv::DbStats s = kv::DBOpener::Stats(*db);
        // Untimed verification.
        for (std::size_t i = 0; i < ids.size(); ++i) {
            if (want_present) {
                mismatches += codes[i] != kv::Code::Ok ||
                              values[i] != Value(ids[i], 0, p.value_size);
            } else {
                mismatches += codes[i] != kv::Code::NotFound;
            }
        }
        const char* name = want_present ? "existing" : "missing";
        EmitLatency(j, name, lat, total);
        EmitStats(j, want_present ? "existing_" : "missing_", s);
        j.Int((std::string(name) + "_pread_bytes").c_str(), sys.pread_bytes);
    }
    j.Int("verify_mismatches", mismatches);
    j.Int("timer_overhead_ns", TimerOverheadNs());
    j.Raw("ok", mismatches == 0 ? "true" : "false");
    std::printf("%s\n", j.Done().c_str());
    return mismatches == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    const Params p = Parse(argc, argv);
    if (p.dir.empty()) {
        Fail("--dir is required");
    }
    if (p.mode == "insert") {
        return RunInsert(p);
    }
    if (p.mode == "prepare") {
        return RunPrepare(p);
    }
    if (p.mode == "read") {
        return RunRead(p);
    }
    Fail("unknown --mode");
}
