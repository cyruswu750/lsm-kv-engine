#include <gtest/gtest.h>

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
#include <vector>

#include "core/db.h"
#include "core/memtable.h"
#include "core/types.h"
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

constexpr int kSeedsPerRun = 20;

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

    void RunDb(const std::string& workload, std::uint64_t reopen_every);

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
                             std::uint64_t reopen_every) {
    Rng rng(Seed());
    Trace trace(workload, Seed());
    const fs::path dir = tmp_.path() / "db";
    std::map<std::string, std::string> model;

    std::unique_ptr<DB> db;
    ASSERT_TRUE(DB::Open(dir, Options{}, db).ok());
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
        const Status s = DB::Open(dir, Options{}, db);
        if (!s.ok()) {
            return trace.Fail(index, "reopen failed: " + s.ToString());
        }
        return compare_all(index, "after reopen");
    };

    for (std::size_t i = 0; i < OpCount(); ++i) {
        const Op op = NextOp(rng);
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
    RecordProperty("seed", std::to_string(Seed()));
    RecordProperty("ops", std::to_string(OpCount()));
    RecordProperty("reopens", std::to_string(reopens));
}

TEST_P(DifferentialTest, DbWithoutReopen) {
    RunDb("DbWithoutReopen", 0);
}

TEST_P(DifferentialTest, DbPeriodicReopen) {
    RunDb("DbPeriodicReopen", ReopenEvery());
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
