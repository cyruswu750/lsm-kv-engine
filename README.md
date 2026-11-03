# lsm-kv-engine

[![CI](https://github.com/cyruswu750/lsm-kv-engine/actions/workflows/ci.yml/badge.svg)](https://github.com/cyruswu750/lsm-kv-engine/actions/workflows/ci.yml)

An embedded persistent byte-key/byte-value store in C++20, built to study
recovery of acknowledged writes, safe publication of on-disk state, and the
costs of durability. It does not use LevelDB or RocksDB as its storage
implementation, and its formats are intentionally not LevelDB compatible.
The design contract is [docs/design.md](docs/design.md). The exact M2
SSTable, manifest, naming, and directory-state rules are in
[docs/sst-format.md](docs/sst-format.md).

The badge shows the result of the latest remote CI run. Local test results
are not a substitute for it.

Verification status of the M2 code (commit c6a410f):

- Remote CI: GitHub Actions run 41539015954 on c6a410f succeeded (Debug,
  ASan and UBSan, 400 tests passed).
- Local: the same 400 tests passed in a Debug ASan/UBSan build with test
  data on ext4, with no skipped or disabled tests.
- Release tag: `v0.2-m2` (annotated) is on `6d702e4`; the commits
  after `c6a410f` change documentation only.

## Status: M2 (SSTables, synchronous flush, manifest)

M1 (WAL and memtable) and M2 are implemented. Compaction (M3) and Bloom
filters and benchmarks (M4) are not.

Implemented in M1:

- `DB::Open`, `Put`, `Get`, `Delete` (`src/core/db.h`). Mutations are
  synchronous: encode, append the WAL record, fsync, apply to the memtable,
  then return `Ok`.
- An ordered `std::map` memtable with tombstones, so an empty value is
  distinct from a deleted key.
- WAL v1 (design.md section 6): a 16-byte file header, then records with a
  28-byte header and separate header and payload CRC-32C checksums.
- Recovery (design.md section 7): replay from byte 16, truncate and sync an
  incomplete trailing record before any append, and fail closed on
  corruption, including a bad final record.
- An exclusive `flock` on `LOCK` (a second opener gets `Busy`), and
  rejection of unrelated or non-regular entries.
- After any failure once persistence has begun, the handle returns
  `NeedsRecovery` for every later call. A failed fsync is never retried.

Implemented in M2 (docs/sst-format.md):

- SSTable version 1: 4 KiB target data blocks (a larger single entry gets
  its own block), a sparse index with one entry per block, a 48-byte
  footer, and CRC-32C on every block, the index, and the footer. Open
  validates every referenced SST by streaming its blocks; Get re-validates
  the block it reads.
- A single-record, checksummed `MANIFEST` naming the active WAL, the
  checkpoint sequence, the next file ID, a generation, and the live SSTs
  with their exact sequence ranges. It is replaced through
  `MANIFEST.tmp`, fsync, rename, and directory fsync.
- Synchronous flush before a valid mutation when the memtable reaches
  `Options::memtable_bytes`: SST and new WAL synced and renamed, one
  directory fsync, manifest published, state installed, old WAL removed.
  Publication failures move the handle to `NeedsRecovery`. A failed
  unlink during cleanup is not an error. A failed cleanup directory fsync
  is.
- Lookup: memtable, then SSTs from newest to oldest, stopping at the
  first value or tombstone.
- M2 file names (`wal-000001.log`, `sst-000002.sst`). An M1 directory is
  rejected with `UnsupportedFormat` and left unchanged; there is no
  migration. Interrupted first initialization leaves residue that is
  reported as `Corruption` and preserved.
- File IDs are never reused: allocation starts above the manifest's
  high-water mark and every ID observed on disk. High-ID orphans are kept
  until a published manifest covers them. Sequence, file-ID, and
  generation exhaustion reject mutations before any I/O.

Settled design decisions and their validating tests are in
[docs/decisions.md](docs/decisions.md). Observed defects are in
[docs/BUGLOG.md](docs/BUGLOG.md).

## Build and test

Requires Linux, CMake 3.20 or newer, Ninja, and GCC or Clang with C++20.
GoogleTest is fetched at configure time.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DKV_SANITIZE=ON -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure --parallel "$(nproc)"
```

`KV_SANITIZE=ON` builds everything with AddressSanitizer and
UndefinedBehaviorSanitizer. CI ([.github/workflows/ci.yml](.github/workflows/ci.yml))
runs exactly this configuration on every push to `main` and on every pull
request.

### Differential tests

`Seeds/DifferentialTest.*` compares five workloads against an independent
reference map: the memtable; the DB without reopen; the DB with a reopen
every 500 operations; and the same two DB workloads with a 4 KiB memtable
(`DbFlushWithoutReopen`, `DbFlushPeriodicReopen`), so flushes and SST
lookups happen throughout. The flush workloads read the published MANIFEST
with an independent parser and require at least `KV_DIFF_OPS / 500` flushes
per seed. Each seed records its flush count as a test property. Seeds
1-20 at 5000 operations produced 35 to 50 flushes each. It runs 20 seeds x
5000 operations per workload, and CI pins these values. Environment
variables:

| Variable | Default | Meaning |
|---|---|---|
| `KV_DIFF_FIRST_SEED` | 1 | Seeds used are `FIRST_SEED` .. `FIRST_SEED+19` |
| `KV_DIFF_OPS` | 5000 | Operations per seed and workload |
| `KV_DIFF_REOPEN_EVERY` | 500 | Reopen interval for `DbPeriodicReopen` |

For more seeds, run `scripts/run_differential.sh [BLOCKS] [OPS]`, which
runs 20-seed blocks. A failure prints the seed, operation index, the last
operations, and a reproduce command, and writes the full trace up to the
failing operation to a `.trace` file in the working directory. Traces are
not automatically minimized.

The DB workloads are fsync-bound. On an ext4 disk under WSL2, one M1
`DbPeriodicReopen` seed took about 18 seconds. On tmpfs, fsync is
effectively free. The tests create their directories under the system
temporary directory, so where that lives decides which case a run measures.

## What the tests do and do not show

- Fault tests inject syscall results (short writes and reads, EINTR,
  ENOSPC, EIO, failed open, close, rename, unlink, and fsync, and
  exceptions) through an internal per-object seam (decisions D-004 and
  D-014). They exercise the engine's control flow and recovered state.
  Each M2 publication fault test asserts that the injected fault was
  actually reached.
- No test simulates power loss or discards the page cache. Results show
  behavior under the filesystem's fsync contract, not hardware durability.
  WSL2 results also depend on host storage.

### M1 failure matrix

"Seam" means syscall results injected through `DBOpener` (D-004, D-014).
"Passes locally" means passing in local sanitizer builds; remote CI is
reported only by the badge. Test names are GoogleTest suite.name. The rows
below cover the WAL paths introduced in M1; they run against the M2 layout,
where the WAL is `wal-<id>.log` and is named by MANIFEST.

| Checkpoint | Mechanism | Expected result | Test | Status |
|---|---|---|---|---|
| WAL append: short writes, EINTR | Seam | Exact record written, `Ok` | `DBFaultTest.ShortWritesAndEintrStillWriteExactRecord` | Passes locally |
| WAL append: partial then EIO; ENOSPC/EIO before progress | Seam | `IOError`, then `NeedsRecovery` with no I/O; reopen yields the prior prefix | `DBFaultTest.AppendFailureInvalidatesHandleAndRecoversPrefix` | Passes locally |
| WAL fsync: EIO, ENOSPC, EINTR | Seam | One fsync, `IOError`, `NeedsRecovery`; reopen yields prefix or prefix + failed op | `DBFaultTest.SyncFailureInvalidatesHandleWithoutRetry`, `DBFaultTest.OverwriteWithFailedSyncRecoversOldOrNewValue` | Passes locally |
| Exception during append or fsync | Seam | Handle invalidated, exception propagates | `DBFaultTest.ExceptionAfterPersistenceBeginsInvalidatesHandle` | Passes locally |
| Exception while applying to memtable | None | Handle invalidated (code review only) | None | Not tested |
| Init: header write, tmp fsync | Seam | No handle; no WAL published; since M2, reopen reports the temporary file as preserved `Corruption` residue (M1 reinitialized) | `DBFaultTest.HeaderWriteFailureDuringInitialization`, `DBFaultTest.TempFileSyncFailureDoesNotPublishWal` | Passes locally |
| Init: rename of the initial WAL | Seam (since M2) | No handle; residue is `Corruption` | `PublishFaultTest.InitializationFaultsExposeNoHandle` | Passes locally |
| Init: directory fsync after the WAL rename; after the MANIFEST rename; parent fsync after mkdir | Seam | No handle; no rollback. Reopen: `Corruption` residue before MANIFEST exists (M1 reopened); success after the MANIFEST rename; uninitialized after mkdir | `DBFaultTest.DirectorySyncFailureAfterRenameExposesNoHandle`, `DBFaultTest.ManifestDirectorySyncFailureExposesNoHandle`, `DBFaultTest.ParentSyncFailureAfterCreatingDirectory` | Passes locally |
| Recovery read: short, EINTR, EIO | Seam | Correct replay; EIO leaves WAL untouched | `TornTail.ShortReadsAndEintrDuringRecovery`, `TornTail.ReadErrorDuringRecoveryLeavesWalUntouched` | Passes locally |
| Tail repair: ftruncate or fsync failure; ordering | Seam and event log | No handle and no append; ftruncate then fsync before first write | `TornTail.TruncateFailureLeavesWalAndExposesNoHandle`, `TornTail.SyncFailureDuringRepairExposesNoHandle`, `TornTail.RepairIsTruncatedAndSyncedBeforeFirstAppend` | Passes locally |
| Torn WAL at every byte (222 cuts) | Byte truncation | Exact complete-record prefix, repaired length, append and reopen; header cuts are `Corruption` | `DBRecoveryTest.EveryCutRecoversCompletePrefixAndAcceptsAppend`, `DBRecoveryTest.EveryCutInsideFileHeaderIsCorruption` | Passes locally |
| Corrupt bytes and malformed fields | Byte mutation, resealed CRCs | `Corruption` with path and offset; WAL unchanged | `DBRecoveryTest.*` mutation and malformed-field tests | Passes locally |
| Second opener, same process | Real `flock` | `Busy`; open succeeds after release | `DBTest.SecondOpenInSameProcessIsBusy` | Passes locally |
| Second opener, another process | Subprocess | `Busy` | None | Not tested |
| Process termination at named checkpoints (append, sync, apply, response, init publication) | Subprocess kill | Ordered prefix plus optional in-flight op | None | Not tested |
| Random operation traces with reopen | Differential, seeds 1-20 x 5000 ops | Matches the reference map | `Seeds/DifferentialTest.*` | Passes locally |
| Power loss, lost writeback | None | Not modeled | None | Out of scope |

### M2 failure matrix

| Checkpoint | Mechanism | Expected result | Test | Status |
|---|---|---|---|---|
| Flush F1-F5: every open, write, fsync, close, and rename of the SST, new WAL, and manifest, plus both directory fsyncs | Seam (EIO, ENOSPC) | `IOError`, then `NeedsRecovery` with no I/O; reopen yields every acknowledged write and never reuses an observed file ID | `PublishFaultTest.EveryPublicationFailureRequiresRecovery` | Passes locally |
| Exception during publication or cleanup | Seam | Handle invalidated; reopen yields the old or new generation | `PublishFaultTest.ExceptionBeforeOrAfterDurablePublication` | Passes locally |
| Cleanup: unlink of the old WAL fails | Seam | `Ok`; the handle stays usable; removal is retried at the next flush | `PublishFaultTest.UnlinkFailureLeavesGarbageButKeepsTheHandle` | Passes locally |
| Cleanup: directory fsync after unlink fails | Seam (EIO, EINTR) | `IOError`, `NeedsRecovery`, no retry; reopen yields the new generation | `PublishFaultTest.CleanupDirectorySyncFailureRequiresRecovery` | Passes locally |
| Reopen after a MANIFEST rename whose directory fsync failed | Seam | Open fsyncs the directory before removing the old WAL or accepting writes; if that fsync fails, no handle and nothing removed | `UnsyncedManifestTest.*` | Passes locally |
| Order of syncs and renames in a flush | Seam event trace | Each dependency synced once and renamed before the manifest refers to it; old WAL removed only after publication | `PublishFaultTest.FlushOrderSyncsEachDependencyOnceBeforePublication` | Passes locally |
| Initialization: every step | Seam | No handle; residue before the MANIFEST rename is `Corruption` and preserved; after the rename, reopen succeeds | `PublishFaultTest.InitializationFaultsExposeNoHandle` | Passes locally |
| SST read error at Open, and during Get | Seam | Open: no handle. Get: error returned, handle stays usable | `PublishFaultTest.SstReadFailureAtOpenExposesNoHandle`, `PublishFaultTest.SstReadFailureDuringGetKeepsTheHandle` | Passes locally |
| SST truncated at every byte; every byte bit-flipped; malformed fields with valid CRCs | Byte truncation and mutation | `Corruption` (`UnsupportedFormat` for the version field) | `SstTest.*` | Passes locally |
| Manifest truncated, bit-flipped, invalid fields, unknown version | Byte truncation and mutation | `Corruption` or `UnsupportedFormat`; never replaced | `Manifest.*`, `DBM2Test.MalformedManifestIsNeverTreatedAsANewDatabase` | Passes locally |
| Missing, damaged, or mismatched referenced SST or WAL | File removal and mutation | `Corruption` (`UnsupportedFormat` for SST version 2); nothing changed | `DBM2ReferenceTest.*` | Passes locally |
| Deleted key reappearing | Flushes and reopen | Never: a flushed tombstone or a WAL-only tombstone hides an older SST value | `DBM2Test.FlushedDeleteShadowsOlderFlushedValue`, `DBM2Test.WalOnlyDeleteShadowsFlushedValue` | Passes locally |
| Random traces with frequent flushes | Differential, 4 KiB memtable, seeds 1-20 x 5000 ops, reopen every 500 | Matches the reference map; at least 10 flushes per seed | `Seeds/DifferentialTest.DbFlush*` | Passes locally |
| Process termination or power loss during publication | None | Planned for the M3 crash simulator | None | Not tested |

Coverage status: every testable item on the Week 1 and Week 2 checklists
has a passing local test; remote CI, review, and release status are listed
at the top. These items on the broader design.md section 10 roadmap remain
open:

- Failure-matrix documentation: the matrices above are compact. Seed and
  observed-result columns, and M3 checkpoints (compaction publication),
  are still to be added.
- Process termination and simulated power loss at enumerated checkpoints
  (the M3 crash simulator): not implemented.
- Cross-process lock testing: not implemented.

## Limitations

- Recovery reads the whole active WAL into memory, so peak memory is about
  twice its size (D-007). A flush retires the WAL when the memtable
  reaches its threshold, but overwrites that do not grow the memtable
  still grow the WAL.
- No compaction yet (M3): every flush adds an SST, and Get may probe
  every SST from newest to oldest. Each probe opens and closes the file
  (D-011).
- Open reads every live SST in full to validate it (D-010). Startup I/O
  grows with the data size.
- Flushes run in the foreground, inside the `Put` or `Delete` that finds
  the memtable full.
- Each SST holds at most 1,048,576 blocks and a 64 MiB index (B-006,
  D-010). The default `memtable_bytes` cannot reach these limits. With
  `memtable_bytes` above about 47.9 MiB and long keys, a flush can exceed
  them; the database then stays readable but refuses mutations that need a
  flush. Acknowledged data is not lost (docs/sst-format.md section 3.6).
- A crash during the very first initialization can leave residue that
  Open reports as `Corruption`. It needs manual cleanup or a fresh
  directory.
- M1 databases cannot be opened (D-009).
- One process and one calling thread. The lock is advisory. It does not
  stop other processes from editing the directory.
- Symbolic links are not followed: a symlinked database directory, `LOCK`,
  or WAL is rejected (D-005). Only one directory level is created.
- Untested failure cases are listed in the failure matrices above.
- Once sequence numbers reach `UINT64_MAX`, further mutations are rejected
  with `InvalidArgument` (D-006). The same applies when file IDs or
  manifest generations run out (D-013).

## Layout

```
src/core/      engine: codecs (WAL, SST, manifest), memtable, file names,
               POSIX file/directory/lock, DB
src/testing/   header-only test helpers (temp dirs, independent WAL, SST,
               and manifest builders, fault-injecting syscalls)
tests/harness/ GoogleTest suites
scripts/       longer differential runs
docs/          design contract, M2 format spec, decisions, bug log
```
