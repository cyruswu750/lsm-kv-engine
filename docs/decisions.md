# Design Decisions

Decisions that affect correctness, durability, architecture, or measurable
performance, in the order they were made, each with its alternatives, cost,
and validating tests. IDs are stable; a decision replaced by a later one keeps
its text and is marked here and in its entry. Engine versions are named by
their release tags (M1 = `v0.1-m1` … M4 = `v0.4-m4`).

| ID | Decision | Status |
|---|---|---|
| D-001 | Publish new WAL files through a temporary file | Active; the M1 file names it used were replaced by D-009 |
| D-002 | Require recovery after a WAL sync failure | Active |
| D-003 | Recognize interrupted initialization | Superseded by D-009 |
| D-004 | Inject DB-level faults through an internal syscall seam | Active |
| D-005 | Classify database entries with lstat and scan twice | Active |
| D-006 | Report sequence exhaustion as InvalidArgument before I/O | Active |
| D-007 | Read the whole WAL into memory during M1 recovery | Active |
| D-008 | Replace the manifest as one whole-file record | Active |
| D-009 | Separate M2 file names; reject M1 directories without migration | Active |
| D-010 | SST layout with per-block checksums and full streaming validation at Open | Active |
| D-011 | Open each SST per probe instead of holding descriptors | Active |
| D-012 | Publication order and the separate cleanup error classes | Active |
| D-013 | File-ID high-water mark, orphan retention, and counter exhaustion | Active |
| D-014 | Extend the syscall seam to open, pread, fstat, rename, and unlink | Active |
| D-015 | SST read errors during Get do not invalidate the handle | Active |
| D-016 | Open syncs the database directory before retiring or acknowledging | Active |
| D-017 | Full compaction of every live SST (M3) | Active |
| D-018 | Compaction keeps the checkpoint and the active WAL (M3) | Active |
| D-019 | Compaction errors follow the flush classification (M3) | Active |
| D-020 | Mediate every filesystem call, plus named checkpoints (M3) | Active |
| D-021 | A simulated crash freezes a snapshot and kills the instance (M3) | Active |
| D-022 | Two crash models (M3) | Active |
| D-023 | A response-aware recovery oracle (M3) | Active |
| D-024 | Compaction output is one key-partitioned base run (M3) | Active |
| D-025 | Manifest version 2 only; no M2 migration (M3) | Active |
| D-026 | Trigger compaction by logical run count (M3) | Active |
| D-027 | Bloom filters in SST version 2 (M4) | Active |
| D-028 | A damaged filter fails Open (M4) | Active |
| D-029 | Bounded filter memory (M4) | Active |
| D-030 | Internal counters and benchmark-only switches (M4) | Active |

## D-001: Publish new WAL files through a temporary file

Status: active. The principle applies to every WAL; the file names below are
the M1 ones, replaced by `wal-<id>.log` in D-009 (sst-format.md section 8).

- Decision: Write the header to `000001.wal.tmp`, fsync the file,
  rename it to `000001.wal`, then fsync the database directory.
  Do not write the initial header directly to the final WAL path.
- Reason: An interrupted initialization should not leave a partially
  written header at the final WAL path, where it would be treated as
  database corruption.
- Validation: Failures injected through the syscall seam (D-004) before
  and after publication: `DBFaultTest.HeaderWriteFailureDuringInitialization`,
  `DBFaultTest.TempFileSyncFailureDoesNotPublishWal`,
  `DBFaultTest.DirectorySyncFailureAfterRenameExposesNoHandle`,
  `DBFaultTest.ParentSyncFailureAfterCreatingDirectory`. Each reopen either
  completes initialization or opens the published WAL. Planned: process
  termination at named checkpoints; these tests do not model power loss.

## D-002: Require recovery after a WAL sync failure

- Decision: On a WAL sync failure, return `IOError` and move the
  database handle to `NeedsRecovery`. Do not retry and treat a later
  successful sync as proof that the original write is durable.
- Reason: The persistence outcome is uncertain. Continuing with the
  current in-memory state could hide a mismatch with the WAL.
  Reopening must validate and replay the available log.
