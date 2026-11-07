# Bug Log

Defects found during development that affected correctness, recovery, or data integrity. Each entry
uses the same fields: symptom, impact, root cause, how it was found, the fix, and the regression tests
that now catch it. Test names use the current suite names. Commit IDs are on `main`.

| ID | Summary | Area | Fix commit |
|---|---|---|---|
| [B-005](#b-005-open-retired-files-and-acknowledged-writes-on-a-manifest-not-known-to-be-durable) | Open trusted a manifest whose directory sync never completed | recovery ordering | f7e4fff |
| [B-006](#b-006-sst-open-allocated-a-footer-declared-index-of-any-size-that-fit-the-file) | A crafted footer caused an unbounded allocation | SST validation | 855168d |
| [B-002](#b-002-decodewalrecord-reported-invalid-tail-records-as-incomplete) | Invalid WAL records were treated as a torn tail | WAL recovery | 8bddb8c |
| [B-004](#b-004-a-stale-manifesttmp-that-open-could-not-remove-blocked-every-flush) | A stale `MANIFEST.tmp` blocked every flush | publication | f7e4fff |
| [B-007](#b-007-the-compaction-merge-missed-duplicate-sequences-behind-a-newer-version) | Merge missed a duplicate-sequence conflict | compaction | 53e53ed |
| [B-003](#b-003-writable-file-factories-accepted-embedded-nul-paths) | Paths with an embedded NUL opened a different file | file I/O | cca0dd1 |
| [B-001](#b-001-decodefixed3264-mis-decoded-bytes--0x80) | Fixed-width decoding sign-extended bytes | encoding | 0bfc9cb |

## B-005: Open retired files and acknowledged writes on a manifest not known to be durable
- **Symptom:** after a flush renamed `MANIFEST.tmp` over `MANIFEST` but its directory fsync failed
  (handle invalidated) or never ran (crash), the next Open read the new generation, immediately unlinked
  the old WAL and other covered orphans, and acknowledged new writes, all without any directory sync.
- **Impact:** under power loss the directory could revert to the older generation, whose WAL had already
  been deleted: acknowledged writes lost.
- **Root cause:** recovery trusted the visible MANIFEST as durable; only the publication path synced the
  directory after a manifest rename.
- **Found by:** review of the recovery ordering against design.md section 8, step 4, before the code was
  committed.
- **Fix:** `DB::Impl::Recover` (`src/core/db.cc`) fsyncs the database directory after validation and
  replay, before `RemoveOrphansAtOpen` and before exposing the handle. A failure returns IOError and
  removes nothing (D-016).
- **Regression tests:** `UnsyncedManifestTest.OpenSyncsDirectoryBeforeRetiringOrAcknowledging` and
  `UnsyncedManifestTest.FailedOpenDirectorySyncRetiresNothing` (both failed before the fix; the failed
  sync is injected through the syscall seam). The crash matrix's TailRepair workload, added later, also
  crashes at the Open-time directory sync under both crash models (failure-matrix.md section 2).
- **Commits:** fix in f7e4fff, tests in ec97213; the pre-fix failure was observed in a local build.

## B-006: SST Open allocated a footer-declared index of any size that fit the file
- **Symptom:** `SstTable::Open` read the whole footer-declared `index_length` after checking only that it
  fit inside the file. A sparse 268,435,529-byte file (8 KiB on disk) with a 256 MiB declared index made
  Open throw `std::bad_alloc` under a 128 MiB `RLIMIT_AS` instead of returning Corruption.
- **Impact:** a malformed file could exhaust memory before being rejected.
- **Root cause:** the file size was the only bound on the allocation, and a sparse file makes a large
  declared size cheap; the format had no limit on index size or block count.
- **Found by:** review of the SST reader before the code was committed.
- **Fix:** format limits `kMaxSstBlocks` (1,048,576) and `kMaxSstIndexSize` (64 MiB). `DecodeFooter`
  rejects an impossible `index_length` before allocating, `DecodeIndex` rejects an oversized block count,
  and `SstBuilder` enforces both limits when writing (D-010, sst-format.md section 3.6).
- **Regression tests:** `SstTest.ImpossibleIndexLengthIsRejectedBeforeTheIndexIsRead`,
  `SstTest.IndexLengthAboveTheFormatLimitIsRejectedBeforeTheIndexIsRead`,
  `SstTest.IndexBlockCountAboveTheLimitIsCorruption`. They count requested `pread` bytes through the
  syscall seam, so they never allocate the declared size.
- **Consequence:** a flush whose output would exceed a limit fails without losing acknowledged data. The
  default `memtable_bytes` cannot reach the limits.
- **Commits:** 855168d (with the SST module), format text in 0d48e5c.

## B-002: DecodeWalRecord reported invalid tail records as Incomplete
- **Symptom:** a record with a valid header CRC but an invalid field (an over-limit length, a Delete with
  a value, a bad sequence) and a truncated payload decoded as Incomplete instead of Corruption.
- **Impact:** recovery would have treated a corrupt record as a torn tail and silently truncated it
  instead of failing closed.
- **Root cause:** the "remaining < total" check ran before field validation, so a missing payload masked
  invalid fields.
- **Found by:** `WalRecordMalformedTest.OversizedLengths` and
  `WalRecordMalformedTest.InvalidFieldsWithMissingPayloadAreCorruption`.
- **Fix:** validate every header field before the length check.
- **Regression tests:** the two tests above.
- **Commits:** 8bddb8c.

## B-004: A stale MANIFEST.tmp that Open could not remove blocked every flush
- **Symptom:** if Open's best-effort cleanup could not unlink a leftover `MANIFEST.tmp` (for example
  EACCES), every later flush failed at the exclusive create of `MANIFEST.tmp`, moved the handle to
  NeedsRecovery, and each reopen repeated the cycle.
- **Impact:** a permanently unwritable database while the unlink kept failing; no data loss.
- **Root cause:** publication creates `MANIFEST.tmp` with `O_EXCL`, but the deferred-orphan retry ran only
  in cleanup (F7), after that create.
- **Found by:** review of the publication path before the code was committed.
- **Fix:** retry the removal of a deferred `MANIFEST.tmp` before writing it (F4); a failure there is an
  ordinary publication failure (`DB::Impl::PublishFlush`).
- **Regression tests:** `DBM2Test.StaleManifestTempThatOpenCouldNotRemoveDoesNotBlockFlush` (failed
  against the pre-fix code; the unlink failure is injected through the syscall seam).
- **Commits:** f7e4fff (fix and test together).

## B-007: The compaction merge missed duplicate sequences behind a newer version
- **Symptom:** for one key with sequences 10, 5 and 5 (two different values at 5), input orders
  [10, 5, 5] and [5, 10, 5] returned the sequence-10 version without reporting the conflict.
- **Impact:** a missed Corruption report; the winner was still correct and no data was lost. The input
  is very hard to reach through the engine, because Open checks every SST's sequence range and flush runs
  have disjoint, ordered intervals.
- **Root cause:** each version was compared only with the current winner, so once the newer version won,
  the two equal older sequences were never compared with each other.
- **Found by:** review of the merge before the code was committed.
- **Fix:** every pair of versions of the current key is compared before the winner is chosen
  (`src/core/compaction.cc`), independent of input order.
- **Regression tests:** `CompactionInputTest.MergeRejectsDuplicateSequencesBehindANewerVersion` (all six
  input orders; 4 of 6 returned Ok before the fix).
- **Commits:** 53e53ed (fix and test together).

## B-003: Writable-file factories accepted embedded NUL paths
- **Symptom:** a path `abc\0suffix` was accepted: `NewWritableFile` created `abc`, and
  `OpenAppendableFile` returned a writable handle to an existing `abc`.
- **Impact:** a caller-supplied path could open a different file than the one named.
- **Root cause:** `OpenPosixFile` passed the path to `open` without checking for embedded NUL bytes, so
  the syscall used only the prefix.
- **Found by:** review of path validation while adding the directory operations.
- **Fix:** reuse `CheckSyscallPath` before allocating and opening (`src/core/posix_file.cc`); invalid and
  empty paths return InvalidArgument.
- **Regression tests:** `CreateNewRejectsEmbeddedNulPath`, `OpenAppendableRejectsEmbeddedNulPath` (both
  failed before the fix).
- **Commits:** tests in 19b3cf3, fix in cca0dd1.

## B-001: DecodeFixed32/64 mis-decoded bytes >= 0x80
- **Symptom:** `DecodeFixed32("\x80\x00\x00\x00")` returned 0xFFFFFF80 instead of 0x80;
  `DecodeFixed64` aborted under UBSan ("shift exponent 32 is too large for 32-bit type 'int'").
- **Impact:** wrong lengths and checksums for any encoded byte with the high bit set.
- **Root cause:** the 32-bit path cast signed `char` directly to `uint32_t` (sign extension); the 64-bit
  path shifted as `int` before widening (undefined for shifts of 32 or more).
- **Found by:** golden-byte and round-trip coding tests (32-bit) and UBSan (64-bit).
- **Fix:** `char` → `unsigned char` → `uintN_t`, then shift.
- **Regression tests:** `Coding.DecodeFixed32FromSpecifiedBytes`, `Coding.DecodeFixed64FromSpecifiedBytes`.
- **Commits:** 0bfc9cb.
