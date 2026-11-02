# lsm-kv-engine

[![CI](https://github.com/cyruswu750/lsm-kv-engine/actions/workflows/ci.yml/badge.svg)](https://github.com/cyruswu750/lsm-kv-engine/actions/workflows/ci.yml)

An embedded persistent byte-key/byte-value store in C++20, built to study
recovery of acknowledged writes, safe publication of on-disk state, and the
costs of durability. It does not use LevelDB or RocksDB as its storage
implementation, and its WAL format is intentionally not LevelDB compatible.
The design contract is [docs/design.md](docs/design.md).

The badge shows the result of the latest remote CI run. Local test results
are not a substitute for it.

## Status: M1 (write-ahead log and memtable)

Implemented:

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
- The M1 directory state machine: an exclusive `flock` on `LOCK` (a second
  opener gets `Busy`), WAL publication through `000001.wal.tmp` with file
  and directory fsync, and rejection of unrelated or non-regular entries.
- After any failure once WAL persistence has begun, the handle returns
  `NeedsRecovery` for every later call. A failed fsync is never retried.

Not implemented (M2 and later): SSTables, manifest, flush, compaction,
Bloom filters, benchmarks. `Options::memtable_bytes` is validated but no
flush happens yet.

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

`Seeds/DifferentialTest.*` compares the memtable, the DB without reopen,
and the DB with a reopen every 500 operations against an independent
reference map. It runs 20 seeds x 5000 operations per workload, and CI pins
these values. Environment variables:

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

The DB workloads are fsync-bound. On an ext4 disk under WSL2, one
`DbPeriodicReopen` seed took about 18 seconds. On tmpfs, fsync is
effectively free.

## What the tests do and do not show

- Fault tests inject syscall results (short writes, EINTR, ENOSPC, EIO,
  fsync failure, exceptions) through an internal per-object seam (decision
  D-004). They exercise the engine's control flow and recovered state.
- No test simulates power loss or discards the page cache. Results show
  behavior under the filesystem's fsync contract, not hardware durability.
  WSL2 results also depend on host storage.

### M1 failure matrix

"Seam" means syscall results injected through `DBOpener` (D-004). "Passes
locally" means passing in local sanitizer builds; remote CI is reported
only by the badge. Test names are GoogleTest suite.name.

| Checkpoint | Mechanism | Expected result | Test | Status |
|---|---|---|---|---|
| WAL append: short writes, EINTR | Seam | Exact record written, `Ok` | `DBFaultTest.ShortWritesAndEintrStillWriteExactRecord` | Passes locally |
| WAL append: partial then EIO; ENOSPC/EIO before progress | Seam | `IOError`, then `NeedsRecovery` with no I/O; reopen yields the prior prefix | `DBFaultTest.AppendFailureInvalidatesHandleAndRecoversPrefix` | Passes locally |
| WAL fsync: EIO, ENOSPC, EINTR | Seam | One fsync, `IOError`, `NeedsRecovery`; reopen yields prefix or prefix + failed op | `DBFaultTest.SyncFailureInvalidatesHandleWithoutRetry`, `DBFaultTest.OverwriteWithFailedSyncRecoversOldOrNewValue` | Passes locally |
| Exception during append or fsync | Seam | Handle invalidated, exception propagates | `DBFaultTest.ExceptionAfterPersistenceBeginsInvalidatesHandle` | Passes locally |
| Exception while applying to memtable | None | Handle invalidated (code review only) | None | Not tested |
| Init: header write, tmp fsync | Seam | No handle; no WAL published; reopen recognizes tmp | `DBFaultTest.HeaderWriteFailureDuringInitialization`, `DBFaultTest.TempFileSyncFailureDoesNotPublishWal` | Passes locally |
| Init: rename of `000001.wal.tmp` | None (rename bypasses the seam) | No handle | None | Not tested |
| Init: directory fsync after rename; parent fsync after mkdir | Seam | No handle; no rollback; reopen succeeds | `DBFaultTest.DirectorySyncFailureAfterRenameExposesNoHandle`, `DBFaultTest.ParentSyncFailureAfterCreatingDirectory` | Passes locally |
| Recovery read: short, EINTR, EIO | Seam | Correct replay; EIO leaves WAL untouched | `TornTail.ShortReadsAndEintrDuringRecovery`, `TornTail.ReadErrorDuringRecoveryLeavesWalUntouched` | Passes locally |
| Tail repair: ftruncate or fsync failure; ordering | Seam and event log | No handle and no append; ftruncate then fsync before first write | `TornTail.TruncateFailureLeavesWalAndExposesNoHandle`, `TornTail.SyncFailureDuringRepairExposesNoHandle`, `TornTail.RepairIsTruncatedAndSyncedBeforeFirstAppend` | Passes locally |
| Torn WAL at every byte (222 cuts) | Byte truncation | Exact complete-record prefix, repaired length, append and reopen; header cuts are `Corruption` | `DBRecoveryTest.EveryCutRecoversCompletePrefixAndAcceptsAppend`, `DBRecoveryTest.EveryCutInsideFileHeaderIsCorruption` | Passes locally |
| Corrupt bytes and malformed fields | Byte mutation, resealed CRCs | `Corruption` with path and offset; WAL unchanged | `DBRecoveryTest.*` mutation and malformed-field tests | Passes locally |
| Second opener, same process | Real `flock` | `Busy`; open succeeds after release | `DBTest.SecondOpenInSameProcessIsBusy` | Passes locally |
| Second opener, another process | Subprocess | `Busy` | None | Not tested |
| Process termination at named checkpoints (append, sync, apply, response, init publication) | Subprocess kill | Ordered prefix plus optional in-flight op | None | Not tested |
| Random operation traces with reopen | Differential, seeds 1-20 x 5000 ops | Matches the reference map | `Seeds/DifferentialTest.*` | Passes locally |
| Power loss, lost writeback | None | Not modeled | None | Out of scope |

Coverage status: every item on the Week 1 checklist (DB behavior,
recovery, exhaustive truncation, corruption, directory states and
same-process locking, DB-level fault injection, differential tests) has a
passing local test. Four items on the broader design.md section 10 roadmap
remain open:

- Failure-matrix documentation: the M1 matrix above is a compact version.
  Seed and observed-result columns, and M2/M3 checkpoints (SST, manifest,
  compaction publication), are still to be added.
- Subprocess termination at named checkpoints: not implemented.
- Cross-process lock testing: not implemented.
- Rename-failure injection: not implemented; it needs a seam for rename
  or another mechanism.

## Limitations

- Recovery reads the whole WAL into memory, so peak memory is about twice
  the WAL size (D-007). The WAL grows without bound until M2 adds flush.
- One process and one calling thread. The lock is advisory. It does not
  stop other processes from editing the directory.
- Symbolic links are not followed: a symlinked database directory, `LOCK`,
  or WAL is rejected (D-005). Only one directory level is created.
- Untested failure cases are listed in the M1 failure matrix above.
- Once sequence numbers reach `UINT64_MAX`, further mutations are rejected
  with `InvalidArgument` (D-006).

## Layout

```
src/core/      engine: codec, memtable, POSIX file/directory/lock, DB
src/testing/   header-only test helpers (temp dirs, independent WAL builder,
               fault-injecting syscalls)
tests/harness/ GoogleTest suites
scripts/       longer differential runs
docs/          design contract, decisions, bug log
```
