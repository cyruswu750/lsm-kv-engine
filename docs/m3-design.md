# M3 Design: Full Compaction and the Crash Simulator

Status: specification of M3. It extends design.md sections 9 and 10
and docs/sst-format.md. Where they differ, design.md is authoritative.
design.md sections 8 and 9 state the owner-approved capacity policy of
section 5 (a multi-SST base run, run-count triggering, manifest version 2
only). SST v1, WAL v1, and the file names are unchanged.
The manifest is version 2 only (sst-format.md section 4): M2 databases
(manifest version 1) are deliberately unsupported, with no migration.
M3 decisions are recorded in section 11 for now; they move to
docs/decisions.md when the pending M2 documentation is integrated.

## 1. Scope

In scope:
- Streaming traversal of one SST and a multiway merge of all live SSTs.
- Synchronous full compaction of every live SST into one base run of
  zero or more key-partitioned SSTs, each within the B-006 limits.
- Manifest version 2 with `base_run_count`; a trigger by logical run
  count.
- Mediation of every filesystem call the engine makes, including path
  inspection, directory listing, directory creation, and locking.
- A deterministic in-memory filesystem simulator (`CrashSimSyscalls`,
  test-only) with separate volatile and durable state, crash enumeration,
  and a response-aware recovery oracle.

Out of scope: Bloom filters, benchmarks (M4), leveled or partial
compaction, background work, and any public Options change.

## 2. Internal interfaces

`src/core/db_internal.h`:

```cpp
struct InternalOptions {
    // Logical run count (sst-format.md section 4.1) that triggers a
    // compaction before a mutation. 0 disables compaction; 1, or a value
    // above max_manifest_ssts, is rejected (InvalidArgument).
    std::size_t compaction_trigger = 4;
    // SST writer limits for flush and compaction output. Tests lower them;
    // values above the format limits are clamped (SstLimits).
    SstLimits sst_limits;
    // SST-entry limit for the manifest. Tests lower it; values above
    // kMaxManifestSsts are clamped.
    std::uint32_t max_manifest_ssts = kMaxManifestSsts;
};

class DBOpener {
    static Status Open(dir, const Options&, PosixSyscalls&, std::unique_ptr<DB>&);
    static Status Open(dir, const Options&, const InternalOptions&,
                       PosixSyscalls&, std::unique_ptr<DB>&);
};
```

`DB::Open` uses the defaults and the real syscalls. Nothing new is
reachable through the public `Options`.

`src/core/sst.h`:

```cpp
struct SstEntry { std::string_view key; std::uint64_t sequence;
                  ValueType type; std::string_view value; };

// Streams one SST in key order, holding one validated block at a time.
class SstIterator {
public:
    static Status Open(const SstTable& table, std::unique_ptr<SstIterator>& out);
    bool Valid() const;               // false at the end or after an error
    const SstEntry& entry() const;    // views stay valid until Next
    Status Next();
    Status Close();
};
```

Each block is validated exactly as at Open (sst-format.md section 3.4,
step 4): checksum, entry rules, key order across blocks, and the index
last key. The iterator keeps one read-only descriptor open until it is
closed or destroyed.

`src/core/compaction.h`:

```cpp
// Merges inputs by key, in any input order. For each key it yields the
// version with the greatest sequence. Any two versions of one key with the
// same sequence are Corruption, including two older versions shadowed by
// a newer one.
class MergingIterator {
public:
    static Status Open(const std::vector<const SstTable*>& tables,
                       std::unique_ptr<MergingIterator>& out);
    bool Valid() const;
    const SstEntry& entry() const;  // the winning version, tombstones included
    Status Next();
    Status Close();
};
```

There is no separate output-writing function. `DB::Impl::PublishCompaction`
(src/core/db.cc) drains the merge, skips winning tombstones, and adds each
winning Put to an `SstBuilder` whose temporary file is created on the
first surviving entry.

`src/core/posix_file.h` (`PosixSyscalls`) gains, with real-syscall
defaults:

| Method | Default |
|---|---|
| `Lstat(path, stat*)` | `lstat(2)` |
| `Mkdir(path, mode)` | `mkdir(2)` |
| `ListDirectory(path, names&)` | `opendir`/`readdir`/`closedir`, excluding `.` and `..`; returns 0 or -1 with errno |
| `Flock(fd, op)` | `flock(2)` |
| `Checkpoint(name)` | no-op |

