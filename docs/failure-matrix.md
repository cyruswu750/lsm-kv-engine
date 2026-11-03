# Failure Matrix (M1-M4)

Evidence: one fresh Debug ASan/UBSan acceptance run of the whole suite
on the M4 code before it was committed: the M3 code plus the M4 changes,
with test data on ext4. Result: 534/534 passing, none skipped or
disabled, no sanitizer report. Every count below comes from that run.
The committed code is that code with three comments corrected
(`DB::Get` in src/core/db.cc, src/core/manifest.cc, and `wal_bytes` in
src/core/db_internal.h); the documentation was edited after the run.
The earlier runs (457/457 and 493/493 for M3, 531/531 for M4 before the
pre-commit review added three tests) are historical and describe earlier
trees.

Each row names the regression test that checks it.

Mechanisms:
- **errno**: an error returned through `FaultSyscalls` (EIO, ENOSPC,
  EACCES, EINTR, short transfers), on the real filesystem.
- **throw**: an exception thrown from a syscall through `FaultSyscalls`.
- **sim-P / sim-L**: termination in `CrashSimSyscalls` at an enumerated
  crash point, recovered under the process-crash model (volatile state
  kept) or the unsynced-state-loss model (only synced contents and
  synced directory entries kept; called simulated power loss).
- **bytes**: crafted or truncated file contents.
- **diff**: seeded differential comparison with a reference map.

The sim rows cover only the enumerated points of the fixed workloads in
section 2, not every possible execution. Neither model is evidence of real
power-loss behavior. Injected errors never model a lost page cache.

## 1. Fault injection and malformed input