- Validation: `DBFaultTest.SyncFailureInvalidatesHandleWithoutRetry`
  (EIO, ENOSPC, EINTR; one fsync attempt),
  `DBFaultTest.AppendFailureInvalidatesHandleAndRecoversPrefix`,
  `DBFaultTest.ExceptionAfterPersistenceBeginsInvalidatesHandle`. Later
  Put/Delete/Get return `NeedsRecovery` with no further I/O; a reopen
  recovers the prior state, optionally including the failed operation.

## D-003: Recognize interrupted initialization

Status: superseded by D-009. Since M2, initialization residue without a
MANIFEST is preserved as Corruption, and an M1 directory is UnsupportedFormat.

- Decision: For M1, a directory containing only `LOCK` and the
  recognized initialization file `000001.wal.tmp` may be initialized
  when `create_if_missing` is true, after acquiring the exclusive lock.
  Unrelated files are not ignored, and an invalid final WAL is not
  silently replaced.
- Reason: A crash between lock-file creation and WAL publication
  should not permanently prevent initialization.
- Validation: `DBTest.UninitializedStatesFollowCreateIfMissing`,
  `DBTest.UnrelatedEntriesWithoutWalAreRejected`,
  `DBTest.InvalidFinalWalFailsWithoutReplacement`,
  `DBTest.NonRegularWalIsCorruptionWithoutBlocking`,
  `DBTest.SecondOpenInSameProcessIsBusy`. A second opener is tested in the
  same process only; a cross-process opener is not yet tested.

## D-004: Inject DB-level faults through an internal syscall seam

- Decision: `DBOpener::Open` (`src/core/db_internal.h`) has the same
  contract as `DB::Open` but takes a `PosixSyscalls&`. Its virtual
  `Write`, `Read`, `Ftruncate`, `Fsync`, and `Close` carry every WAL read,
  write, truncation, file sync, directory sync, and close. `DB::Open`
  passes the real-syscall default. The seam object is not owned and must
  outlive the DB.
- Alternatives: A hook in `Options` (excluded by design.md section 10).
  Global mutable hooks (hidden shared state between tests). Syscall
  interposition such as `LD_PRELOAD` (process-wide, not per object). A full
  filesystem interface (many methods with no current fault test).
- Reason: Fault tests run the production write loop, recovery, and
  publication code unchanged, with faults isolated to one object.
- Cost: `open`, `mkdir`, `rename`, `unlink`, `flock`, and directory
  listing always use real syscalls, so rename failure is not injected.
  A forgotten seam argument silently falls back to real I/O. (M2: D-014
  moves open, rename, unlink, pread, and fstat behind the seam.)
- Validation: `DBFaultTest.*` and `TornTail.*`.

## D-005: Classify database entries with lstat and scan twice

- Decision: Entry kinds come from `lstat`; symbolic links are never
  followed. A database path that is a symlink, a file, or another
  non-directory returns `InvalidArgument`. `LOCK` must be a regular file
  (`InvalidArgument` otherwise). A non-regular `000001.wal` is
  `Corruption`. `000001.wal.tmp` is the recognized initialization file only
  when it is a regular file; otherwise it counts as an unrelated entry.
  Open scans once before taking the lock, so `NotFound` and
  `InvalidArgument` cases never create `LOCK`, and again under the lock
  before acting. Only one directory level is created, and the parent is
  synced only when this call created it. When a valid WAL exists, a
  regular leftover `000001.wal.tmp` is removed after recovery succeeds;
  that removal is not synced because a reappearing file is removed again.
