# Bug Log

Record significant bugs found during development, especially those
affecting correctness, recovery, or data integrity.

## B-001: DecodeFixed32/64 mis-decode bytes >= 0x80

- Symptom: DecodeFixed32("\x80\x00\x00\x00") returned 0xFFFFFF80 instead of 0x80;
  DecodeFixed64 aborted under UBSan ("shift exponent 32 is too large for 32-bit type 'int'").
- Found by: Coding golden-byte/round-trip tests (32-bit); UBSan (64-bit).
- Root cause: 32-bit cast signed `char` directly to uint32_t (sign extension);
  64-bit shifted as `int` before widening (UB for shifts >= 32).
- Fix: char -> unsigned char -> uintN_t, then shift. Commit 0bfc9cb.
- Regression test: Coding.DecodeFixed32FromSpecifiedBytes, Coding.DecodeFixed64FromSpecifiedBytes.

## B-002: DecodeWalRecord reports invalid tail records as Incomplete

- Symptom: A record with a valid header CRC but an invalid field (key/value length
  over limit, Delete with value_len != 0, bad sequence) and a truncated payload
  decoded as Incomplete instead of Corruption.
- Found by: WalRecordMalformedTest.OversizedLengths,
  WalRecordMalformedTest.InvalidFieldsWithMissingPayloadAreCorruption.
- Root cause: The "remaining < total" check ran before field validation, so a
  missing payload masked invalid fields. Recovery would treat such a record as a
  torn tail and silently truncate it instead of failing closed.
- Fix: Validate all header fields before the length check. Commit 8bddb8c.
- Regression test: the two tests above.

## B-003: Writable-file factories accepted embedded NUL paths 

- Symptom: A path containing `abc\0suffix` was accepted.
  NewWritableFile created `abc`; OpenAppendableFile returned a writable
  handle to an existing `abc`. Found while reviewing path validation
  during the directory-operations task.

- Cause: OpenPosixFile passed the path to open without checking for
  embedded NUL bytes. The syscall interpreted only the prefix before NUL.

- Fix: Reuse CheckSyscallPath before allocation and open in
  src/core/posix_file.cc. Invalid paths return InvalidArgument without
  opening the prefix path. Empty paths are also rejected consistently.

- Verification: CreateNewRejectsEmbeddedNulPath and
  OpenAppendableRejectsEmbeddedNulPath both failed before the fix and
  passed afterward. The full suite passed 151/151 under ASan/UBSan.
  Regression commit: 19b3cf3. Fix commit: cca0dd1.

## B-004: A stale MANIFEST.tmp that Open could not remove blocked every flush

- Symptom: If Open's best-effort orphan cleanup failed to unlink a leftover
  `MANIFEST.tmp` (for example, EACCES), every later flush failed at the
  exclusive create of `MANIFEST.tmp` with IOError. The handle then moved to
  NeedsRecovery, and each reopen repeated the cycle while the unlink kept
  failing.
- Found by: Code review of the uncommitted M2 publication path, before the
  M2 code was committed. Confirmed by a regression test.
- Cause: Publication creates `MANIFEST.tmp` with O_EXCL. The deferred-orphan
  retry ran only in cleanup (F7), after that create.
- Fix: Before writing `MANIFEST.tmp` (F4), retry the removal of a deferred
  `MANIFEST.tmp`. If the retry fails, it is an ordinary publication failure
  (src/core/db.cc, `DB::Impl::PublishFlush`).
- Verification: `DBM2Test.StaleManifestTempThatOpenCouldNotRemoveDoesNotBlockFlush`
  failed at the triggering `Put` when built against the pre-fix `db.cc`, and
  passes with the fix. The unlink failure is injected through the syscall
  seam; it was not observed on a real filesystem.
- Commits: the fix and the regression test were committed together with
  the M2 integration in f7e4fff. There is no separate pre-fix commit; the
  pre-fix failure was observed in a local development build.

## B-005: Open retired files and acknowledged writes on a manifest not known to be durable

