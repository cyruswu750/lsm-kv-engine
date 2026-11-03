#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "core/db.h"
#include "core/db_internal.h"
#include "testing/crash_sim.h"
#include "testing/wal_builder.h"

namespace kv::test {
namespace {

// Crash enumeration (docs/m3-design.md sections 7 and 8). For each fixed
// workload: run it once without a crash to count its N crash points, then
// for every k in 1..N restore the identical initial state, crash at point
// k, and check recovery under both models, followed by continued use.
//
// The database lives at a path that does not exist on the real
// filesystem, so any access that bypassed the seam would fail the run.

constexpr const char* kRoot = "/kvsim-crash-matrix";
constexpr const char* kDb = "/kvsim-crash-matrix/db";

struct Step {
    enum Kind { kOpen, kPut, kDelete, kClose } kind;
    std::string key;
    std::string value;
};

Step OpenStep() { return {Step::kOpen, "", ""}; }
Step Put(std::string k, std::string v) { return {Step::kPut, k, v}; }
Step Del(std::string k) { return {Step::kDelete, k, ""}; }
Step CloseStep() { return {Step::kClose, "", ""}; }

struct Workload {
    std::string name;
    std::size_t memtable_bytes = 1 << 20;
    std::size_t compaction_trigger = 0;
    SstLimits limits;         // lowered to force multi-SST compaction output
    std::vector<Step> setup;  // builds the initial state; never crashes
    std::function<void(CrashSimSyscalls&)> tamper;  // durable edits after setup
    std::vector<Step> steps;  // enumerated
};

struct Mutation {
    bool put = true;
    std::string key;
    std::string value;
};

using Model = std::map<std::string, std::string>;

void Apply(Model& model, const Mutation& m) {
    if (m.put) {
        model[m.key] = m.value;
    } else {
        model.erase(m.key);
    }
}

// What the driver observed before the crash.
struct Progress {
    bool initialized = false;  // some Open returned Ok
    std::vector<Mutation> acked;
    std::optional<Mutation> in_flight;
    std::string error;  // an unexpected engine error (not a crash)
};

Status OpenDb(CrashSimSyscalls& sim,
              const Workload& w,
              bool create,
              std::unique_ptr<DB>& db) {
    Options options;
    options.memtable_bytes = w.memtable_bytes;
    options.create_if_missing = create;
    InternalOptions internal;
    internal.compaction_trigger = w.compaction_trigger;
    internal.sst_limits = w.limits;
    return DBOpener::Open(kDb, options, internal, sim, db);
}

// Runs steps on sim, recording responses. A crash propagates as
// SimulatedCrash; the DB is destroyed during unwinding against the dead
// instance, which cannot change the snapshot.
void RunSteps(CrashSimSyscalls& sim,
              const Workload& w,
              const std::vector<Step>& steps,
              Progress& p) {
    std::unique_ptr<DB> db;
    for (const Step& step : steps) {
        Status s;
        switch (step.kind) {
        case Step::kOpen:
            db.reset();
            s = OpenDb(sim, w, /*create=*/true, db);
            if (s.ok()) {
                p.initialized = true;
            }
            break;
        case Step::kPut:
        case Step::kDelete: {
            const Mutation m{step.kind == Step::kPut, step.key, step.value};
            p.in_flight = m;
            s = m.put ? db->Put(m.key, m.value) : db->Delete(m.key);
            if (s.ok()) {
                sim.Checkpoint("response.before");
                p.acked.push_back(m);
                p.in_flight.reset();
                sim.Checkpoint("response.after");
            }
            break;
        }
        case Step::kClose:
            db.reset();
            break;
        }
        if (!s.ok()) {
            p.error = s.ToString();
            return;
        }
    }
}

// Every key a mutation step names, including the empty key (a valid key),
// plus the key written after recovery and one that is never written.
std::vector<std::string> KeysOf(const Workload& w) {
    std::set<std::string> keys = {"never-written", "post"};
    for (const auto* list : {&w.setup, &w.steps}) {
        for (const Step& s : *list) {
            if (s.kind == Step::kPut || s.kind == Step::kDelete) {
                keys.insert(s.key);
            }
        }
    }
    return {keys.begin(), keys.end()};
}

// Expected Open result for a directory that no Open has initialized yet,
// decided from the directory contents alone with the docs/sst-format.md
// section 8 table. It deliberately does not call the engine's classifier.
// Before initialization completes, the workloads can leave only these
// names; anything else is unexpected.
enum class InitExpect {
    kInitialize,  // rows 2 and 9: missing, empty, or LOCK only
    kResidue,     // row 7: engine files without MANIFEST, preserved
    kDatabase,    // row 5: MANIFEST was published before the crash
    kUnexpected,
};

struct DirState {
    bool exists = false;
    std::map<std::string, std::string> files;  // name -> contents
};

DirState ReadDir(const CrashSimSyscalls& sim) {
    DirState d;
    d.exists = sim.Exists(kDb);
    for (const std::string& name : sim.Names(kDb)) {
        std::string bytes;
        d.files[name] = sim.ReadFile(std::string(kDb) + "/" + name, bytes)
                            ? bytes
                            : "<not a regular file>";
    }
    return d;
}

InitExpect ExpectedInitOutcome(const DirState& d) {
    if (!d.exists) {
        return InitExpect::kInitialize;
    }
    if (d.files.count("MANIFEST") != 0) {
        return InitExpect::kDatabase;
    }
    bool residue = false;
    for (const auto& [name, bytes] : d.files) {
        if (bytes == "<not a regular file>") {
            return InitExpect::kUnexpected;
        }
        if (name == "LOCK") {
            continue;
        }
        if (name == "wal-000001.log.tmp" || name == "wal-000001.log" ||
            name == "MANIFEST.tmp") {
            residue = true;
            continue;
        }
        return InitExpect::kUnexpected;
    }
    return residue ? InitExpect::kResidue : InitExpect::kInitialize;
}

// Judges the first Open after a crash during initialization. Returns "" if
// the result is the one the table allows.
std::string JudgeInitOpen(InitExpect expect,
                          const Status& s,
                          bool directory_unchanged) {
    switch (expect) {
    case InitExpect::kInitialize:
    case InitExpect::kDatabase:
        return s.ok() ? "" : "expected a usable database, got " + s.ToString();
    case InitExpect::kResidue:
        if (s.code != Code::Corruption) {
            return "expected Corruption for initialization residue, got " +
                   s.ToString();
        }
        return directory_unchanged
                   ? ""
                   : "Open changed a directory holding residue";
    case InitExpect::kUnexpected:
        break;
    }
    return "unexpected directory contents after an initialization crash";
}

// Reads every key; returns "" or a description of the first failure.
std::string ReadAll(const DB& db,
                    const std::vector<std::string>& keys,
                    std::map<std::string, std::optional<std::string>>& out) {
    for (const std::string& key : keys) {
        std::string value;
        const Status s = db.Get(key, value);
        if (s.ok()) {
            out[key] = value;
        } else if (s.code == Code::NotFound) {
            out[key] = std::nullopt;
        } else {
            return "Get(" + key + "): " + s.ToString();
        }
    }
    return "";
}

bool Matches(const std::map<std::string, std::optional<std::string>>& got,
             const Model& model) {
    for (const auto& [key, value] : got) {
        const auto it = model.find(key);
        const std::optional<std::string> want =
            it == model.end() ? std::nullopt
                              : std::optional<std::string>(it->second);
        if (value != want) {
            return false;
        }
    }
    return true;
}

// The recovery oracle for one crash and one model. Returns "" on success.
std::string CheckRecovery(const Workload& w,
                          const Progress& p,
                          const SimFs& snapshot,
                          CrashModel model,
                          std::string* outcome) {
    CrashSimSyscalls after(CrashSimSyscalls::Recover(snapshot, model));
    std::unique_ptr<DB> db;
    std::string row;  // the table row an initialization crash fell into
    if (!p.initialized) {
        // No Open has returned, so nothing was acknowledged. The allowed
        // result follows from what the crash left in the directory.
        if (!p.acked.empty()) {
            return "acknowledged mutations before any successful Open";
        }
        const DirState before = ReadDir(after);
        const InitExpect expect = ExpectedInitOutcome(before);
        const Status s = OpenDb(after, w, /*create=*/true, db);
        const std::string err = JudgeInitOpen(
            expect, s, s.ok() || ReadDir(after).files == before.files);
        if (!err.empty()) {
            return err;
        }
        if (expect == InitExpect::kResidue) {
            *outcome = "init-residue";
            return "";
        }
        row = expect == InitExpect::kDatabase ? "init-published "
                                              : "init-fresh ";
    } else {
        const Status s = OpenDb(after, w, /*create=*/false, db);
        if (!s.ok()) {
            return "reopen failed: " + s.ToString();
        }
    }
    Status s;

    // All acknowledged mutations, and optionally the in-flight one.
    Model without;
    for (const Mutation& m : p.acked) {
        Apply(without, m);
    }
    Model with = without;
    if (p.in_flight) {
        Apply(with, *p.in_flight);
    }
    const std::vector<std::string> keys = KeysOf(w);
    std::map<std::string, std::optional<std::string>> got;
    std::string err = ReadAll(*db, keys, got);
    if (!err.empty()) {
        return err;
    }
    Model chosen;
    if (Matches(got, without)) {
        chosen = without;
        *outcome = row + "acknowledged";
    } else if (p.in_flight && Matches(got, with)) {
        chosen = with;
        *outcome = row + "with-in-flight";
    } else {
        return "recovered state is neither the acknowledged prefix nor it "
               "plus the in-flight mutation";
    }

    // Continued use: write, delete, close, reopen, and check again.
    for (const Mutation& m : {Mutation{true, "post", "p"},
                              Mutation{false, keys.front(), ""}}) {
        s = m.put ? db->Put(m.key, m.value) : db->Delete(m.key);
        if (!s.ok()) {
            return "write after recovery failed: " + s.ToString();
        }
        Apply(chosen, m);
    }
    db.reset();
    s = OpenDb(after, w, /*create=*/false, db);
    if (!s.ok()) {
        return "second reopen failed: " + s.ToString();
    }
    got.clear();
    err = ReadAll(*db, keys, got);
    if (!err.empty()) {
        return err;
    }
    if (!Matches(got, chosen)) {
        return "state after continued use differs from the model";
    }
    return "";
}

struct Prepared {
    SimFs initial;
    Progress base;
};

Prepared Prepare(const Workload& w) {
    CrashSimSyscalls sim;
    sim.MakeDirs(kRoot);
    Prepared out;
    RunSteps(sim, w, w.setup, out.base);
    EXPECT_EQ(out.base.error, "") << w.name << " setup";
    if (w.tamper) {
        w.tamper(sim);
    }
    out.initial = sim.state();
    return out;
}

struct MatrixResult {
    std::uint64_t points = 0;
    std::vector<std::string> labels;
    std::map<std::string, std::uint64_t> outcomes;  // "<model> <outcome>"
    std::vector<std::string> violations;            // "<k> <model> <label>: why"
};

MatrixResult RunMatrix(const Workload& w,
                       const std::string& skip_file_sync_suffix = "") {
    const Prepared prepared = Prepare(w);
    MatrixResult result;
    {
        CrashSimSyscalls sim(prepared.initial);
        sim.skip_file_sync_suffix = skip_file_sync_suffix;
        Progress p = prepared.base;
        RunSteps(sim, w, w.steps, p);
        EXPECT_EQ(p.error, "") << w.name << " uncrashed run";
        result.points = sim.points();
        result.labels = sim.point_log();
    }
    for (std::uint64_t k = 1; k <= result.points; ++k) {
        CrashSimSyscalls sim(prepared.initial);
        sim.skip_file_sync_suffix = skip_file_sync_suffix;
        sim.CrashAt(k);
        Progress p = prepared.base;
        std::string label;
        try {
            RunSteps(sim, w, w.steps, p);
        } catch (const SimulatedCrash& crash) {
            label = crash.point;
        }
        if (!sim.dead()) {
            result.violations.push_back(std::to_string(k) + ": no crash");
            continue;
        }
        if (!p.error.empty()) {
            result.violations.push_back(std::to_string(k) + ": " + p.error);
            continue;
        }
        for (const CrashModel model :
             {CrashModel::kProcess, CrashModel::kUnsyncedLoss}) {
            std::string outcome;
            const std::string err =
                CheckRecovery(w, p, sim.snapshot(), model, &outcome);
            if (err.empty()) {
                ++result.outcomes[std::string(ModelName(model)) + " " +
                                  outcome];
            } else {
                result.violations.push_back(std::to_string(k) + " " +
                                            ModelName(model) + " " + label +
                                            ": " + err);
            }
        }
    }
    return result;
}

std::string Join(const std::vector<std::string>& lines) {
    std::string out;
    for (const std::string& line : lines) {
        out += line + "\n";
    }
    return out;
}

bool HasLabel(const MatrixResult& r, const std::string& label) {
    for (const std::string& l : r.labels) {
        if (l == label) {
            return true;
        }
    }
    return false;
}

// ---- Workloads -----------------------------------------------------------------

Workload InitWorkload() {
    Workload w;
    w.name = "Init";
    w.steps = {OpenStep(), Put("a", "1"), Put("b", "2"), Del("a"), CloseStep()};
    return w;
}

Workload WalAppendWorkload() {
    Workload w;
    w.name = "WalAppend";
    w.setup = {OpenStep(), Put("a", "1"), CloseStep()};
    w.steps = {OpenStep(),      Put("b", "2"), Put("a", "3"),
               Del("b"),        Put("c", ""),  CloseStep()};
    return w;
}

Workload FlushWorkload() {
    Workload w;
    w.name = "Flush";
    w.memtable_bytes = 1;  // flush before every mutation after the first
    w.compaction_trigger = 0;
    w.setup = {OpenStep(), Put("a", "1"), CloseStep()};
    w.steps = {OpenStep(), Put("b", "2"), Put("a", "3"), Del("b"),
               CloseStep()};
    return w;
}

Workload CompactionWorkload() {
    Workload w;
    w.name = "Compaction";
    w.memtable_bytes = 1;
    w.compaction_trigger = 2;
    w.setup = {OpenStep(), Put("a", "1"), Put("b", "2"), CloseStep()};
    // Each mutation flushes and then compacts; deleting every key ends in
    // an empty compaction output, and the last Put writes again.
    w.steps = {OpenStep(), Put("c", "3"), Del("a"), Del("b"), Del("c"),
               Put("d", "4"), CloseStep()};
    return w;
}

Workload TailRepairWorkload() {
    Workload w;
    w.name = "TailRepair";
    w.memtable_bytes = 1;
    w.setup = {OpenStep(), Put("a", "1"), Put("b", "2"), CloseStep()};
    // A torn record at the end of the active WAL, an obsolete WAL, and a
    // stale MANIFEST.tmp, as an interrupted earlier session could leave.
    w.tamper = [](CrashSimSyscalls& sim) {
        const std::string wal = std::string(kDb) + "/wal-000003.log";
        std::string bytes;
        ASSERT_TRUE(sim.ReadFile(wal, bytes));
        sim.PutFile(wal, bytes + RawRecord(3, kRawPut, "t", "torn").substr(0, 12));
        sim.PutFile(std::string(kDb) + "/wal-000001.log", WalFileHeaderBytes());
        sim.PutFile(std::string(kDb) + "/MANIFEST.tmp", "stale");
    };
    w.steps = {OpenStep(), Put("c", "3"), Put("d", "4"), CloseStep()};
    return w;
}

// The empty key through overwrite, delete, flush, and compaction.
Workload EmptyKeyWorkload() {
    Workload w;
    w.name = "EmptyKey";
    w.memtable_bytes = 1;
    w.compaction_trigger = 2;
    w.setup = {OpenStep(), Put("", "e0"), CloseStep()};
    w.steps = {OpenStep(), Put("", "e1"), Put("a", "1"), Del(""),
               Put("", ""),  Del("a"),      CloseStep()};
    return w;
}

// Compaction into several one-block output SSTs (a multi-SST base run),
// then flushes above it, an empty key, a delete, and an overwrite.
Workload MultiOutputWorkload() {
    const std::string big(3000, 'v');  // two never share a block
    Workload w;
    w.name = "MultiOutput";
    w.memtable_bytes = 1;
    w.compaction_trigger = 2;
    w.limits = SstLimits{1, kMaxSstIndexSize};
    w.setup = {OpenStep(), Put("a", big), Put("b", big), CloseStep()};
    w.steps = {OpenStep(), Put("c", big), Put("", "e"), Del("a"),
               Put("b", "b2"), CloseStep()};
    return w;
}

class CrashMatrixTest : public ::testing::TestWithParam<Workload> {};

TEST_P(CrashMatrixTest, EveryCrashPointRecoversUnderBothModels) {
    const Workload& w = GetParam();
    const MatrixResult r = RunMatrix(w);
    ASSERT_GT(r.points, 0u);
    EXPECT_TRUE(r.violations.empty()) << Join(r.violations);
    std::uint64_t checked = 0;
    for (const auto& [key, count] : r.outcomes) {
        checked += count;
        RecordProperty(key, std::to_string(count));
    }
    EXPECT_EQ(checked, 2 * r.points);
    // Nothing reached the real filesystem.
    EXPECT_FALSE(std::filesystem::exists(kRoot));
    RecordProperty("crash_points", std::to_string(r.points));
    RecordProperty("recoveries_checked", std::to_string(checked));

    // The enumeration includes the named non-syscall boundaries.
    for (const char* label : {"checkpoint write.applied",
                              "checkpoint response.before",
                              "checkpoint response.after"}) {
        EXPECT_TRUE(HasLabel(r, label)) << label;
    }
    if (w.name == "Init") {
        EXPECT_TRUE(HasLabel(r, "mkdir db"));
        EXPECT_TRUE(HasLabel(r, "rename MANIFEST.tmp -> MANIFEST"));
        // Every table row reachable by an initialization crash occurs.
        for (const char* row : {"init-residue", "init-fresh acknowledged",
                                "init-published acknowledged"}) {
            EXPECT_GT(r.outcomes.count(std::string("process ") + row) +
                          r.outcomes.count(std::string("unsynced-loss ") + row),
                      0u)
                << row;
        }
    }
    if (w.name == "Flush") {
        EXPECT_TRUE(HasLabel(r, "checkpoint flush.installed"));
        EXPECT_TRUE(HasLabel(r, "fsync sst-000004.sst.tmp"));
        EXPECT_TRUE(HasLabel(r, "unlink wal-000003.log"));
    }
    if (w.name == "Compaction") {
        EXPECT_TRUE(HasLabel(r, "checkpoint compaction.installed"));
        EXPECT_TRUE(HasLabel(r, "unlink sst-000002.sst"));
    }
    if (w.name == "MultiOutput") {
        // The second output of the first compaction is enumerated too.
        EXPECT_TRUE(HasLabel(r, "fsync sst-000007.sst.tmp"));
        EXPECT_TRUE(HasLabel(r, "rename sst-000007.sst.tmp -> sst-000007.sst"));
    }
    if (w.name == "TailRepair") {
        EXPECT_TRUE(HasLabel(r, "ftruncate wal-000003.log"));
        EXPECT_TRUE(HasLabel(r, "unlink wal-000001.log"));
        EXPECT_TRUE(HasLabel(r, "unlink MANIFEST.tmp"));
    }
}

INSTANTIATE_TEST_SUITE_P(Workloads, CrashMatrixTest,
                         ::testing::Values(InitWorkload(), WalAppendWorkload(),
                                           FlushWorkload(), CompactionWorkload(),
                                           TailRepairWorkload(),
                                           EmptyKeyWorkload(),
                                           MultiOutputWorkload()),
                         [](const auto& info) { return info.param.name; });

// ---- Oracle self-checks ---------------------------------------------------------

// A state built by running steps once, without a crash.
SimFs StateAfter(const Workload& w, const std::vector<Step>& steps) {
    CrashSimSyscalls sim;
    sim.MakeDirs(kRoot);
    Progress ignored;
    RunSteps(sim, w, steps, ignored);
    EXPECT_EQ(ignored.error, "");
    return sim.state();
}

// The empty key is a valid key: the oracle must check it, and must reject
// a recovered state that lost or changed an acknowledged empty-key write.
TEST(CrashOracleTest, WrongEmptyKeyStateIsAViolation) {
    Workload w;
    w.name = "EmptyKeyOracle";
    w.steps = {OpenStep(), Put("", "new"), Put("x", "1"), CloseStep()};
    const std::vector<std::string> keys = KeysOf(w);
    EXPECT_EQ(keys.front(), "");

    Progress p;
    p.initialized = true;
    p.acked = {{true, "", "new"}, {true, "x", "1"}};
    std::string outcome;

    // The correct state passes.
    EXPECT_EQ(CheckRecovery(w, p, StateAfter(w, w.steps),
                            CrashModel::kProcess, &outcome),
              "");

    // The acknowledged empty-key Put is missing.
    const SimFs lost = StateAfter(w, {OpenStep(), Put("x", "1"), CloseStep()});
    EXPECT_NE(CheckRecovery(w, p, lost, CrashModel::kProcess, &outcome), "");

    // The empty key holds an older value.
    const SimFs stale = StateAfter(
        w, {OpenStep(), Put("", "old"), Put("x", "1"), CloseStep()});
    EXPECT_NE(CheckRecovery(w, p, stale, CrashModel::kProcess, &outcome), "");

    // An acknowledged Delete of the empty key must not be undone.
    Progress deleted = p;
    deleted.acked.push_back({false, "", ""});
    EXPECT_NE(CheckRecovery(w, deleted, StateAfter(w, w.steps),
                            CrashModel::kProcess, &outcome),
              "");
}

// The initialization expectations, checked on crafted directories and
// fabricated results, so a wrong engine result would be reported.
TEST(CrashOracleTest, InitializationOutcomesFollowTheDirectoryTable) {
    struct Case {
        const char* what;
        std::vector<std::pair<std::string, std::string>> files;
        bool make_dir;
        InitExpect expect;
    };
    const std::string manifest = "m";  // contents are not inspected
    const std::vector<Case> cases = {
        {"missing directory", {}, false, InitExpect::kInitialize},
        {"empty directory", {}, true, InitExpect::kInitialize},
        {"LOCK only", {{"LOCK", ""}}, true, InitExpect::kInitialize},
        {"partial WAL temporary", {{"LOCK", ""}, {"wal-000001.log.tmp", "x"}},
         true, InitExpect::kResidue},
        {"WAL published, no manifest", {{"wal-000001.log", "w"}}, true,
         InitExpect::kResidue},
        {"manifest temporary", {{"wal-000001.log", "w"}, {"MANIFEST.tmp", "t"}},
         true, InitExpect::kResidue},
        {"manifest published",
         {{"LOCK", ""}, {"wal-000001.log", "w"}, {"MANIFEST", manifest}}, true,
         InitExpect::kDatabase},
        {"unrelated entry", {{"notes.txt", ""}}, true, InitExpect::kUnexpected},
        {"other engine name", {{"sst-000002.sst", ""}}, true,
         InitExpect::kUnexpected},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.what);
        CrashSimSyscalls sim;
        sim.MakeDirs(kRoot);
        if (c.make_dir) {
            sim.MakeDirs(kDb);
        }
        for (const auto& [name, bytes] : c.files) {
            sim.PutFile(std::string(kDb) + "/" + name, bytes);
        }
        EXPECT_EQ(ExpectedInitOutcome(ReadDir(sim)), c.expect);
    }

