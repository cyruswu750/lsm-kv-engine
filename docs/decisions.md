# Design Decisions

Record decisions that affect correctness, durability, architecture,
or measurable performance. Update validation status as tests are added.

## D-001: Publish new WAL files through a temporary file
Date: 2026-10-28

- Decision: Write the header to `000001.wal.tmp`, fsync the file,
  rename it to `000001.wal`, then fsync the database directory.
  Do not write the initial header directly to the final WAL path.
- Reason: An interrupted initialization should not leave a partially
  written header at the final WAL path, where it would be treated as
  database corruption.
- Validation: Planned — interrupt initialization before and after
  publication. Verify that reopening either safely completes
  initialization or opens a valid WAL.

## D-002: Require recovery after a WAL sync failure
Date: 2026-10-28

- Decision: On a WAL sync failure, return `IOError` and move the
  database handle to `NeedsRecovery`. Do not retry and treat a later
  successful sync as proof that the original write is durable.
- Reason: The persistence outcome is uncertain. Continuing with the
  current in-memory state could hide a mismatch with the WAL.
  Reopening must validate and replay the available log.
- Validation: Planned — inject a sync failure and verify that later
  operations are rejected until the database is reopened.

## D-003: Recognize interrupted initialization
Date: 2026-10-28

- Decision: For M1, a directory containing only `LOCK` and the
  recognized initialization file `000001.wal.tmp` may be initialized
  when `create_if_missing` is true, after acquiring the exclusive lock.
  Unrelated files are not ignored, and an invalid final WAL is not
  silently replaced.
- Reason: A crash between lock-file creation and WAL publication
  should not permanently prevent initialization.
- Validation: Planned — test lock-only and temporary-WAL-only
  initialization states, unrelated files, an invalid final WAL,
  and rejection of a second concurrent opener.