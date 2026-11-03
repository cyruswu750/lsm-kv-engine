#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "core/db.h"
#include "core/db_internal.h"
#include "core/posix_file.h"
#include "core/memtable.h"
#include "core/types.h"
#include "testing/manifest_builder.h"
#include "testing/temp_dir.h"

namespace kv {
namespace {

namespace fs = std::filesystem;

// Differential tests: run a deterministic random operation stream against
// the engine and an independent reference map, comparing every result.
//
// Environment variables (read when the test binary starts):
//   KV_DIFF_FIRST_SEED    first seed (default 1); the 20 registered test
//                         instances use seeds FIRST_SEED .. FIRST_SEED+19
//   KV_DIFF_OPS           operations per seed and workload (default 5000)
//   KV_DIFF_REOPEN_EVERY  reopen interval for DbPeriodicReopen (default 500)
// For more seeds, run repeatedly with different KV_DIFF_FIRST_SEED values
// (scripts/run_differential.sh). On failure the test prints the seed, the
// operation index, and the last operations, and writes the full trace up to
// the failing operation to a file in the working directory. No automatic
// trace minimization is performed.
//
// The DbFlush* workloads use a 4 KiB memtable so that flushes, SST
// lookups, and manifest publication happen throughout the run, and they
// assert from the published MANIFEST that many flushes actually occurred.
// They disable compaction, so every flushed SST stays live (the M2
// behavior). DbCompactionPeriodicReopen uses the same memtable with
// compaction triggered at 2 live SSTs, and asserts from the engine's named
// checkpoints that flushes and compactions both happened repeatedly.

constexpr int kSeedsPerRun = 20;
constexpr std::size_t kSmallMemtableBytes = 4096;

std::uint64_t EnvOr(const char* name, std::uint64_t fallback) {
    const char* text = std::getenv(name);
    if (text == nullptr || *text == '\0') {
        return fallback;
    }
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (*end != '\0') {
        ADD_FAILURE() << name << " is not a decimal integer: " << text;
        return fallback;
    }
    return value;
}

std::uint64_t FirstSeed() { return EnvOr("KV_DIFF_FIRST_SEED", 1); }
std::uint64_t OpCount() { return EnvOr("KV_DIFF_OPS", 5000); }
std::uint64_t ReopenEvery() { return EnvOr("KV_DIFF_REOPEN_EVERY", 500); }
// A deliberately loose lower bound, scaled with KV_DIFF_OPS (10 at the
// pinned 5000 operations); actual counts are recorded per seed.
std::uint64_t MinFlushesPerSeed() { return OpCount() / 500; }
// Equally loose (5 at 5000 operations).
std::uint64_t MinCompactionsPerSeed() { return OpCount() / 1000; }

// Real syscalls, counting the engine's maintenance checkpoints.
class MaintenanceCounter : public PosixSyscalls {
public:
    void Checkpoint(std::string_view name) override {
        if (name == "flush.installed") {
            ++flushes;
        } else if (name == "compaction.installed") {
            ++compactions;
        }
    }

    std::uint64_t flushes = 0;
    std::uint64_t compactions = 0;
};

struct DbWorkload {
    std::uint64_t reopen_every = 0;
    std::size_t memtable_bytes = Options{}.memtable_bytes;
    std::size_t compaction_trigger = InternalOptions{}.compaction_trigger;
    std::uint64_t min_flushes = 0;
    std::uint64_t min_compactions = 0;
    SstLimits limits;  // lowered to force multi-SST compaction output
    std::size_t value_pad = 0;  // bytes appended to every Put value
    bool alternate_filters = false;  // filters off on every other open
    std::uint32_t min_max_base_run = 0;  // required largest base run seen
};

// Logical runs in a parsed manifest: the base run counts once.
std::uint64_t Runs(const test::RawManifest& m) {
    return (m.base_run_count > 0 ? 1 : 0) +
           (m.ssts.size() - m.base_run_count);
}

// SplitMix64: fully specified, so a seed yields the same operations with
// any compiler or standard library.
class Rng {
public:
    explicit Rng(std::uint64_t seed) : state_(seed) {}