| Checkpoint | Mechanism | Expected | Observed | Regression test |
|---|---|---|---|---|
| WAL append | errno, short writes, EINTR | short writes and EINTR complete the record; other errors return IOError, invalidate the handle, and recovery yields the old or new prefix | pass | `DBFaultTest.ShortWritesAndEintrStillWriteExactRecord`, `DBFaultTest.AppendFailureInvalidatesHandleAndRecoversPrefix` |
| WAL sync | errno | IOError, NeedsRecovery, no retry; the overwritten key recovers old or new | pass | `DBFaultTest.SyncFailureInvalidatesHandleWithoutRetry`, `DBFaultTest.OverwriteWithFailedSyncRecoversOldOrNewValue` |
| After WAL persistence begins | throw | handle invalidated before propagation | pass | `DBFaultTest.ExceptionAfterPersistenceBeginsInvalidatesHandle` |
| Initialization (WAL, manifest, parent sync) | errno | no handle; residue preserved as Corruption; a completed manifest rename survives | pass | `DBFaultTest.HeaderWriteFailureDuringInitialization`, `TempFileSyncFailureDoesNotPublishWal`, `DirectorySyncFailureAfterRenameExposesNoHandle`, `ManifestDirectorySyncFailureExposesNoHandle`, `ParentSyncFailureAfterCreatingDirectory`, `PublishFaultTest.InitializationFaultsExposeNoHandle` |
| Recovery read of the WAL (short reads, EINTR, EIO), before any modification | errno, short reads | short reads and EINTR replay correctly; EIO returns IOError with no handle, nothing truncated, the torn WAL unchanged | pass | `TornTail.ShortReadsAndEintrDuringRecovery`, `TornTail.ReadErrorDuringRecoveryLeavesWalUntouched` |
| Tail repair: ftruncate fails | errno | IOError, no handle, no fsync and no append; the torn WAL is unchanged; a later Open repairs it | pass | `TornTail.TruncateFailureLeavesWalAndExposesNoHandle` |
| Tail repair: ftruncate succeeds, then its fsync fails | errno | IOError, no handle, no append; the truncation has already happened and is not rolled back (the WAL holds only the complete prefix); a later Open recovers that prefix | pass | `TornTail.SyncFailureDuringRepairExposesNoHandle` |
| Tail repair ordering | event trace | ftruncate, then fsync, before the first append | pass | `TornTail.RepairIsTruncatedAndSyncedBeforeFirstAppend` |
| WAL bytes | bytes | every cut recovers the complete prefix (206 cuts); header and payload mutations are Corruption | pass | `DBRecoveryTest.*` (7) |
| Flush F1-F5 (18 calls x EIO, ENOSPC) | errno | IOError, nothing appended, NeedsRecovery; reopen recovers the published generation with acknowledged data | pass | `PublishFaultTest.EveryPublicationFailureRequiresRecovery` |
| Flush, around durable publication | throw | NeedsRecovery; reopen recovers generation 1 or 2 | pass | `PublishFaultTest.ExceptionBeforeOrAfterDurablePublication` |
| Flush cleanup F7 | errno | unlink failure and old-WAL close failure are not fatal; cleanup directory-sync failure is | pass | `PublishFaultTest.UnlinkFailureLeavesGarbageButKeepsTheHandle`, `OldWalCloseFailureIsNotFatal`, `CleanupDirectorySyncFailureRequiresRecovery` |
| Visible but unsynced manifest at Open | errno | Open syncs the directory before retiring files or acknowledging; a failed sync retires nothing (B-005) | pass | `UnsyncedManifestTest.*` (2) |
| SST reads at Open and Get | errno | Open fails with no handle; Get fails without invalidating | pass | `PublishFaultTest.SstReadFailureAtOpenExposesNoHandle`, `SstReadFailureDuringGetKeepsTheHandle` |
| SST and manifest bytes | bytes | every truncation and bit flip fails closed; oversized indexes rejected before allocation (B-006) | pass | `SstTest.*` (18), `Manifest.*` (8), `ManifestFileTest.*` (2) |
| Missing or damaged referenced files | bytes | Corruption at Open | pass | `DBM2ReferenceTest.*` (4) |
| Compaction C1-C6 (14 calls x EIO, ENOSPC) | errno | C1-C4 failures: IOError, NeedsRecovery, generation 3 recovered; C4 sync and C6 sync failures: generation 4; all acknowledged data recovered and later compactions succeed | pass | `CompactionFaultTest.EveryCompactionFailureRequiresRecovery` |
| Compaction output write, manifest rename, input unlink | throw | NeedsRecovery; recovered generation 3, 3, 4 | pass | `CompactionFaultTest.ExceptionDuringCompactionRequiresRecovery` |
| Compaction input unlink | errno | publication kept, handle usable, garbage removed by the next publication; referenced output never removed | pass | `CompactionFaultTest.InputUnlinkFailureLeavesGarbageButKeepsHandle` |
| Compaction ordering | trace | inputs read, output synced and named durably, manifest published, then inputs removed | pass | `CompactionFaultTest.CompactionOrderSyncsOutputBeforePublication` |
| Compaction input corruption | bytes | Corruption, NeedsRecovery, no output published | pass | `CompactionFaultTest.CorruptInputFailsCompactionAndOpen` |
| Merge: two versions of one key with one sequence, shadowed by a newer version, every input order | crafted SSTs | Corruption from Open and from Next | pass (failed for 4 of 6 orders before the fix, B-007) | `CompactionInputTest.MergeRejectsDuplicateSequencesBehindANewerVersion` |
| Crash oracle: wrong empty-key state; initialization outcome table | crafted states and results | each wrong state or outcome is reported as a violation | pass (the empty-key case failed before the oracle fix) | `CrashOracleTest.*` (3) |
| Compaction output split (lowered limits) | crafted workload | outputs split exactly at the block limit, key-ordered and key-disjoint, with overlapping sequence intervals allowed; one base run | pass | `DBM3Test.CompactionSplitsOutputAtExactBoundariesIntoOneBaseRun` |
| Maximum legal entry, empty and binary keys across outputs; delete everything | crafted workload | the maximum entry gets its own output; all lookups correct; full deletion leaves no SST and keeps the checkpoint; writes continue after reopen | pass | `DBM3Test.MaximumEntryAndEmptyKeysSplitAndDeleteCleanly` |
| Run-count trigger with a multi-SST base | crafted workload | no new compaction of an unchanged base before or after reopen; one compaction after one new flush | pass | `DBM3Test.UnchangedBaseRunIsNotCompactedAgainAfterReopen` |
| Flush runs above a base run | crafted workload | overwrite and tombstone shadow the base; grouping kept across reopen; the next compaction drops the deleted key | pass | `DBM3Test.FlushRunsOverABaseRunShadowItAndKeepTheGrouping` |
| Malformed base runs on disk | bytes | overlapping or misordered key ranges, a base count above the SST count, a flush run not newer than the base, a missing output: Corruption at Open | pass | `DBM3Test.MalformedBaseRunsFailOpen`, `Manifest.InvalidFieldsWithCorrectChecksumAreCorruption` |
| File-ID and manifest-entry exhaustion during a multi-output compaction | crafted manifest, lowered limits | InvalidArgument, NeedsRecovery, durable state and data unchanged; a trigger above the entry limit is rejected at Open | pass | `DBM3Test.CompactionExhaustionFailsBeforePublication` |
| Flush into a full manifest | lowered limit | InvalidArgument with no I/O; handle usable | pass | `DBM3Test.FlushIntoAFullManifestIsRejectedWithoutIo` |
| Faults at the second of two outputs, its directory sync, the manifest, and cleanup | errno | IOError, NeedsRecovery; the first output had completed; reopen recovers generation 3 or 4; orphaned outputs are removed later | pass | `CompactionFaultTest.FailuresAtALaterOutputRequireRecovery` |
| M2 database (manifest version 1) opened by M3 | genuine fixture written by the v0.2-m2 build | UnsupportedFormat with no write, truncate, sync, rename, or unlink; every name and byte unchanged | pass | `DBM3Test.M2DatabaseIsUnsupportedAndLeftUnchanged` |
| Manifest versions written by M3 | inspection | version 2 for new, single-SST, compacted, and emptied databases | pass | `DBM3Test.M3AlwaysWritesManifestVersion2` |
| Unknown manifest versions | bytes | versions 0, 1, 3, and 2^32-1 are UnsupportedFormat | pass | `Manifest.UnknownVersionIsUnsupportedFormat` |
| Filter hash and encoding (M4) | independent golden vectors | KVHASH1 values and complete filter bytes equal scripts/bloom_reference.py; the version 2 SST golden too | pass | `Bloom.Kvhash1MatchesIndependentVectors`, `Bloom.EncodingMatchesIndependentFilterBytes`, `SstTest.ProductionAndTestBuildersMatchGoldenBytes` |
| No false negatives (M4) | generated keys | every added key, including empty, binary, and 300-byte keys, is reported present after reload, at 1, 2, 10, and 64 bits per key | pass | `Bloom.NoFalseNegativesAfterReload` |
| False-positive rate (M4) | 10,000 keys, 100,000 disjoint probes | at most 1.5 p + 0.2% for p from the actual m, n, k; observed 9.13% at 5 bits/key, 0.891% at 10, 0.048% at 16 | pass | `Bloom.FalsePositiveRateMatchesTheParameters` |
| Malformed filters with valid CRCs (M4) | bytes | unknown filter or hash version: UnsupportedFormat; key count 0, above the data region or the limit, or different from the entries; probes 0 or 31; too many or too few bit bytes; a filter that denies a stored key: Corruption | pass | `BloomSstTest.MalformedFiltersWithValidChecksumsFailOpen` |
| Filter bit flips and truncations (M4) | bytes | every flip in the filter block and every truncation of a version 2 SST fails Open | pass | `BloomSstTest.EveryFilterBitFlipAndTruncationFailsClosed` |
| Version 2 footer regions (M4) | bytes | filter length 0, 27, above the limit, or 2^64-1; overflowing or inconsistent offsets; a version 1 footer with a filter: Corruption | pass | `BloomSstTest.FooterFilterRegionsAreBoundedWithoutOverflow` |
| Filter size bounded before allocation (M4) | sparse file | a footer claiming the largest legal filter after a 25-byte data region is rejected after reading 48 + 16 bytes | pass | `BloomSstTest.ImpossibleFilterSizeIsRejectedBeforeTheBitArrayIsRead` |
| Filterless and filter-bearing SSTs together (M4) | engine and hand-built files | version 1 and version 2 SSTs in one manifest version 2 database answer correctly with filters used or bypassed; compaction rewrites as version 2 | pass | `DBM4Test.FilterlessAndFilterBearingSstsCoexist`, `DBM4Test.IndependentlyBuiltVersion1SstOpensBesideVersion2` |
| Tombstone masking through filters (M4) | Put, flush, Delete, flush, compaction disabled | the tombstone SST's filter holds the key; NotFound with filters used or bypassed, before and after reopen | pass | `DBM4Test.FlushedTombstoneMasksOlderValueThroughFilters` |
| Filters on multi-output compaction (M4) | lowered limits | every output is version 2 with a filter of exactly its keys, revalidated at reopen | pass | `DBM4Test.MultiOutputCompactionWritesAFilterPerOutput` |
| Filter key limit in compaction (M4) | lowered limits | compaction rolls over to a new output before the key limit; every output's filter holds at most the limit; data intact after reopen | pass | `DBM4Test.CompactionRollsOverAtTheFilterKeyLimit` |
| Filter key limit in flush (M4) | lowered limits | a flush with more keys than the limit fails with InvalidArgument; NeedsRecovery; no SST published; no data lost after reopen | pass | `DBM4Test.FlushOverTheFilterKeyLimitFailsClosed` |
| Filterless output and the key limit (M4) | bits per key 0 | version 1 output is not subject to the filter key limit | pass | `DBM4Test.FilterlessOutputIgnoresTheFilterKeyLimit` |
| SST writer limit in flush or compaction | lowered limits | InvalidArgument, NeedsRecovery, no data lost; repeats after reopen (capacity issue, docs/m3-design.md section 5) | pass | `DBM3Test.FlushOverTheSstLimitFailsClosedAndRepeats`, `DBM3Test.OutputOverTheSstLimitFailsClosedAndRepeats` |
| Counter exhaustion before compaction | crafted manifest | InvalidArgument without I/O; handle usable | pass | `DBM3Test.CompactionCounterExhaustionRejectsWithoutIo` |