- Alternatives: Following symlinks (an engine file could resolve outside
  the database directory); recursive parent creation (expands the
  design's creation policy).
- Reason: Regular-file helpers never block on a FIFO or act through a
  link, and directory state is decided by what the directory contains.
- Cost: A symlinked database directory is unsupported. The lock is
  advisory: it does not stop other processes from replacing entries
  between inspection and use.
- Validation: `DBTest.PathThatIsNotADirectoryIsRejected`,
  `DBTest.NonRegularLockIsRejected`,
  `DBTest.NonRegularWalIsCorruptionWithoutBlocking`,
  `DBTest.ExistingWalRemovesOnlyRecognizedLeftoverTemp`,
  `DBTest.MissingParentIsNotCreated`.

## D-006: Report sequence exhaustion as InvalidArgument before I/O

- Decision: A recovered sequence of `UINT64_MAX` is valid. A later
  Put/Delete returns `InvalidArgument` before encoding or I/O, and the
  handle stays usable for Get.
- Alternatives: `NeedsRecovery` (blocks reads although nothing failed);
  `IOError` (implies an I/O failure); a new status code (changes the
  public `Code` enum).
- Reason: Nothing is persisted or invalidated, and the public API is
  unchanged.
- Cost: `InvalidArgument` does not name the condition; callers must read
  the message.
- Validation: `DBTest.MaxSequenceIsPreservedAndFurtherMutationsRejected`,
  `DBTest.PutCanUseMaxSequenceOnce`.

## D-007: Read the whole WAL into memory during M1 recovery

- Decision: Recovery reads the complete WAL with `ReadFileToString`,
  then decodes records from that buffer.
- Alternatives: A streaming reader with a bounded buffer.
- Reason: The decoder works on one validated span and copies retained
  bytes into the memtable, which keeps M1 recovery simple.
- Cost: Peak recovery memory is about twice the WAL size (buffer plus
  memtable copies). Recovery is not bounded-memory for arbitrarily large
  WALs. In M2 a flush retires the WAL whenever the memtable reaches
  `memtable_bytes` before a mutation, so the active WAL holds roughly one
  memtable's worth of records. This is still not a strict bound:
  overwrites add WAL bytes without growing the memtable estimate.
- Validation: None; this is a documented limitation.

## D-008: Replace the manifest as one whole-file record

- Decision: MANIFEST is a single checksummed record (docs/sst-format.md
  section 4) that each publication replaces through `MANIFEST.tmp`, file
  fsync, rename, and directory fsync. It is never appended to.
- Alternatives: An append-only edit log with periodic snapshots (the
  LevelDB approach).
- Reason: The whole live state (one active WAL, an SST list without
  levels or key ranges) is small. A single record has one validation
  path, has no tail-repair rules, and cannot accumulate a long replay
  history. Rename gives readers the old or the new generation, never a
  mixture.
- Cost: Each flush rewrites 60 + 32 x (live SSTs) bytes. Without M3
  compaction the SST list grows with every flush. (M3: the manifest is
  version 2, D-025.) The list is capped at
  2^20 entries; at the cap, a flush is refused (D-013).
- Validation: `Manifest.*` (golden bytes, every truncation, every bit
  flip, resealed invalid fields, unknown versions),
  `PublishFaultTest.FlushOrderSyncsEachDependencyOnceBeforePublication`.

## D-009: Separate M2 file names; reject M1 directories without migration

- Decision: M2 files are `MANIFEST`, `wal-<id>.log`, and `sst-<id>.sst`
  (6 to 20 digit canonical IDs), plus `.tmp` forms. A directory without
  MANIFEST but with `000001.wal` (any kind) or a regular `000001.wal.tmp`
  is a recognized M1 directory. Open returns UnsupportedFormat and
  changes nothing, not even LOCK. Without MANIFEST, any regular M2 engine
  file is interrupted-initialization residue: Corruption, preserved, and
  never reinitialized. MANIFEST, when present, takes precedence, and M1
  names next to it are ignored.
- Alternatives: Reuse `000001.wal` as the first M2 WAL (an interrupted M2
  initialization would then look like a valid M1 database). Migrate M1
  WALs on open (more code and failure modes for a format that has no
  users).
- Reason: Distinct names make each state unambiguous from a listing
  alone, and fail-closed residue handling never guesses which partial
  initialization step happened.
- Cost: An M1 database cannot be opened by M2. A crash during the very
  first initialization can leave a directory that needs manual cleanup or
  a fresh path. M1 tests that expected automatic recovery from
  `000001.wal.tmp` now expect UnsupportedFormat or Corruption; each
  adapted test cites the spec row.
- Validation: `DBM2Test.ValidM1DirectoryIsUnsupportedAndUnchanged`,
  `DBM2Test.InterruptedInitializationResidueIsPreservedCorruption`,
  `DBM2Test.MalformedManifestIsNeverTreatedAsANewDatabase`,
  `DBTest.UninitializedStatesFollowCreateIfMissing`,
  `PublishFaultTest.InitializationFaultsExposeNoHandle`.

## D-010: SST layout with per-block checksums and full streaming validation at Open

- Decision: SST version 1 is data blocks (target 4 KiB of entries, no
  prefix compression, CRC-32C per block), one index block (last key,
  offset, and length per block, CRC-32C), and a 48-byte footer (CRC-32C,
  magic, version, and zero filter fields). There is no file header. A
  single entry larger than the target gets its own block, bounded by the
  API limits. Open reads every referenced SST block by block and checks
  every structural rule, key order, and the exact sequence range recorded
  in the manifest. Get re-validates the one block it reads.
  Amended by B-006: an SST has at most `kMaxSstBlocks` = 1,048,576
  blocks and an index of at most `kMaxSstIndexSize` = 64 MiB, trailer
  included. The writer checks each block as it closes it, before writing
  it, and returns InvalidArgument at a limit. The reader rejects an
  index_length above min(floor(index_offset / 25), 2^20) x (16 + 65,536)
  + 8 or above 64 MiB as Corruption from the footer alone, before
  allocating the index (docs/sst-format.md sections 3.4 and 3.6).
- Alternatives: Validate only the footer and index at Open and trust
  blocks until read. Keep a cached copy of whole SSTs in memory. For the
  index: bound it only by the file size (the B-006 defect: a sparse file
  makes any size cheap), or read it in chunks without a total limit
  (still unbounded memory for the parsed handles). Split an oversized
  flush into several SSTs (more publication states, out of M2 scope).
- Reason: Corruption and sequence-range errors surface at Open with one
  explicit rule set, and memory stays at one block plus the in-memory
  indexes. A fixed index limit makes the largest allocation for an
  index independent of what a file claims.
- Cost: Open reads every live SST in full (startup I/O proportional to
  data size). Per-block checksums are also recomputed on every Get. One
  SST holds about 2 to 4 GiB of small entries, or 1,023 entries with
  65,536-byte keys. The default memtable_bytes cannot reach either limit
  (any value up to 50,266,080 cannot). A larger memtable_bytes with long
  keys can: that flush fails in F1, the handle needs recovery, and every
  later flush of the replayed memtable fails too, so the database stays
  readable but refuses mutations that trigger a flush. No acknowledged
  data is lost. M2 tests the writer limits directly, not this database
  path end to end.
- Validation: `SstTest.*` (golden bytes from an independent script,
  every truncation, every bit flip, resealed malformed footer, index, and
  blocks), `DBM2ReferenceTest.*`. Limits:
  `SstTest.ImpossibleIndexLengthIsRejectedBeforeTheIndexIsRead`,
  `SstTest.IndexLengthAboveTheFormatLimitIsRejectedBeforeTheIndexIsRead`,
  `SstTest.IndexBlockCountAboveTheLimitIsCorruption` (all three failed
  before B-006 was fixed), and the writer boundaries with lowered limits,
  `SstTest.BuilderRefusesABlockPastTheBlockLimitBeforeWritingIt` and
  `SstTest.BuilderIndexLimitIsExactAndCountsEachBlocksLastKey`.

## D-011: Open each SST per probe instead of holding descriptors

- Decision: `SstTable` keeps the validated index in memory. Each Get
  probe opens the file read-only, preads one block, and closes it.
- Alternatives: Keep one descriptor open per live SST.
- Reason: Without compaction (M3), the number of SSTs grows with every
  flush. Per-probe opens keep descriptor use constant and keep ownership
  simple, and the M3 simulator will not need to model long-lived read
  descriptors.
- Cost: Each probed SST costs an extra open and close. M4 Bloom filters
  skip most probes for missing keys. The cost is measured in M4, not
  assumed.
- Validation: `SstTest.ReadsGoThroughTheSyscallSeam` (one close per
  Open and per Get, including error paths).

## D-012: Publication order and the separate cleanup error classes

- Decision: A flush writes and syncs the SST and the new WAL, renames
  both, then makes both names durable with one directory sync. Only then
  does it write, sync, and rename MANIFEST and sync the directory again
  (docs/sst-format.md section 10). Any failure before that second sync
  returns its error and moves the handle to NeedsRecovery. After it, the
  in-memory state is installed without allocation. An unlink failure
  during cleanup is not reported, and the file is retried at the next
  publication. A cleanup directory-sync failure returns IOError and moves
  the handle to NeedsRecovery, without retrying the sync. A close failure
  on the already-synced old WAL is ignored.
- Alternatives: A separate directory sync after each rename, as the
  earlier design.md section 8 step 2 text listed (design.md now states
  the single sync). Treating every cleanup failure as fatal. Treating
  cleanup sync failures as nonfatal.
- Reason: One sync covers both renames because no manifest refers to
  either file before it completes. Unlinking an unreferenced file cannot
  lose data. A failed directory fsync, however, may hide other lost
  metadata, so the handle conservatively stops.
- Cost: One fewer directory sync per flush than a sync per rename.
  After a cleanup sync failure, the caller must reopen even though the
  published state is durable.
- Validation: `PublishFaultTest.EveryPublicationFailureRequiresRecovery`
  (every syscall of F1-F5, EIO and ENOSPC, each asserting that the
  injected fault was reached),
  `PublishFaultTest.ExceptionBeforeOrAfterDurablePublication`,
  `PublishFaultTest.UnlinkFailureLeavesGarbageButKeepsTheHandle`,
  `PublishFaultTest.OldWalCloseFailureIsNotFatal`,
  `PublishFaultTest.CleanupDirectorySyncFailureRequiresRecovery`,
  `PublishFaultTest.FlushOrderSyncsEachDependencyOnceBeforePublication`.
  These inject syscall results only. They do not model process
  termination or power loss.

## D-013: File-ID high-water mark, orphan retention, and counter exhaustion

- Decision: Allocation starts above both the manifest's next_file_id and
  every engine-name ID visible at Open, including temporary files,
  orphans, and non-regular entries. Orphans below next_file_id are
  removed at Open on a best-effort basis, after Open's directory sync has
  made the covering manifest durable (D-016); the removals themselves are
  not synced.
  Orphans at or above it are kept until a published manifest covers
  them, and that publication's cleanup removes them. File IDs (a flush
  needs two), generations, and the SST-count cap are checked before any
  flush I/O. Exhaustion of any of them, like sequence exhaustion (D-006),
  returns InvalidArgument and leaves the handle usable for reads.