    std::uint64_t Next() {
        std::uint64_t z = (state_ += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }

    std::size_t Below(std::size_t n) {
        return static_cast<std::size_t>(Next() % n);
    }

private:
    std::uint64_t state_;
};

// Keys written by the workload, including empty and binary keys, plus two
// keys that are only ever read.
const std::vector<std::string>& WrittenKeys() {
    static const std::vector<std::string> keys = [] {
        std::vector<std::string> k = {
            "",
            std::string(1, '\0'),
            std::string("\x00\xff", 2),
            std::string("\xff", 1),
            std::string("bin\x00key\x80", 8),
            "a",
            "aa",
            "ab",
            std::string(300, 'L'),
        };
        for (int i = 0; i < 24; ++i) {
            k.push_back("key" + std::to_string(i));
        }
        return k;
    }();
    return keys;
}

const std::vector<std::string>& AllKeys() {
    static const std::vector<std::string> keys = [] {
        std::vector<std::string> k = WrittenKeys();
        k.push_back("missing-0");
        k.push_back(std::string("missing\x00-1", 10));
        return k;
    }();
    return keys;
}

struct Op {
    enum Kind { kPut, kDelete, kGet } kind;
    std::string key;
    std::string value;
};

std::string RandomValue(Rng& rng) {
    const std::size_t pick = rng.Below(100);
    std::size_t length;
    if (pick < 10) {
        length = 0;
    } else if (pick < 70) {
        length = 1 + rng.Below(16);
    } else if (pick < 95) {
        length = 17 + rng.Below(240);
    } else {
        length = 1000 + rng.Below(1000);
    }
    std::string value(length, '\0');
    for (char& c : value) {
        c = static_cast<char>(rng.Next() & 0xff);
    }
    return value;
}

// 45% Put, 20% Delete, 35% Get. Gets also target never-written keys.
Op NextOp(Rng& rng) {
    const std::size_t pick = rng.Below(100);
    if (pick < 45) {
        const std::string& key = WrittenKeys()[rng.Below(WrittenKeys().size())];
        return {Op::kPut, key, RandomValue(rng)};
    }
    if (pick < 65) {
        return {Op::kDelete, WrittenKeys()[rng.Below(WrittenKeys().size())],
                ""};
    }
    return {Op::kGet, AllKeys()[rng.Below(AllKeys().size())], ""};
}

std::string Hex(const std::string& bytes, std::size_t limit = SIZE_MAX) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (std::size_t i = 0; i < bytes.size() && i < limit; ++i) {
        const auto b = static_cast<unsigned char>(bytes[i]);
        out.push_back(digits[b >> 4]);
        out.push_back(digits[b & 0xf]);
    }
    if (bytes.size() > limit) {
        out += "...";
    }
    return out;
}

std::string Describe(const Op& op, std::size_t value_limit = SIZE_MAX) {
    std::ostringstream s;
    switch (op.kind) {
        case Op::kPut:
            s << "PUT key=" << Hex(op.key) << " value("
              << op.value.size() << ")=" << Hex(op.value, value_limit);
            break;
        case Op::kDelete:
            s << "DEL key=" << Hex(op.key);
            break;
        case Op::kGet:
            s << "GET key=" << Hex(op.key);
            break;
    }
    return s.str();
}

// Records the operation stream and produces a reproducible failure report.
class Trace {
public:
    Trace(std::string workload, std::uint64_t seed)
        : workload_(std::move(workload)), seed_(seed) {}

    void Add(std::string line) { lines_.push_back(std::move(line)); }

    std::string Fail(std::size_t index, const std::string& what) const {
        const std::string file = "differential-" + workload_ + "-seed" +
                                 std::to_string(seed_) + ".trace";
        {
            std::ofstream out(file);
            out << "workload " << workload_ << "\nseed " << seed_
                << "\nops " << OpCount() << "\nreopen_every "
                << ReopenEvery() << "\nfailing_op " << index << "\n"
                << what << "\n";
            for (const std::string& line : lines_) {
                out << line << "\n";
            }
        }
        std::ostringstream msg;
        msg << "workload=" << workload_ << " seed=" << seed_
            << " op_index=" << index << ": " << what << "\nfull trace: "
            << (fs::current_path() / file).string()
            << "\nreproduce: KV_DIFF_FIRST_SEED=" << seed_
            << " KV_DIFF_OPS=" << OpCount()
            << " KV_DIFF_REOPEN_EVERY=" << ReopenEvery()
            << " kv_tests --gtest_filter='Seeds/DifferentialTest." << workload_
            << "/0'\nlast operations:\n";
        const std::size_t first = lines_.size() > 20 ? lines_.size() - 20 : 0;
        for (std::size_t i = first; i < lines_.size(); ++i) {
            msg << "  " << lines_[i].substr(0, 200) << "\n";
        }
        return msg.str();
    }

private:
    std::string workload_;
    std::uint64_t seed_;
    std::vector<std::string> lines_;
};

std::string ShowValue(const std::optional<std::string>& v) {
    return v ? "value(" + std::to_string(v->size()) + ")=" + Hex(*v, 32)
             : "absent";
}

class DifferentialTest : public ::testing::TestWithParam<int> {
protected:
    std::uint64_t Seed() const {
        return FirstSeed() + static_cast<std::uint64_t>(GetParam());
    }