## 2. Enumerated crash points (both models)

Each workload starts from one fixed simulator state. Each point is
checked under both models: reopen, the oracle over every key the
workload's mutations name (including the empty key) plus a never-written
key, continued writes, and a second reopen. A crash before any Open
returned is judged by the directory table (docs/sst-format.md section
8), decided by the test from the recovered directory contents:
- "fresh": initialization was required and succeeded;
- "published": MANIFEST existed and the database opened;
- "residue": Corruption, with the directory left unchanged.

| Workload | Exercises | Points | Recoveries | Outcomes (process / unsynced-loss) | Observed | Test |
|---|---|---:|---:|---|---|---|
| Init | mkdir, parent sync, WAL and manifest initialization, first writes | 28 | 56 | init-fresh 3 / 7; init-published 2 / 1; init-residue 8 / 5; acknowledged 3 / 6; with in-flight 12 / 9 | pass | `Workloads/CrashMatrixTest.EveryCrashPointRecoversUnderBothModels/Init` |
| WalAppend | puts, overwrite, delete, empty value | 21 | 42 | acknowledged 5 / 9; with in-flight 16 / 12 | pass | `.../WalAppend` |
| Flush | three flushes (SST with filter, WAL publication, manifest, cleanup) | 76 | 152 | acknowledged 64 / 67; with in-flight 12 / 9 | pass | `.../Flush` |
| Compaction | five flush+compaction rounds, ending with an empty output and a new write | 203 | 406 | acknowledged 183 / 188; with in-flight 20 / 15 | pass | `.../Compaction` |
| TailRepair | torn tail truncate and sync, Open-time directory sync, orphan and stale MANIFEST.tmp removal, appends | 55 | 110 | acknowledged 47 / 49; with in-flight 8 / 6 | pass | `.../TailRepair` |
| EmptyKey | the empty key through overwrite, delete, empty value, flush, and compaction (trigger 2) | 194 | 388 | acknowledged 174 / 179; with in-flight 20 / 15 | pass | `.../EmptyKey` |
| MultiOutput | one block per output: compaction into a multi-SST base run (every output's sync and rename, one directory sync, one manifest), flushes above it, empty key, delete, overwrite | 216 | 432 | acknowledged 200 / 204; with in-flight 16 / 12 | pass | `.../MultiOutput` |
| Total | | 793 | 1586 | | pass | |

Crash points are completed state-changing calls (open that creates or
truncates, write, ftruncate, fsync, rename, unlink, mkdir) and the named
checkpoints `write.applied`, `flush.installed`, `compaction.installed`,
`response.before`, and `response.after`. The test asserts that the
named checkpoints and the expected publication and cleanup calls occur in
each workload's enumeration.

## 3. Mutation check

| Variant | Model | Expected | Observed | Test |
|---|---|---|---|---|
| Normal control: Flush (76 points), MultiOutput (216 points) | both | no violation | 0 and 0 | `Workloads/CrashMutationTest.SkippedSstContentSyncIsCaughtOnlyUnderUnsyncedLoss/{Flush,MultiOutput}` |
| SST content fsync skipped (`.sst.tmp`) | process | no violation (completed writes survive) | 0 and 0 | same |
| SST content fsync skipped | unsynced-loss | violations once a manifest naming the SST is durable; for MultiOutput, also on a compaction output | Flush: 59, the first at point 18 (the manifest's directory fsync), `sst-000002.sst: size 0 differs from the expected 128`. MultiOutput: 199, the first at point 18 (a flushed SST, `sst-000004.sst`); the test also requires a violation that names the compaction output `sst-000006.sst` | same |

This is a planted mutation that checks the harness; it is not a defect
found in the engine.

## 4. Differential workloads

All seeds 1-20 at 5000 operations, with a reopen every 500 operations
where periodic. All `db_dir` paths were on ext4. Filters are on (10 bits
per key) in every DB workload except where stated.

| Workload | Opens | Flushes per seed | Compactions per seed | Largest base run | Observed | Test |
|---|---:|---:|---:|---:|---|---|
| Memtable | - | - | - | - | pass | `Seeds/DifferentialTest.Memtable/*` |
| DbWithoutReopen / DbPeriodicReopen (4 MiB memtable) | 1 / 11 | 0 | 0 | 0 | pass | `.../DbWithoutReopen/*`, `.../DbPeriodicReopen/*` |
| DbFlushWithoutReopen / DbFlushPeriodicReopen (4 KiB, compaction off) | 1 / 11 | 35-50 | 0 | 0 | pass | `.../DbFlush*/*` |
| DbMixedFilterPeriodicReopen (as DbFlushPeriodicReopen, filters off on every other open) | 11 | 35-50 | 0 | 0; live SSTs at the end: 14-27 version 1 and 15-26 version 2 per seed | pass | `.../DbMixedFilterPeriodicReopen/*` |
| DbCompactionPeriodicReopen (4 KiB, trigger 2) | 11 | 35-50 | 34-49 | 1 | pass | `.../DbCompactionPeriodicReopen/*` |
| DbMultiOutputCompactionPeriodicReopen (4 KiB, trigger 2, at most 3 blocks per SST, values padded by 600 bytes) | 11 | 333-356 | 332-355 | 2-3 | pass | `.../DbMultiOutputCompactionPeriodicReopen/*` |

Every compaction workload asserts compactions <= flushes. An unchanged
base run, however many SSTs it has, is never merged again before a new
flush, including across reopens. WAL truncation cuts: 206.

## 5. Untested or out of scope

- Real power loss, device write caches, writeback reordering inside a
  file, torn sector writes, and media errors. The simulator models only
  the two crash models above.
- Crash points outside the seven enumerated workloads, including crashes
  during a crash recovery, long or randomized crash workloads, and
  crashes combined with injected errors.
- Cross-directory renames, symbolic links, and FIFOs in the simulator
  (the engine never renames across directories; non-regular entries are
  covered on the real filesystem only).
- Exceptions thrown from memtable Apply (M1 limitation), and
  cross-process lock behavior (only same-process Busy is tested).
- A real writeback error that a later fsync hides (design.md section 5);
  reopen validates only what the filesystem returns.
- The SST, file-ID, and manifest-entry limits at their real sizes
  (tested with lowered internal limits and crafted manifests only). A
  flush larger than one SST is still rejected (the M2 B-006 case).
- Migration of M2 databases: deliberately not supported (manifest
  version 2 only).
- Compaction with a very large number of inputs (descriptor and memory
  use grow with the input count).
- Concurrency (the engine is single-threaded by contract).
- The filter key limit (2^22 keys per SST) at its real size, and filter
  memory at scale; the limit is tested with a lowered builder limit.
- False-positive rates for key distributions other than the generated
  test keys.
- Cold-cache and device-level I/O: the benchmarks count engine requests
  with a warm page cache.