- Alternatives: Delete every orphan at Open (could erase the only
  evidence of a high ID and allow reuse after a crash). Persist a
  high-water mark before deleting (an extra publication at Open).
  Silently skip the flush when IDs run out (violates the memtable
  threshold without telling the caller).
- Reason: Published IDs and IDs observed at Open are never reused, at
  the cost of one extra flush-time cleanup.
- Cost: High-ID orphans occupy space until the next flush. An ID
  allocated in a crashed session whose file vanished completely has no
  recoverable identity and is not covered. At exhaustion, every mutation
  that needs a flush is rejected.
- Validation: `DBM2Test.HighIdOrphansAreKeptUntilAPublishedHighWaterCoversThem`
  (two reopens without reuse), `DBM2Test.CoveredOrphansAreRemovedAndOthersKept`,
  `DBM2Test.FailedOrphanRemovalAtOpenIsRetriedAfterPublication`,
  `DBM2Test.FileIdExhaustionRejectsMutationWithoutIo`,
  `DBM2Test.ObservedMaxIdOrphanExhaustsFileIds`,
  `DBM2Test.GenerationExhaustionRejectsMutationWithoutIo`,
  `DBM2Test.ExhaustedSequenceDoesNotTriggerFlush`.

## D-014: Extend the syscall seam to open, pread, fstat, rename, and unlink

