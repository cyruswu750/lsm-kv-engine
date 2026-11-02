# Bug Log

Record significant bugs found during development, especially those
affecting correctness, recovery, or data integrity.

## B-001: DecodeFixed32/64 mis-decode bytes >= 0x80 (2026-10-31)

- Symptom: DecodeFixed32("\x80\x00\x00\x00") returned 0xFFFFFF80 instead of 0x80;
  DecodeFixed64 aborted under UBSan ("shift exponent 32 is too large for 32-bit type 'int'").
- Found by: Coding golden-byte/round-trip tests (32-bit); UBSan (64-bit).
- Root cause: 32-bit cast signed `char` directly to uint32_t (sign extension);
  64-bit shifted as `int` before widening (UB for shifts >= 32).
- Fix: char -> unsigned char -> uintN_t, then shift. Commit 0bfc9cb.
- Regression test: Coding.DecodeFixed32FromSpecifiedBytes, Coding.DecodeFixed64FromSpecifiedBytes.
- Lesson: Widen bytes through unsigned char before shifting.

## B-002: DecodeWalRecord reports invalid tail records as Incomplete (2026-11-01)

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
- Lesson: Validate everything the header claims before deciding a record is just short.