`InspectPath`, `ListDirectory`, `CreateDirectory` (posix_dir.h) and
`LockFile` (file_lock.h) take a `PosixSyscalls&` parameter that defaults
to the real syscalls. `FileLock` releases through the same seam. After
this change, every filesystem access by the engine goes through the seam
(sst-format.md section 12's list of unmediated calls is now empty).

`Checkpoint(name)` marks engine boundaries that are not syscalls. The
engine calls it at:

| Name | Where |
|---|---|
| `write.applied` | after a mutation is applied to the memtable, before Put/Delete returns |
| `flush.installed` | after a flush's new state is installed (F6), before cleanup |
| `compaction.installed` | after a compaction's new state is installed, before cleanup |

The simulator treats each call as a crash point. The test driver adds
`response.before` and `response.after` around the moment it records a
successful response.

## 3. Mutation ordering

Put/Delete:

1. Reject without I/O: NeedsRecovery, lengths, sequence exhaustion.
2. Encode the WAL record.
3. If the memtable is nonempty and at its threshold, flush (sst-format.md
   section 10).
4. If compaction is enabled and the logical run count (sst-format.md
   section 4.1) is at least the trigger, compact (section 4).
5. Append and sync the record to the active WAL, apply it, call
   `write.applied`, return Ok.

A flush or compaction error is returned for the triggering mutation, and
the mutation is not appended. Compaction runs at step 4 even without a
flush in step 3, for example after a reopen that found many flush runs.

Why runs, not SSTs. A compaction may publish a base run of several SSTs.
Counting SSTs would make that base alone reach the trigger, so every
following mutation would merge the same unchanged inputs again. Counting
the base run once means a new compaction needs `trigger - 1` new flushes,
the same amount of new work as with single-SST outputs, and it stays so
after reopen because the grouping is persisted (`base_run_count`).

## 4. Compaction

Steps (inputs are all live SSTs; S1, S2, ... are output IDs):

| Step | Action |
|---|---|
| C0 | Checks without I/O: generation not exhausted, and one file ID available (next_file_id at most 2^64-2). Failure: InvalidArgument, handle usable, mutation rejected |
| C1 | Open an `SstIterator` per input and merge. All versions of a key are compared together; the greatest sequence wins; shadowed versions are dropped; a winning tombstone is dropped with its key. Each surviving Put goes to the current output through `SstBuilder`. An output is created (`sst-<Si>.sst.tmp`, a new file ID) on the first entry that the current output cannot take: `SstBuilder::CanAdd` checks that adding the entry and then finishing would stay within the block and index limits. The finished output is fsynced, closed, and renamed to `sst-<Si>.sst`. So splits fall only between keys, outputs are key-ordered and key-disjoint, and an empty builder always accepts one entry of the maximum legal size. Before each output is created: an ID must be available (next_file_id at most 2^64-2), and the output count must stay within max_manifest_ssts - (compaction_trigger - 1); otherwise InvalidArgument. With no survivor, no file is created |
| C2 | If any output exists: one directory fsync after the last rename, so every output's name is durable before a manifest names it (each output's contents were fsynced in C1) |
| C3 | Retry the removal of a stale `MANIFEST.tmp` (as F4). Write the next manifest to `MANIFEST.tmp`: generation + 1; next_file_id = the in-memory allocation floor (above every output; otherwise unchanged, which still covers any high-ID orphan seen at Open); checkpoint and active_wal_id unchanged; SST list = the outputs in key order; base_run_count = the output count (0 if none). fsync; close |
| C4 | Rename over `MANIFEST`; fsync the directory. Publication of all outputs is durable when this returns, in one step |
| C5 | Install: move the retired input names into the deferred-orphan list, then install the manifest and the table list. Everything installed was built before publication, so C5 only moves and swaps and does not allocate. `compaction.installed` |
| C6 | Cleanup: every input becomes an orphan below next_file_id; unlink them; if any unlink succeeded, fsync the directory |

Output metadata: min_sequence and max_sequence are taken from the
emitted entries only. The checkpoint is kept even if nothing survives,
so last_sequence and the next sequence never fall back (after an empty
compaction the manifest still carries checkpoint >= every used sequence).