    void RunDb(const std::string& workload, const DbWorkload& w);

    test::TempDir tmp_;
};

TEST_P(DifferentialTest, Memtable) {
    Rng rng(Seed());
    Trace trace("Memtable", Seed());
    Memtable memtable;
    // nullopt marks a tombstone, distinct from a never-written key.
    std::map<std::string, std::optional<std::string>> model;
    std::uint64_t sequence = 0;

    for (std::size_t i = 0; i < OpCount(); ++i) {
        const Op op = NextOp(rng);
        trace.Add(std::to_string(i) + " " + Describe(op));
        if (op.kind == Op::kPut) {
            memtable.Apply(++sequence, ValueType::kPut, op.key, op.value);
            model[op.key] = op.value;
            continue;
        }
        if (op.kind == Op::kDelete) {
            memtable.Apply(++sequence, ValueType::kDelete, op.key, "");
            model[op.key] = std::nullopt;
            continue;
        }

        std::string out = "sentinel";
        const LookupResult got = memtable.Lookup(op.key, &out);
        const auto it = model.find(op.key);
        LookupResult want = LookupResult::kAbsent;
        if (it != model.end()) {
            want = it->second ? LookupResult::kFound : LookupResult::kDeleted;
        }
        const bool value_ok = want == LookupResult::kFound
                                  ? out == *it->second
                                  : out == "sentinel";
        if (got != want || !value_ok) {
            FAIL() << trace.Fail(i, "memtable lookup mismatch: got kind " +
                                        std::to_string(static_cast<int>(got)) +
                                        ", want " +
                                        std::to_string(static_cast<int>(want)));
        }
    }
}

void DifferentialTest::RunDb(const std::string& workload,
                             const DbWorkload& w) {
    Rng rng(Seed());
    Trace trace(workload, Seed());
    const fs::path dir = tmp_.path() / "db";
    std::map<std::string, std::string> model;
    const std::uint64_t reopen_every = w.reopen_every;
    Options options;
    options.memtable_bytes = w.memtable_bytes;
    InternalOptions internal;
    internal.compaction_trigger = w.compaction_trigger;
    internal.sst_limits = w.limits;
    MaintenanceCounter sys;
    // The largest base run seen at any reopen or at the end.
    std::uint32_t max_base_run = 0;
    auto note_base_run = [&]() {
        test::RawManifest m;
        if (test::ReadManifest(dir, m)) {
            max_base_run = std::max(max_base_run, m.base_run_count);
        }
    };

    std::unique_ptr<DB> db;
    ASSERT_TRUE(DBOpener::Open(dir, options, internal, sys, db).ok());
    std::uint64_t reopens = 0;

    // Compares every known key, including deleted and never-written ones.
    auto compare_all = [&](std::size_t index, const char* when) {
        for (const std::string& key : AllKeys()) {
            std::string out = "stale";
            const Status s = db->Get(key, out);
            const auto it = model.find(key);
            std::optional<std::string> got;
            if (s.ok()) {
                got = out;
            } else if (s.code != Code::NotFound || !out.empty()) {
                return trace.Fail(index, std::string(when) + ": Get(" +
                                             Hex(key) + ") returned " +
                                             s.ToString());
            }
            const std::optional<std::string> want =
                it == model.end() ? std::nullopt
                                  : std::optional<std::string>(it->second);
            if (got != want) {
                return trace.Fail(index, std::string(when) + ": key " +
                                             Hex(key) + " is " +
                                             ShowValue(got) + ", want " +
                                             ShowValue(want));
            }
        }
        return std::string();
    };

    auto reopen = [&](std::size_t index) {
        trace.Add("REOPEN after op " + std::to_string(index));
        ++reopens;
        db.reset();
        if (w.alternate_filters) {
            // Odd opens write filterless version 1 SSTs.
            internal.bloom_bits_per_key =
                reopens % 2 == 1 ? 0 : kDefaultBloomBitsPerKey;
        }
        const Status s = DBOpener::Open(dir, options, internal, sys, db);
        if (!s.ok()) {
            return trace.Fail(index, "reopen failed: " + s.ToString());
        }
        note_base_run();
        return compare_all(index, "after reopen");
    };

    for (std::size_t i = 0; i < OpCount(); ++i) {
        Op op = NextOp(rng);
        if (op.kind == Op::kPut && w.value_pad > 0) {
            op.value.append(w.value_pad, 'p');
        }
        trace.Add(std::to_string(i) + " " + Describe(op));

        if (op.kind == Op::kPut || op.kind == Op::kDelete) {
            const Status s = op.kind == Op::kPut ? db->Put(op.key, op.value)
                                                 : db->Delete(op.key);
            if (!s.ok()) {
                FAIL() << trace.Fail(i, "mutation failed: " + s.ToString());
            }
            if (op.kind == Op::kPut) {
                model[op.key] = op.value;
            } else {
                model.erase(op.key);
            }
        } else {
            std::string out = "stale";
            const Status s = db->Get(op.key, out);
            const auto it = model.find(op.key);
            const bool ok = it == model.end()
                                ? s.code == Code::NotFound && out.empty()
                                : s.ok() && out == it->second;
            if (!ok) {
                FAIL() << trace.Fail(
                    i, "Get returned " + s.ToString() + " " + ShowValue(out) +
                           ", want " +
                           ShowValue(it == model.end()
                                         ? std::nullopt
                                         : std::optional<std::string>(
                                               it->second)));
            }
        }

        if (reopen_every != 0 && (i + 1) % reopen_every == 0) {
            const std::string failure = reopen(i);
            if (!failure.empty()) {
                FAIL() << failure;
            }
        }
    }

    // A final reopen checks the complete log for both DB workloads.
    const std::string failure = reopen(OpCount());
    if (!failure.empty()) {
        FAIL() << failure;
    }

    // Evidence that the configured workload actually ran.
    const std::uint64_t periodic =
        reopen_every == 0 ? 0 : OpCount() / reopen_every;
    EXPECT_EQ(reopens, periodic + 1);

    // Evidence of maintenance. The engine reports each installed flush and
    // compaction through a named checkpoint; each also publishes exactly
    // one manifest generation, read here with the test-side parser.
    test::RawManifest manifest;
    ASSERT_TRUE(test::ReadManifest(dir, manifest));
    const std::uint64_t flushes = sys.flushes;
    const std::uint64_t compactions = sys.compactions;
    EXPECT_EQ(manifest.generation - 1, flushes + compactions);
    std::uint64_t sst_files = 0;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() == ".sst") {
            ++sst_files;
        }
    }
    // Cleanup removes every compacted input, so the live SSTs are the only
    // SST files.
    EXPECT_EQ(sst_files, manifest.ssts.size());
    if (w.compaction_trigger == 0) {
        // Without compaction every flushed SST stays live.
        EXPECT_EQ(compactions, 0u);
        EXPECT_EQ(manifest.ssts.size(), flushes);
    } else {
        // A mutation never ends with trigger or more logical runs.
        EXPECT_LT(Runs(manifest), w.compaction_trigger);
        // Each compaction needs at least one new flush first: an unchanged
        // base run, however many SSTs it has, is never compacted again.
        EXPECT_LE(compactions, flushes);
    }
    note_base_run();
    EXPECT_GE(max_base_run, w.min_max_base_run);
    // SST versions among the live files: 1 (no filter) and 2 (filter).
    std::uint64_t versions[3] = {0, 0, 0};
    for (const auto& sst : manifest.ssts) {
        std::string bytes;
        ASSERT_TRUE(test::ReadBytes(dir / test::RawSstName(sst.file_id),
                                    bytes));
        const std::uint64_t v = test::ReadLE(bytes, bytes.size() - 12, 4);
        ASSERT_TRUE(v == 1 || v == 2) << v;
        ++versions[v];
    }
    if (w.alternate_filters) {
        EXPECT_GT(versions[1], 0u);
        EXPECT_GT(versions[2], 0u);
    }
    RecordProperty("live_v1_ssts", std::to_string(versions[1]));
    RecordProperty("live_v2_ssts", std::to_string(versions[2]));
    EXPECT_GE(flushes, w.min_flushes) << "workload=" << workload
                                      << " seed=" << Seed();
    EXPECT_GE(compactions, w.min_compactions) << "workload=" << workload
                                              << " seed=" << Seed();
    RecordProperty("seed", std::to_string(Seed()));
    RecordProperty("ops", std::to_string(OpCount()));
    RecordProperty("reopens", std::to_string(reopens));
    RecordProperty("flushes", std::to_string(flushes));
    RecordProperty("compactions", std::to_string(compactions));
    RecordProperty("compaction_trigger", std::to_string(w.compaction_trigger));
    RecordProperty("max_base_run", std::to_string(max_base_run));
    RecordProperty("final_base_run", std::to_string(manifest.base_run_count));
    // Where the data lived, so a run can show which filesystem it used.
    RecordProperty("db_dir", dir.string());
}