    const Status ok = Status::Ok();
    const Status corrupt = Status::Corruption("residue");
    const Status io = Status::IOError("disk");
    EXPECT_EQ(JudgeInitOpen(InitExpect::kInitialize, ok, true), "");
    EXPECT_NE(JudgeInitOpen(InitExpect::kInitialize, corrupt, true), "");
    EXPECT_EQ(JudgeInitOpen(InitExpect::kDatabase, ok, true), "");
    EXPECT_NE(JudgeInitOpen(InitExpect::kDatabase, corrupt, true), "");
    EXPECT_EQ(JudgeInitOpen(InitExpect::kResidue, corrupt, true), "");
    EXPECT_NE(JudgeInitOpen(InitExpect::kResidue, ok, true), "");
    EXPECT_NE(JudgeInitOpen(InitExpect::kResidue, io, true), "");
    EXPECT_NE(JudgeInitOpen(InitExpect::kResidue, corrupt, false), "");
    EXPECT_NE(JudgeInitOpen(InitExpect::kUnexpected, corrupt, true), "");
    EXPECT_NE(JudgeInitOpen(InitExpect::kUnexpected, ok, true), "");
}

// Residue left by a real initialization crash is judged by the table: the
// engine reports Corruption and leaves every file as it was.
TEST(CrashOracleTest, InitializationResidueIsPreservedByTheEngine) {
    CrashSimSyscalls sim;
    sim.MakeDirs(kRoot);
    sim.MakeDirs(kDb);
    sim.PutFile(std::string(kDb) + "/wal-000001.log.tmp", "partial");
    const DirState before = ReadDir(sim);
    ASSERT_EQ(ExpectedInitOutcome(before), InitExpect::kResidue);
    std::unique_ptr<DB> db;
    const Status s = OpenDb(sim, InitWorkload(), /*create=*/true, db);
    EXPECT_EQ(JudgeInitOpen(InitExpect::kResidue, s,
                            ReadDir(sim).files == before.files),
              "");
    EXPECT_EQ(db, nullptr);
}

// The mutation check of design.md section 10: skipping the SST content
// sync must be caught under the unsynced-loss model once a manifest that
// references the SST is durable, while the normal control passes and the
// process model (which keeps completed writes) still recovers.
class CrashMutationTest : public ::testing::TestWithParam<Workload> {};

TEST_P(CrashMutationTest, SkippedSstContentSyncIsCaughtOnlyUnderUnsyncedLoss) {
    const Workload& w = GetParam();
    const MatrixResult control = RunMatrix(w);
    EXPECT_TRUE(control.violations.empty()) << Join(control.violations);

    const MatrixResult mutated = RunMatrix(w, ".sst.tmp");
    EXPECT_EQ(mutated.points, control.points);
    std::uint64_t loss = 0;
    for (const std::string& v : mutated.violations) {
        EXPECT_NE(v.find(" unsynced-loss "), std::string::npos)
            << "the process model must keep completed writes: " << v;
        if (v.find(" unsynced-loss ") != std::string::npos) {
            ++loss;
        }
    }
    EXPECT_GT(loss, 0u) << "the mutation was not detected";
    if (w.name == "MultiOutput") {
        // Caught on a compaction output too, not only on a flushed SST.
        bool output = false;
        for (const std::string& v : mutated.violations) {
            output = output || v.find("sst-000006.sst: size 0") !=
                                   std::string::npos;
        }
        EXPECT_TRUE(output) << Join(mutated.violations);
    }
    RecordProperty("crash_points", std::to_string(mutated.points));
    RecordProperty("control_violations",
                   std::to_string(control.violations.size()));
    RecordProperty("mutated_unsynced_loss_violations", std::to_string(loss));
    if (!mutated.violations.empty()) {
        RecordProperty("first_violation", mutated.violations.front());
    }
}

// Flush output, and compaction output split into several SSTs.
INSTANTIATE_TEST_SUITE_P(Workloads, CrashMutationTest,
                         ::testing::Values(FlushWorkload(),
                                           MultiOutputWorkload()),
                         [](const auto& info) { return info.param.name; });

}  // namespace
}  // namespace kv::test