Correctness. Every live SST is an input, and the memtable and the active
WAL hold only sequences above the checkpoint, which bounds every SST
sequence. So for each key, the inputs contain every version that any SST
holds, and every version outside them is newer. Keeping the greatest
input sequence is therefore the newest SST-resident version, and if that
is a tombstone, no older version exists anywhere to resurrect: dropping
it together with its key is safe. A newer version in the memtable or WAL
still shadows the output, because lookup checks the memtable first.

Errors follow the flush classification:

| Failure | Returned | Handle | Durable state |
|---|---|---|---|
| C0 check | InvalidArgument | usable; no I/O | unchanged |
| Any error in C1-C4, including input read errors, input corruption, file-ID exhaustion after the first output, and the output-count limit | that error | NeedsRecovery | old or new generation; files of the other are orphans (outputs at or above the durable next_file_id are kept until a later publication covers them, D-013) |
| Exception C1-C5 | propagated | NeedsRecovery | as above. An exception after C4 (for example, from the `compaction.installed` checkpoint in the simulator) leaves memory on the old generation while the new one is durable; the handle is unusable, and reopen recovers the new generation |
| Unlink failure in C6 | not reported; mutation proceeds | usable | new generation; garbage retried later |
| Directory fsync failure in C6 | IOError | NeedsRecovery | new generation |

Inputs are deleted only in C6, after the manifest that drops them is
durable. A file that the installed manifest references is never removed.

Resources. Compaction holds one descriptor and one block buffer per
input (a block is at most `kMaxSstBlockSize`) plus one output block and
one output descriptor at a time. Each finished output's index stays in
memory for installation, so peak index memory is about the old tables'
indexes plus the new outputs' indexes.
The merge scans the inputs linearly per key, so it costs O(inputs) per
entry, plus O(h^2) sequence comparisons for a key held by h inputs (every
pair is checked for a duplicate sequence). With the default trigger the input count is the trigger; a
database reopened with many SSTs (for example, built with compaction
disabled) merges all of them at once.

## 5. Capacity policy (owner-approved)

B-006 limits one SST to 1,048,576 blocks and a 64 MiB index. With a
single compaction output, those limits capped the whole database's live
data. Inputs that were each legal could merge into an output that was
not, and the database then refused every mutation. With 64 KiB keys this
happened at 1,023 live keys.

The owner approved the following policy, implemented in M3:
- Full compaction is kept. Every live SST is an input, so dropping
  winning tombstones stays safe (section 4).
- The output is one base run of zero or more SSTs, split at key
  boundaries before either per-SST limit would be exceeded. The per-SST
  limits and the public key and value limits are unchanged.
- The manifest is version 2 with `base_run_count` (sst-format.md section
  4). Key ranges are derived at Open, not persisted. M3 reads and writes
  version 2 only. M2 (version 1) databases are rejected with
  UnsupportedFormat and left unchanged. There is no migration,
  dual-version reader, conditional version 1 writer, or downgrade.
- Compaction is triggered by logical run count (section 3).

Remaining limits, stated honestly:
- Live data must fit in max_manifest_ssts - (compaction_trigger - 1)
  outputs: about 2^20 SSTs at the default settings, each within the
  per-SST limits (at least 1,023 entries even with 64 KiB keys). A larger
  merge fails during C1 with InvalidArgument and NeedsRecovery, and the
  database then refuses mutations that trigger compaction (no data is
  lost).
- A flush still writes one SST, so the B-006 large-memtable flush case
  (sst-format.md section 3.6) is unchanged.
- File IDs: one per output; exhaustion is described in sst-format.md
  section 6.
- Compaction rewrites all live data (full rewrite write amplification),
  and Open still reads every SST in full.

History: until this policy, the branch failed such a compaction closed
(InvalidArgument and NeedsRecovery, data intact, repeating after
reopen). Section 5.1 of the earlier draft proposed the policy above; the
owner approved it with the version 2 only boundary.

## 6. Crash simulator

`src/testing/crash_sim.h`, test-only: `CrashSimSyscalls : PosixSyscalls`.

State. A namespace of inodes. A directory inode has volatile entries and
durable entries (name to inode). A file inode has volatile contents and
durable contents; size is the contents' length. A descriptor refers to
an inode with an offset and access flags. Locks belong to descriptors.
Paths are absolute; there are no symbolic links, FIFOs, or hard links in
the model.