TEST_P(DifferentialTest, DbWithoutReopen) {
    RunDb("DbWithoutReopen", {});
}

TEST_P(DifferentialTest, DbPeriodicReopen) {
    DbWorkload w;
    w.reopen_every = ReopenEvery();
    RunDb("DbPeriodicReopen", w);
}

DbWorkload FlushWorkload(std::uint64_t reopen_every) {
    DbWorkload w;
    w.reopen_every = reopen_every;
    w.memtable_bytes = kSmallMemtableBytes;
    w.compaction_trigger = 0;
    w.min_flushes = MinFlushesPerSeed();
    return w;
}

TEST_P(DifferentialTest, DbFlushWithoutReopen) {
    RunDb("DbFlushWithoutReopen", FlushWorkload(0));
}

TEST_P(DifferentialTest, DbFlushPeriodicReopen) {
    RunDb("DbFlushPeriodicReopen", FlushWorkload(ReopenEvery()));
}

// The M3 workload: 4 KiB memtable, compaction at 2 live SSTs, reopen every
// KV_DIFF_REOPEN_EVERY operations.
TEST_P(DifferentialTest, DbCompactionPeriodicReopen) {
    DbWorkload w;
    w.reopen_every = ReopenEvery();
    w.memtable_bytes = kSmallMemtableBytes;
    w.compaction_trigger = 2;
    w.min_flushes = MinFlushesPerSeed();
    w.min_compactions = MinCompactionsPerSeed();
    RunDb("DbCompactionPeriodicReopen", w);
}

