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
