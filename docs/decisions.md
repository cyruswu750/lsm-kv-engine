# Design Decisions

Record decisions that affect correctness, durability, architecture,
or measurable performance. Update validation status as tests are added.

## D-00X: Title
Date: YYYY-MM-DD

- Decision:
- Alternatives: What else was viable, and why rejected.
- Reason:
- Cost: What this choice gives up.
- Validation: Test name (or "Planned: ...").

## D-001: Publish new WAL files through a temporary file
Date: 2026-10-28

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
Date: 2026-10-28

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
Date: 2026-10-28

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
Date: 2026-11-01

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
  A forgotten seam argument silently falls back to real I/O.
- Validation: `DBFaultTest.*` and `TornTail.*`.

## D-005: Classify database entries with lstat and scan twice
Date: 2026-11-01

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
Date: 2026-11-01

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
Date: 2026-11-01

- Decision: Recovery reads the complete WAL with `ReadFileToString`,
  then decodes records from that buffer.
- Alternatives: A streaming reader with a bounded buffer.
- Reason: The decoder works on one validated span and copies retained
  bytes into the memtable, which keeps M1 recovery simple.
- Cost: Peak recovery memory is about twice the WAL size (buffer plus
  memtable copies). Recovery is not bounded-memory for arbitrarily large
  WALs; M2 flush and WAL retirement are expected to bound WAL size.
- Validation: None; this is a documented limitation.