// Filters alternate between opens with compaction disabled, so filterless
// version 1 and filter-bearing version 2 SSTs are live together.
TEST_P(DifferentialTest, DbMixedFilterPeriodicReopen) {
    DbWorkload w = FlushWorkload(ReopenEvery());
    w.alternate_filters = true;
    RunDb("DbMixedFilterPeriodicReopen", w);
}

// The same with at most three blocks per output SST and every Put value
// padded by 600 bytes. A flush (under 4 KiB plus one entry) always fits in
// three blocks, since any two consecutive blocks hold more than 4 KiB. The
// live data (about 33 keys of several hundred bytes) needs more, so
// compactions split their output into a multi-SST base run, and reopens
// must keep the grouping.
TEST_P(DifferentialTest, DbMultiOutputCompactionPeriodicReopen) {
    DbWorkload w;
    w.reopen_every = ReopenEvery();
    w.memtable_bytes = kSmallMemtableBytes;
    w.compaction_trigger = 2;
    w.limits = SstLimits{3, kMaxSstIndexSize};
    w.value_pad = 600;
    w.min_flushes = MinFlushesPerSeed();
    w.min_compactions = MinCompactionsPerSeed();
    w.min_max_base_run = 2;
    RunDb("DbMultiOutputCompactionPeriodicReopen", w);
}

INSTANTIATE_TEST_SUITE_P(Seeds, DifferentialTest,
                         ::testing::Range(0, kSeedsPerRun));

// Passing runs never print a report, so check its contents directly.
TEST(DifferentialTrace, FailureReportIdentifiesSeedIndexAndTrace) {
    Trace trace("SelfCheck", 4242);
    Rng rng(4242);
    std::vector<std::string> lines;
    for (int i = 0; i < 30; ++i) {
        lines.push_back(std::to_string(i) + " " + Describe(NextOp(rng)));
        trace.Add(lines.back());
    }
    const std::string report = trace.Fail(29, "example mismatch");
    EXPECT_NE(report.find("seed=4242"), std::string::npos) << report;
    EXPECT_NE(report.find("op_index=29"), std::string::npos) << report;
    EXPECT_NE(report.find("KV_DIFF_FIRST_SEED=4242"), std::string::npos);
    EXPECT_NE(report.find(lines.back().substr(0, 40)), std::string::npos);

    const fs::path file = "differential-SelfCheck-seed4242.trace";
    std::string contents;
    ASSERT_TRUE(test::ReadBytes(file, contents));
    fs::remove(file);
    EXPECT_NE(contents.find("failing_op 29"), std::string::npos);
    for (const std::string& line : lines) {
        EXPECT_NE(contents.find(line + "\n"), std::string::npos) << line;
    }

    // The same seed reproduces the same operations.
    Rng again(4242);
    EXPECT_EQ("0 " + Describe(NextOp(again)), lines.front());
}

}  // namespace
}  // namespace kv