- Decision: `PosixSyscalls` gains `Open`, `Pread`, `Fstat`, `Rename`, and
  `Unlink`. Every WAL, SST, and manifest file operation, and the
  descriptor opened for each directory sync, goes through it. `RenameFile`
  and `RemoveFile` take the seam (the real syscalls by default).
  `FaultSyscalls` records armed calls with their target file names and
  supports rules that fail or throw on a named operation and file. This
  extends D-004 and removes its "rename is not injected" cost.
- Alternatives: A full virtual filesystem interface now (that belongs to
  M3, which needs volatile and durable state).
- Reason: Publication, cleanup, and SST read faults must run through
  production code to be meaningful.
- Cost: lstat, directory listing, mkdir, and the LOCK file's open and
  flock still use real syscalls. They are listed in docs/sst-format.md
  section 12 for the M3 simulator. (M3: D-020 mediates them too.)
- Validation: every `PublishFaultTest` case asserts that its rule fired.
  `SstTest.ReadsGoThroughTheSyscallSeam`,
  `ManifestFileTest.ReadsThroughTheSeamAndNamesThePath`.

## D-015: SST read errors during Get do not invalidate the handle

- Decision: A pread error or block corruption found by Get returns
  IOError or Corruption from that Get and clears the output. The handle
  stays usable.