| Operation | Volatile effect | Durable effect |
|---|---|---|
| open with O_CREAT on a missing name | new empty inode linked in the parent | none |
| open with O_TRUNC | contents emptied | none |
| write, ftruncate | contents changed (full writes, no short writes) | none |
| fsync on a file | none | durable contents = volatile contents |
| fsync on a directory | none | durable entries = volatile entries of that directory |
| rename | entry moved atomically, replacing a target | none |
| unlink | entry removed | none |
| mkdir | new directory linked in the parent | none |
| flock, close | lock table only | none |

A file sync never makes a name durable, and a directory sync never makes
contents durable. A directory created by mkdir is durable only after its
parent is synced: under the power-loss model a lost directory takes its
whole subtree with it, even if files inside were synced.

Crash points. Every completed call that changes volatile or durable
state is a crash point: open that creates or truncates, write, ftruncate,
fsync, rename, unlink, mkdir. So is every `Checkpoint` call. When the
armed point k completes, the simulator copies its whole state into a
snapshot, marks itself dead, and throws `SimulatedCrash`. A dead instance
never changes the snapshot: every later call fails with EIO (close
returns 0) and nothing throws, so destructors and error paths in the
engine cannot alter what was captured. A crash is never reported as an
ordinary error status.

Models. From a snapshot a new instance is built with no descriptors and
no locks:
- Process crash: volatile state is kept (the page cache survives).
- Unsynced-state loss ("simulated power loss"): only durable state is
  kept. The namespace is rebuilt from the root through durable entries;
  each reachable file gets its durable contents. Unreachable inodes are
  dropped.

The second model tests loss of unsynced state. It does not model torn
writes, reordered writeback inside a file, or device failures, and it is
not evidence of real power-loss behavior.

Mutation hook (test-only): `skip_file_sync_suffix`. An fsync on a file
whose name at open ends with that suffix returns success without
updating durable contents.

## 7. Test driver and oracle

A crash workload is a fixed list of steps (open, put, delete, reopen,
close) with fixed options, run against a fixed initial simulator state.
The driver first runs it without a crash and counts N crash points. For
every k in 1..N it restores the identical initial state, runs until the
crash at k, takes the snapshot, and for each of the two models:

1. Builds the model instance and reopens the database.
2. Checks the oracle.
3. Continues: performs new mutations, closes, reopens, and checks again.

Oracle. The driver records each mutation whose call returned Ok and
whose response was recorded (`response.after`). The in-flight mutation,
if any, is the one being executed at the crash. The recovered state must
equal, over every key that a Put or Delete step names (the empty key
included) plus a never-written key, either the model after all acknowledged mutations or the model after
those plus the in-flight one. A state that mixes keys from different
prefixes fails. Get must not return an error.

Initialization. If the crash happened before the first Open returned,
there is no acknowledged mutation. The test decides the allowed result
from the recovered directory contents alone, with the directory state
table (sst-format.md section 8), without calling the engine's
classifier:
- Missing directory, empty, or `LOCK` only: Open must initialize and
  succeed with an empty database.
- `MANIFEST` present: Open must succeed.
- Only `LOCK`, `wal-000001.log.tmp`, `wal-000001.log`, and
  `MANIFEST.tmp`, with at least one of the last three: Open must return
  Corruption and leave every name and every file's bytes unchanged.
- Any other name: a violation.
Once a database has been initialized, the reopen must succeed.

Mutation test. A flush workload is run with
`skip_file_sync_suffix = ".sst.tmp"`. At crash points after the
publishing manifest's directory sync, the normal control (no mutation)
must pass under both models. With the mutation, the process-crash model
must still pass (completed writes survive), and the power-loss model
must violate the oracle at least once (the SST is named durably but its
contents were never synced, so Open reports Corruption). A skipped
directory sync is not used as the mutation, because a later sync of the
same directory may cover it.

## 8. Workloads

Crash matrix (CI, fixed, small):