- Symptom: After a flush renamed `MANIFEST.tmp` over `MANIFEST` but its
  directory fsync failed (handle invalidated) or never ran (crash), the
  next Open read the new generation and immediately unlinked the old WAL
  and other covered orphans. It then acknowledged new writes, all without
  any directory sync. Under power loss the directory could revert to the
  older generation, whose WAL had already been deleted.
- Found by: Review of M2 recovery ordering against design.md section 8
  step 4, before the M2 code was committed.
- Cause: Recovery trusted the visible MANIFEST as durable. Only the
  publication path synced the directory after a manifest rename.
- Fix: `DB::Impl::Recover` (src/core/db.cc) fsyncs the database directory
  after validation and replay, and before `RemoveOrphansAtOpen` and before
  the handle is exposed. A failure returns IOError and removes nothing
  (D-016).
- Verification: `UnsyncedManifestTest.OpenSyncsDirectoryBeforeRetiringOrAcknowledging`
  failed ("Open never synced the directory") and
  `UnsyncedManifestTest.FailedOpenDirectorySyncRetiresNothing` failed
  (the old WAL was unlinked and a handle was returned) before the fix;
  both pass after it. The failed sync is injected through the syscall
  seam. Power loss itself is not simulated in M2.
- Commits: the fix is part of the M2 integration commit f7e4fff, and the
  regression tests are in ec97213. There is no separate pre-fix commit;
  the pre-fix failures were observed in a local development build.

## B-006: SST Open allocated a footer-declared index of any size that fit the file

- Symptom: `SstTable::Open` allocated and read the whole `index_length`
  from the footer after checking only that the index fit inside the file.
  A malformed sparse SST therefore caused a large allocation before its
  impossible index was rejected. Reproduction against the pre-fix source:
  a valid 25-byte data block, `index_length` 256 MiB, a correctly
  checksummed v1 footer, and a matching file size (268,435,529 bytes,
  8 KiB allocated on disk). Opened in a separate non-sanitizer process
  under a 128 MiB `RLIMIT_AS`, Open threw `std::bad_alloc` instead of
  returning Corruption.
- Found by: Review of the uncommitted M2 SST reader, before the M2 code
  was committed.
- Cause: The only bound on the index allocation was the file size, and a
  sparse file makes a large declared size cheap. No format limit existed
  on the index size or block count, so the writer had no matching limit
  either.
- Fix: New format limits `kMaxSstBlocks` (1,048,576) and
  `kMaxSstIndexSize` (64 MiB, trailer included) in src/core/sst.h. The
  reader (`DecodeFooter`, src/core/sst.cc) rejects an index_length above
  min(floor(index_offset / 25), 2^20) x (16 + 65,536) + 8 or above 64 MiB
  before allocating, and `DecodeIndex` rejects a block_count above the
  limit. `SstBuilder::FlushBlock` checks both limits before writing each
  block and returns InvalidArgument. Recorded in D-010.
- Verification: `SstTest.ImpossibleIndexLengthIsRejectedBeforeTheIndexIsRead`
  failed before the fix (Open requested 268,435,504 pread bytes for the
  256 MiB case, and 65,609 for the bound plus one, instead of the 48-byte
  footer, and reported "index checksum mismatch").
  `SstTest.IndexLengthAboveTheFormatLimitIsRejectedBeforeTheIndexIsRead`
  failed the same way (67,108,913 bytes), and
  `SstTest.IndexBlockCountAboveTheLimitIsCorruption` failed with "extends
  past the index". All three pass after the fix; the tests count requested
  pread bytes through the syscall seam and never allocate the declared
  size. The `RLIMIT_AS` reproduction, rebuilt against the fixed source,
  returns Corruption ("index length 268435456 exceeds the limit 65560 for
  a data region of 25 bytes"). Writer boundaries are tested with lowered
  limits.
- Consequence: a flush whose output would exceed a limit fails. The
  default memtable_bytes cannot reach either limit; a much larger one with
  long keys can, and the database then refuses mutations that need a
  flush without losing acknowledged data (D-010, docs/sst-format.md
  section 3.6).
- Commits: the limits, the fix, and the regression tests were committed
  together with the SST module in 855168d, and the format text in
  0d48e5c. There is no separate pre-fix commit.