- Alternatives: Move to NeedsRecovery, as after a write failure.
- Reason: Nothing durable or in memory changed, so the handle's state
  still matches the published state. Write failures are different
  because they can leave durable and in-memory state disagreeing.
- Cost: A persistently bad SST keeps failing Gets that reach it until the
  database is reopened (when Open's validation rejects it) or repaired.
- Validation: `PublishFaultTest.SstReadFailureDuringGetKeepsTheHandle`,
  `SstTest.GetRevalidatesTheBlockItReads`.

## D-016: Open syncs the database directory before retiring or acknowledging

- Decision: After an existing database's manifest, SSTs, and active WAL
  are validated and replayed, Open fsyncs the database directory once.
  Only after that does it remove orphans and expose a handle. A failure
  returns IOError, removes nothing, and exposes no handle.
- Alternatives: No sync at Open (the earlier M2 code). Rewriting the
  manifest at every Open.
- Reason: The visible MANIFEST can come from a rename whose directory
  sync never completed: a crash between F5's rename and its sync, or a
  failed sync that invalidated the previous handle. design.md section 8
  step 4 forbids acknowledging operations on that generation, or deleting
  files it superseded, until the sync completes. One directory sync
  establishes that state without writing anything new.
- Cost: One directory fsync per Open of an existing database. After a
  real writeback error, a successful sync on a new descriptor still does
  not prove that earlier lost data reached storage (design.md section 5).
- Validation: `UnsyncedManifestTest.OpenSyncsDirectoryBeforeRetiringOrAcknowledging`,
  `UnsyncedManifestTest.FailedOpenDirectorySyncRetiresNothing` (both
  failed before the change; B-005).

## D-017: Full compaction of every live SST (M3)

- Decision: Before a mutation, after any pending flush, compaction runs
  synchronously when the logical run count reaches an internal threshold
  (default 4, minimum 2, 0 disables; not in public Options). Every live
  SST is an input. Per key the greatest sequence wins; shadowed versions
  and winning tombstones are dropped.
- Alternatives: Leveled or partial compaction (tombstones would have to
  be kept, since an unselected file could resurrect a value).
- Reason: Coverage of every older version is trivially true, so dropping
  tombstones is safe.
- Cost: Each compaction rewrites all live data (write amplification grows
  with the database size between compactions).
- Validation: `DBM3Test.*`, `CompactionInputTest.*`,
  `Seeds/DifferentialTest.DbCompactionPeriodicReopen/*`.

## D-018: Compaction keeps the checkpoint and the active WAL (M3)

- Decision: A compaction publishes generation + 1 with the same checkpoint
  and active WAL. Each output's sequence range comes from its emitted
  entries. A file ID is allocated only per created output; an empty
  merge publishes no SST and keeps the checkpoint, so sequences never
  restart.
- Alternatives: Recompute the checkpoint (unnecessary; the WAL is not
  involved).
- Reason: Compaction changes only SST layout.
- Cost: None measured.
- Validation: `DBM3Test.AllDeletedLeavesNoSstAndSequencesContinue`,
  `DBM3Test.CompactionDropsShadowedVersionsAndWinningTombstones`.

## D-019: Compaction errors follow the flush classification (M3)

- Decision: Counter exhaustion before I/O: InvalidArgument, handle usable.
  Any failure before the manifest's directory sync: NeedsRecovery. After
  it: an input unlink failure is not reported, and a cleanup directory
  sync failure is IOError with NeedsRecovery. Inputs are deleted only
  after the new manifest is durable.
- Alternatives: Treat compaction as best effort and ignore its errors
  (could hide a lost publication).
- Reason: The same reasoning as D-012.
- Cost: A failed compaction blocks the triggering mutation.
- Validation: `CompactionFaultTest.*` (14 + 10 injection points, EIO and
  ENOSPC, and exceptions).

## D-020: Mediate every filesystem call, plus named checkpoints (M3)

- Decision: `PosixSyscalls` also covers lstat, mkdir, directory listing,
  and flock; `InspectPath`, `ListDirectory`, `CreateDirectory`,
  `LockFile`, and `FileLock` use it. `Checkpoint(name)` marks non-syscall
  boundaries (`write.applied`, `flush.installed`,
  `compaction.installed`). Production defaults are the real syscalls and
  a no-op checkpoint.
- Alternatives: A separate virtual filesystem interface.
- Reason: The crash simulator must see every call.
- Cost: One virtual call per filesystem operation.
- Validation: `CrashSimTest.EngineDirectoryHelpersUseTheSeam`; the crash
  matrix runs on a path that does not exist on the real filesystem.

## D-021: A simulated crash freezes a snapshot and kills the instance (M3)

- Decision: After the k-th completed state-changing call or checkpoint,
  the simulator copies its state and throws `SimulatedCrash`. A dead
  instance fails every later call with EIO (close and unlock succeed) and
  never throws, so destructors and error paths cannot change the
  snapshot.
- Alternatives: Returning an error status (not a termination).
- Reason: Termination must not run any further engine I/O.
- Cost: Test-only.
- Validation: `CrashSimTest.CrashFreezesTheSnapshotAndKillsTheInstance`.

## D-022: Two crash models (M3)

- Decision: Process crash keeps volatile state. Unsynced-state loss keeps
  only durable file contents reachable through durable directory entries
  from the root. File sync and directory sync persist different things.
- Alternatives: Modeling writeback order inside files.
- Reason: The two models bracket the guarantees the engine relies on.
- Cost: Neither model is evidence of real power-loss behavior.
- Validation: `CrashSimTest.*`, `CrashMutationTest.*`.

## D-023: A response-aware recovery oracle (M3)

- Decision: After recovery, the state over every key a workload's
  mutations name (the empty key included) plus a never-written key must
  equal all acknowledged mutations, optionally plus the in-flight one.
  Crashes before the first successful Open are judged by the directory
  table from the recovered directory contents, independently of the
  engine.
- Alternatives: Comparing only the last key, or only acknowledged state.
- Reason: An operation may be durable before its response is observed.
- Cost: Test-only.
- Validation: `CrashOracleTest.*`, `Workloads/CrashMatrixTest.*`.

## D-024: Compaction output is one key-partitioned base run (M3)

- Decision: The merged output is split between keys before any per-SST
  limit (blocks, index, filter keys) would be exceeded. All outputs are
  synced, made durable by one directory sync, and published by one
  manifest. The output count is limited to the manifest SST limit minus
  (trigger - 1), leaving room for the flushes before the next compaction.
- Alternatives: A single output (the database's live data would be capped
  by one SST, as little as 1,023 keys of 64 KiB). Raising the per-SST
  limits (weakens B-006).
- Reason: Keeps the B-006 allocation bounds per file.
- Cost: Live data must fit in about 2^20 SSTs; a larger merge fails with
  InvalidArgument and NeedsRecovery.
- Validation: `DBM3Test.CompactionSplitsOutputAtExactBoundariesIntoOneBaseRun`,
  `DBM3Test.CompactionExhaustionFailsBeforePublication`,
  `Seeds/DifferentialTest.DbMultiOutputCompactionPeriodicReopen/*`.

## D-025: Manifest version 2 only; no M2 migration (M3)

- Decision: The manifest keeps the version 1 layout but is version 2,
  with `base_run_count` at offset 52. Version 1 (M2 databases) and every
  other version are UnsupportedFormat before any file changes. There is no
  migration, dual-version reader, or downgrade. Key ranges are derived at
  Open, not stored.
- Alternatives: A conditional version 1 writer, or migration (more states
  and code for a format without users).
- Reason: One manifest format and one validation path.
- Cost: M2 databases cannot be opened; an M2 executable rejects M3 and M4
  databases.
- Validation: `DBM3Test.M2DatabaseIsUnsupportedAndLeftUnchanged`
  (a fixture written by the v0.2-m2 build), `Manifest.*`.

## D-026: Trigger compaction by logical run count (M3)

- Decision: The base run counts once, each flush run once.
- Alternatives: Counting SSTs (a multi-SST base alone would reach the
  threshold and be merged again before every mutation).
- Reason: A new compaction needs trigger - 1 new flushes, before or after
  reopen.
- Cost: None.
- Validation: `DBM3Test.UnchangedBaseRunIsNotCompactedAgainAfterReopen`;
  the compaction differential workloads assert compactions <= flushes.

## D-027: Bloom filters in SST version 2 (M4)

- Decision: Each SST written by M4 is version 2 and holds one Bloom
  filter of every stored key, tombstones included, between the data
  blocks and the index (docs/sst-format.md section 3.7). The hash is
  defined by the format (FNV-1a 64 and SplitMix64 finalizers, double
  hashing), with 10 bits per key and k = 7 by default. Version 1 SSTs stay
  readable in the same database.
- Alternatives: Filters in separate files or in the manifest; std::hash
  (not stable across builds).
- Reason: Missing-key lookups skip SSTs without reading data blocks.
- Cost: About 1.25 bytes of memory and file per key; one hash per probed
  SST.
- Validation: `Bloom.*`, `BloomSstTest.*`, `DBM4Test.*`,
  `Seeds/DifferentialTest.DbMixedFilterPeriodicReopen/*`; measured in
  bench/results/summary.md.

## D-028: A damaged filter fails Open (M4)

- Decision: CRC, size, key-count, and probe-count violations, a key count
  different from the SST's entries, and any stored key that the filter
  would deny are Corruption at Open. Unknown filter or hash versions are
  UnsupportedFormat.
- Alternatives: Ignoring a bad filter (a damaged filter could silently
  hide data).
- Reason: A negative answer must never come from an unchecked filter.
- Cost: One filter probe per stored key at Open.
- Validation: `BloomSstTest.MalformedFiltersWithValidChecksumsFailOpen`,
  `BloomSstTest.EveryFilterBitFlipAndTruncationFailsClosed`.

## D-029: Bounded filter memory (M4)

- Decision: At most 2^22 keys per filter-bearing SST. The reader allocates
  the filter block (at most 33,554,452 bytes) only after the footer and
  the 16-byte header passed their bounds, then copies its bit array once
  (peak 2B + 20 bytes). The builder keeps 8 bytes per key and, at Finish,
  three copies of the bit array (peak 8n + 3B + 20 bytes, at most 128 MiB).
  Compaction splits at the key limit; filterless output has none.
- Alternatives: Bounding the filter only by the file size (a sparse file
  makes that cheap, as in B-006).
- Reason: B-006's allocation rule applied to filters.
- Cost: A flush of more than 2^22 keys fails (the large-memtable case of
  B-006).
- Validation: `BloomSstTest.ImpossibleFilterSizeIsRejectedBeforeTheBitArrayIsRead`,
  `BloomBuilderTest.FilterKeyLimitStopsTheBuilderBeforeWriting`,
  `DBM4Test.CompactionRollsOverAtTheFilterKeyLimit`,
  `DBM4Test.FlushOverTheFilterKeyLimitFailsClosed`,
  `DBM4Test.FilterlessOutputIgnoresTheFilterKeyLimit`.

## D-030: Internal counters and benchmark-only switches (M4)

- Decision: `DbStats` counts lookups (candidates, range and filter
  rejections, data-block requests and bytes) and successful write bytes
  (WAL, flush SSTs, compaction SSTs, metadata). `bypass_bloom_queries`
  and the benchmark driver's sync bypass are internal; the sync bypass
  exists only in bench/kv_bench.cpp.
- Alternatives: Public statistics or Options.
- Reason: Measurements without changing the public API.
- Cost: A few counter updates per call.
- Validation: `DBM4Test.CountersFollowTheirDefinitions`,
  `DBM4Test.BypassChangesRequestsButNotAnswers`.