| Workload | Exercises |
|---|---|
| `Init` | directory creation, parent sync, WAL and manifest initialization, first writes |
| `WalAppend` | puts, deletes, overwrites on an initialized database |
| `Flush` | repeated flushes (tiny memtable), compaction disabled |
| `Compaction` | flushes with trigger 2, including deleting every key (empty output) and writing again |
| `TailRepair` | a torn WAL tail and a covered orphan at Open, then appends |
| `EmptyKey` | the empty key through overwrite, delete, empty value, flush, and compaction (trigger 2) |
| `MultiOutput` | one block per output SST: compaction into a multi-SST base run, then flushes above it, an empty key, a delete, and an overwrite |

The missing-SST-content-sync mutation runs on `Flush` and `MultiOutput`;
the latter must also be caught on a compaction output.

Differential (20 seeds x 5000 operations, 4 KiB memtable, reopen every
500 operations, trigger 2):
- `DbCompactionPeriodicReopen`: default SST limits (single-SST outputs
  in practice).
- `DbMultiOutputCompactionPeriodicReopen`: at most three blocks per SST
  and Put values padded by 600 bytes, so outputs split into multi-SST
  base runs while every flush still fits in one SST.

Both assert repeated flushes and compactions (installed-checkpoint
counts), fewer logical runs than the trigger at the end, at most one
compaction per flush (no repeated merge of an unchanged base, including
across reopens), and, for the multi-output workload, a base run of at
least two SSTs.

## 9. Verification scope

Covered: the enumerated crash points of the fixed workloads under two
models, publication and cleanup faults (including at a later output)
through FaultSyscalls, compaction and run semantics, capacity
exhaustion with lowered internal limits, the manifest version boundary
(a genuine M2 fixture, and an M2 build opening an M3 database), and the
differential workloads. Not covered: every possible execution, real
power loss, writeback reordering inside a file, torn sector writes,
concurrency, the capacity limits at their real sizes (tested with
lowered internal limits), and a flush larger than one SST (unchanged
from M2).

## 10. Adapted earlier expectations

- M2 workloads that relied on SSTs accumulating use
  `compaction_trigger = 0` where their assertions count SSTs.
- The approved format boundary changes version expectations. Manifest
  version 2 is now valid, and versions 1 and 3 are UnsupportedFormat
  (`Manifest.UnknownVersionIsUnsupportedFormat`,
  `DBM2Test.MalformedManifestIsNeverTreatedAsANewDatabase`). The test
  builder writes version 2 by default; `version = 1` builds the M2
  layout.
- Manifest golden bytes are version 2 with a base run, recomputed by an
  independent script. The field and count cases follow the new run rules.
- The interim test `DBM3Test.OutputOverTheSstLimitFailsClosedAndRepeats`
  is replaced by the split tests: the approved policy splits the output
  instead of failing.

## 11. Decisions (to move into docs/decisions.md)

- M3-D1 Full compaction of all live SSTs into one base run of
  key-partitioned SSTs (zero or more), at an internal trigger on the
  logical run count (default 4, minimum 2, 0 disables). Not in Options.
  Cost: each compaction rewrites all live data.
- M3-D2 Compaction keeps the checkpoint and active WAL, computes each
  output's sequence bounds from its emitted entries, and allocates an ID
  only per created output (C0 still requires one to be available).
- M3-D3 Compaction errors follow the flush classification (section 4).
- M3-D4 The full syscall seam (lstat, mkdir, directory listing, flock)
  plus named non-syscall checkpoints. Production defaults are the real
  syscalls; the checkpoint default is a no-op.
- M3-D5 Simulator crash = snapshot + throw from a completed mutating
  call or checkpoint; a dead instance fails every later call without
  changing the snapshot.
- M3-D6 Two models: process crash keeps volatile state; unsynced loss
  keeps only durable contents reachable through durable entries.
- M3-D7 Oracle accepts all acknowledged mutations plus optionally the
  in-flight one, over the whole key set; initialization crashes follow
  the directory state table.
- M3-D8 Capacity (owner-approved, section 5): split outputs at key
  boundaries within the per-SST limits; the output count is limited to
  leave room for the flushes before the next compaction.
- M3-D9 Manifest version 2 only, with `base_run_count`. Version 1 (M2)
  and unknown versions are UnsupportedFormat with no file changed; no
  migration or downgrade. Key ranges are derived at Open, not persisted.
- M3-D10 Trigger by logical run count, so that a multi-SST base run is
  not compacted again before new flushes, including after reopen.
