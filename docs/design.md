# C++20 LSM KV Engine — Design Contract

Status: proposed design, not an implementation report. Initial scope: one process, one calling thread, Linux/WSL2. Repository destination: docs/design.md.

## 1. Purpose and engineering questions

Build an embedded persistent byte-key/byte-value store without using LevelDB or RocksDB as the storage implementation. The project investigates (1) recovery of acknowledged operations across explicitly tested failures, (2) safe publication of new on-disk state, and (3) the measurable costs of durability, compaction, and negative lookups. Standard LSM techniques are prior art, not claimed inventions.

The distinguishing deliverables are an executable failure matrix, randomized reference-model tests, reproducible benchmark scripts, and design decisions supported by observed results. No production-readiness, hardware power-loss validation, or performance numbers are claimed before verification.

## 2. Scope and milestones

- M1: ordered memtable, versioned checksummed WAL, synchronous Put/Delete, Get, reopen/replay, malformed-input tests, differential tests.
- M2: immutable SSTables, sparse block index, synchronous flush, manifest publication and WAL retirement, publication failure tests.
- M3: synchronous full-merge compaction, deterministic crash simulation under process-crash and power-loss models, compaction publication failure tests.
- M4: Bloom filters, benchmark ablations, recovery report, documented limitations and README.

No transactions, snapshots, range API, concurrent calls, background workers, replication, SQL, compression, or network service in the baseline. A directory lock rejects a second opener. Later work must not obscure correctness of M1. Compaction merges all live SSTs (§9); leveled compaction is out of scope and must not be claimed.

## 3. Public API (C++20 declarations)

```cpp
enum class Code {
  Ok, NotFound, InvalidArgument, IOError, Corruption,
  UnsupportedFormat, Busy, NeedsRecovery
};

struct Status {
  Code code;
  std::string message;
  bool ok() const { return code == Code::Ok; }
};

struct Options {
  bool create_if_missing = true;
  std::size_t memtable_bytes = 4 * 1024 * 1024;
};

class DB {
 public:
  static Status Open(const std::filesystem::path& directory,
                     const Options& options,
                     std::unique_ptr<DB>& out);
  Status Put(std::string_view key, std::string_view value);
  Status Get(std::string_view key, std::string& out_value) const;
  Status Delete(std::string_view key);
  ~DB();
  DB(const DB&) = delete;
  DB& operator=(const DB&) = delete;
};
```

These declarations summarize the public API, not a complete compilable header. The repository headers remain authoritative for current signatures. Required standard headers include string, string_view, filesystem, memory, and cstddef. C++20 has no std::expected; explicit Status plus output parameters avoids introducing a custom generic result type now.

- Open requires an empty out pointer; a nonempty pointer returns InvalidArgument without replacement. On other failure out remains empty. A DB is exposed only after validation/recovery succeeds. Missing database plus create_if_missing=false returns NotFound. Existing unrelated/nonempty directories are not silently initialized. Initial durable directory creation includes the required parent-directory syncs.
- Keys and values are byte strings: embedded zero bytes and empty strings are valid. Max key length is 64 KiB; max value length is 16 MiB. Reject oversized input before I/O. memtable_bytes must be positive; it is a soft flush threshold, not a strict RSS cap.
- Put replaces the current value. Get returns an owning copy; on any non-Ok result it clears out_value. Thus an empty value is distinct from NotFound. Invalid key length returns InvalidArgument.
- Delete is idempotent: deleting an absent key returns Ok after recording a durable tombstone. Get must not reveal an older value afterward.
- string_view input is borrowed only for the call. The engine copies retained bytes; it never stores views into caller memory. Inputs must not alias mutable output arguments.
- Expected validation, format, and I/O failures use Status. Allocation failure is not promised to be recoverable; if an exception occurs after persistence begins, invalidate the handle before propagating. No further calls on that handle except destruction.
- Destructor releases resources and never throws. Successful mutation durability must not depend on destruction or clean shutdown.

## 4. State and ordering

Use std::map<std::string, Entry, BytewiseLess> initially. Define BytewiseLess as lexicographic unsigned-byte order. Entry owns a value or tombstone and a uint64_t sequence number. std::map supplies an ordered container; implementing a balanced tree is outside this project's purpose.

Sequence numbers begin at 1 and increase for each accepted mutation; reject overflow. They order versions, not keys. Replay is in log order. After M2, next sequence exceeds both the manifest checkpoint and all replayed records. Cross-layer lookup chooses the greatest sequence for a key; a winning tombstone means NotFound. Within one WAL, sequence numbers must be nonzero and strictly increasing; duplicates or decreases are Corruption. Gaps are allowed. Any tolerance for identical versions across WALs and SSTables is an M2 recovery rule, not permission for duplicate records within one WAL. Conflicting versions with the same sequence are Corruption. Early termination is allowed only when ordering invariants prove that no unsearched source can contain a newer version of the same key. File creation time alone is not such a proof. Store the last used sequence, not an unchecked next-sequence expression. Recovery at UINT64_MAX succeeds, but subsequent mutations return InvalidArgument before maintenance or mutation I/O.

## 5. Durability contract

Baseline mutations are synchronous. Validate and encode; append the complete WAL record; successfully fsync the WAL; install the entry in memory; then return Ok. No caller may observe an intermediate state because calls are single-threaded. Prepare required memory before appending when practical. Any failure after append begins makes the handle require recovery, including Get, to avoid serving an in-memory state inconsistent with durable state.

write success is not the persistence boundary. Loop for short writes; retry EINTR; handle zero progress and other errors as failure. A successful fsync is the persistence boundary under the filesystem/device contract. For newly created files, directory entries must also be durably published before acknowledging operations that depend on them.

If Put/Delete returns Ok, recovery must preserve its effect unless a later recovered mutation supersedes it. An operation interrupted before its response, or returning an I/O error after persistence began, has an uncertain outcome: it may survive completely or be absent. It must never produce a partially decoded key/value. An error is not a rollback guarantee.

After WAL append/sync failure return IOError and enter NeedsRecovery. Never retry fsync after it fails: on Linux, dirty pages whose writeback failed may already be marked clean, so a later successful fsync does not prove the data reached storage. The handle enters NeedsRecovery immediately. Subsequent calls return NeedsRecovery. Reopen validates the bytes visible through the filesystem; it does not prove that a prior writeback failure has been repaired. Successful reopen alone does not restore a durability guarantee after a real storage writeback error. Recovery from such failures is outside the baseline guarantee. Do not continue appending past a failed partial record.

Process termination, malformed-file injection, and device power loss are different models. SIGKILL does not clear the OS page cache. Claims are limited to tested failures; WSL2 results also depend on host storage semantics.

## 6. WAL v1 exact encoding (M1)

All integers are unsigned little-endian. Serialize fields explicitly; never dump C++ structs, pointers, padding, or native size_t. There is no record padding or block fragmentation in v1.

File header: 8 bytes magic `KVWAL001`, uint32 version=1, uint32 reserved=0: 16 bytes total. A new header must be written and synced before this WAL accepts mutations. For a new database, the WAL is created through a temporary file: write the 16-byte header to `000001.wal.tmp`, fsync it, rename it to `000001.wal`, then fsync the directory. Without this, a crash during creation could leave a partial header, and because a bad header fails Open, the database could never be opened again. For M1, Open recognizes only `000001.wal.tmp` as an initialization temporary file. After acquiring the exclusive lock and validating the directory state, Open may remove this file before completing initialization or opening an existing valid WAL. It must not remove unrelated files merely because their names end in `.tmp`. An existing truncated/bad header fails Open rather than being silently replaced. Later WAL files are published through the manifest before use.

Each record starts with the following 28-byte header:

| Offset | Bytes | Field | Meaning |
|---:|---:|---|---|
| 0 | 4 | header_crc32c | CRC32C of header bytes [4, 28) |
| 4 | 4 | payload_crc32c | CRC32C of key bytes followed by value bytes |
| 8 | 8 | sequence | Monotonic mutation number |
| 16 | 4 | key_len | Key byte length |
| 20 | 4 | value_len | Value byte length; zero for Delete |
| 24 | 1 | type | 1=Put, 2=Delete |
| 25 | 3 | reserved | All zero |
| 28 | key_len | key | Raw bytes |
| 28+key_len | value_len | value | Raw bytes, absent for Delete |

Total size = 28 + key_len + value_len, calculated using checked wide arithmetic. A separate total-length field would be redundant. Header checksum protects lengths before they are trusted. Payload checksum checks contents. CRC detects accidental errors; it neither repairs data nor provides cryptographic integrity.

Use standard CRC-32C/Castagnoli: reflected polynomial 0x82F63B78, initial state 0xffffffff, final XOR 0xffffffff, no LevelDB masking. Check vector: ASCII `123456789` => 0xe3069283; empty bytes => 0. One precisely tested implementation/helper is sufficient. This format is intentionally not LevelDB compatible.

Example: Put("a", "hi") has key_len=1, value_len=2, type=1 and total size=31 bytes. Delete("a") has key_len=1, value_len=0, type=2 and total size=29 bytes. Empty Put and Delete remain distinct because of type.

M1 directory layout:
- `LOCK`: held with flock(LOCK_EX | LOCK_NB) for the life of the handle; contention returns Busy. Its contents are unused.
- `000001.wal`: the only WAL in M1. From M2 the manifest names the active WAL.
- `000001.wal.tmp`: a non-authoritative initialization file. It may be cleaned up only after acquiring the exclusive lock and validating the directory state. Other `.tmp` files are not treated as engine-owned files.

For M1, the presence of `000001.wal` indicates an existing database, but Open must still validate its header and records. An invalid final WAL must not be silently replaced.

If the directory does not exist, create_if_missing=false returns NotFound without creating anything; otherwise, Open creates it and fsyncs its parent.

If `000001.wal` is absent and the directory contains only `LOCK`, `000001.wal.tmp`, or neither, it is considered uninitialized. With create_if_missing=true, Open acquires the exclusive lock, removes any leftover `000001.wal.tmp`, and creates the WAL using the publication procedure above. With create_if_missing=false, it returns NotFound without initializing the database or removing the temporary file.

If `000001.wal` is absent and any unrelated entry is present, Open returns InvalidArgument without initializing the database or deleting unrelated files.

## 7. Recovery rules (M1)

Acquire exclusive directory lock, validate file header, then scan from byte 16. Keep last_good_offset and last sequence. Before allocation validate header CRC, reserved fields, type, lengths, overflow, and sequence order. Apply an entry only after its full payload passes checksum.

Validate the file header in this order:
1. Fewer than 16 bytes: Corruption.
2. Incorrect magic: Corruption.
3. Unsupported version: UnsupportedFormat.
4. For version 1, nonzero reserved bytes: Corruption.

The file header has no checksum. A corrupted version field may
therefore be reported as UnsupportedFormat.

| Observation | Policy |
|---|---|
| EOF exactly after a valid record | Normal end |
| Fewer than 28 bytes remain for the next header | Incomplete trailing record |
| Valid header, valid bounded lengths, insufficient remaining payload | Incomplete trailing record |
| Full header with bad CRC, invalid type/length/reserved/sequence | Corruption; fail Open |
| Full payload with bad CRC, including the last record | Corruption; fail Open |
| Unsupported file version | UnsupportedFormat; no rewrite |

For an incomplete trailing record in the active WAL, truncate to last_good_offset and fsync before allowing another append. Test append-after-repair followed by another reopen. Do not scan ahead to guess the next record boundary. Inactive WALs referenced by a later manifest must be complete; an incomplete inactive WAL is corruption.

Tail truncation assumes an interrupted append, not arbitrary deletion of previously durable bytes. CRC failure is deliberately fail-closed even if a torn write could explain it: this may reduce availability, but avoids silently interpreting corruption as an unacknowledged tail. Diagnostics include file and offset. No automatic salvage mode in v1.

## 8. SSTables and publication (M2 design boundary)

Flush creates an immutable sorted table containing key, sequence, type, and value. Proposed table components: checksummed data blocks, sparse index mapping block boundary keys to offset/length, versioned footer locating the index and optional Bloom filter. A separate format spec with exact byte offsets and corruption rules is required before implementing SSTables; this document does not pretend those bytes are already settled.

Use synchronous flush before the next mutation when the nonempty memtable reaches or exceeds the soft threshold. Validate the incoming mutation and sequence availability before maintenance I/O. This avoids returning an ambiguous flush error for a mutation already acknowledged. Reads remain available between calls; no background tasks. Replay may temporarily exceed the threshold; bounded-memory recovery is out of scope.

The manifest is the authoritative snapshot of live SST IDs with their sequence ranges, the active WAL ID, checkpoint sequence, next file ID, format version, generation, and (from M3) how many leading SSTs form the base run (§9). It requires a versioned, checksummed encoding and validation before use. From M3 the engine reads and writes manifest version 2 only: an M2 database (manifest version 1) is UnsupportedFormat and is left unchanged, and there is no manifest migration or downgrade. Directory listing alone does not decide which files are live. A recognized M1 directory (one without MANIFEST that contains `000001.wal`, or a regular `000001.wal.tmp` left by interrupted M1 initialization) is UnsupportedFormat and is left unchanged; there is no migration, and M2 never removes M1 files. M2 uses a distinct numeric filename prefix (for example, `wal-000001.log` and `sst-000002.sst`), finalized in the format spec, so interrupted M2 initialization is not mistaken for M1. Only a missing directory or an empty/LOCK-only directory may be initialized. Without MANIFEST, any M2 initialization residue is Corruption and is preserved; automatic initialization repair is out of scope. A crash before initial publication may therefore require a fresh directory. No mutations may be acknowledged until initial publication is durable. Missing MANIFEST in an existing database must never trigger reconstruction from a directory listing. Unrelated entries without a recognized database are InvalidArgument. A malformed existing MANIFEST is never replaced automatically.

Publication protocol (one foreground operation at a time):
1. Keep the previous manifest and its referenced files intact.
2. Write a new SST to a unique temporary file; sync it; rename to a unique final name. Create and sync a new empty WAL through a temporary file and rename it. Then sync the directory once: no manifest refers to either file before this sync completes, so one sync makes both entries durable in the required order.
3. Write a new temporary manifest referencing the new SST and new WAL, with a checkpoint covering the flushed memtable; fsync the temporary manifest.
4. Rename over MANIFEST and fsync the directory. Until this completes, do not acknowledge new operations using the new generation.
5. Switch in-memory state; retire the previous WAL only after the new manifest is durable. Remove obsolete files and sync the directory for persistent cleanup.

If publication fails before the manifest directory sync succeeds, invalidate the handle and reopen. Prepare the in-memory installation before publication where practical; an exception preventing installation of the durable generation also invalidates the handle. Recovery accepts the old or new published state under the filesystem model; both must have their dependencies intact. Missing/corrupt referenced files cause failure. Unreferenced files are ignored, then cleaned only after a valid manifest is established and durable. Open therefore syncs the database directory after validation and replay and before it removes orphans or exposes a handle: the visible MANIFEST may come from a rename whose directory sync never completed, and step 4 forbids acknowledging operations on it until that sync has. Orphan removals at Open are not synced themselves; a reappearing orphan is still covered by the durable high-water mark and is removed again. A failed cleanup removal is not fatal and leaves garbage for a later attempt. After durable publication and successful in-memory installation, obsolete-file unlink failure alone does not invalidate the DB. A cleanup directory-sync failure remains IOError and invalidates the handle conservatively; no sync is retried after failure. These cases have separate tests. Never delete old files before publication. Allocate above both the manifest high-water mark and every recognized file ID observed on disk, including temporary and orphan files. Do not remove the last on-disk evidence of a higher orphan ID until a manifest carrying the raised high-water mark is durable; otherwise leave that orphan in place. This prevents reuse of published or observed persistent IDs across reopen. A never-published allocation lost entirely in a crash has no recoverable identity and is not covered by a stronger historical uniqueness claim. Check file-ID and generation exhaustion before publication; never wrap counters.

The exact format spec must define all widths, checksum coverage, reserved fields, legal region ordering, and allocation bounds. A 4 KiB data-block target is not a hard limit: a single legal large entry gets its own bounded oversized block. Keep the 64 KiB key and 16 MiB value API limits. SST/manifest truncation is Corruption, never a repairable tail; recognized unsupported versions are UnsupportedFormat. For this baseline, Open validates each referenced SST by streaming its blocks, including key ordering, index boundary keys, record fields, checksums, and exact sequence extrema against the manifest. Get validates the block it reads again. This adds startup I/O but keeps corruption rules and sequence-range validation explicit without loading whole SSTs.

All SST sequences are positive and at most the checkpoint. Live SSTs form runs: an optional base run written by the last compaction (SSTs ordered by key with strictly non-overlapping key ranges; their sequence intervals may overlap), followed by flush runs of one SST each. Every flush-run SST is newer than the whole base run, and flush-run sequence intervals are strictly ordered and disjoint. Active-WAL records must be strictly increasing and greater than the checkpoint. New flushes therefore form a newer interval; lookup may search memtable, then flush-run SSTs newest-first, then the one base SST whose key range contains the key, and stop on a value or tombstone. Missing referenced files fail Open. A read error or corruption that Get finds in an SST returns that status without invalidating the handle, because no durable or in-memory state changed; the write-failure rule of section 5 does not apply. Full compaction preserves this ordering because it replaces every live SST with one base run, older than every later flush. An empty SST is not published.

## 9. Compaction (M3) and Bloom filters (M4)

All SSTs form one level of runs (§8). Before the next mutation, after any pending flush, compaction runs synchronously when the logical run count reaches an internal threshold; the base run counts once however many SSTs it holds, so an unchanged base is not merged again before new flushes. It merges all live SSTs and keeps, per key, the version with the greatest sequence. Because every older version of every key is in the input set and the memtable and WAL hold only newer sequences, compaction drops shadowed versions and drops a winning tombstone together with its key. The output is one base run of zero or more SSTs, split only between keys so that each SST stays within the per-SST format limits; if nothing survives, the new manifest lists no SST. All outputs are synced, their names made durable, and published by one manifest. Publish through the same manifest protocol; the active WAL and checkpoint are unchanged. Compute each output's sequence extrema from its emitted records, not all input records; retain the checkpoint even if no output survives. Delete inputs only after the new manifest is durable.

This trades higher write amplification (each compaction rewrites all live data) for a simple coverage proof and, with compaction enabled at a fixed threshold, bounded run counts between maintenance calls (a lookup probes at most one SST per run). This is not a bound on total memory or data size. Measure that cost (§11). Partial compaction must keep tombstones: dropping one because its own inputs lack an older value is incorrect, since an unselected file may resurrect that value.

Bloom filters summarize all keys present in an SST, including tombstones. Negative means skip that SST; positive means inspect it, not that the key exists. False positives are allowed, false negatives are not. Version the stable hash/encoding in the SST format spec; never rely on unspecified std::hash persistence. The filter block is checksummed; corrupt filter metadata fails SST open with Corruption, never an unchecked negative. M4 carries filters in SST version 2 (docs/sst-format.md section 3.7) and also checks at Open that the filter contains every stored key. Filterless version 1 SSTs remain readable beside version 2 SSTs; this is SST-level compatibility only, since the database still requires manifest version 2.

## 10. Verification and evidence

Differential tests: deterministic seeds, operation traces with Put/Get/Delete/overwrite/empty/binary keys, compare to a simple reference map after each operation and after reopen. Extend across forced flush and compaction. Save failing seed and minimized trace. Use separate encoding golden vectors, not only encode/decode round trips.

WAL tests: truncate at every byte of selected record streams; mutate header/payload fields; assert the policy above. Test huge lengths, invalid type, sequence overflow/order, duplicate semantics, repair-and-append, short reads/writes, EINTR, ENOSPC/EIO, and sync failure. A crafted corrupted input must never trigger unbounded allocation.

Fault injection uses two mechanisms through the internal syscall seam: an adapter for deterministic short writes and errors, and a deterministic crash simulator. For selected deterministic workloads, enumerate calls that change file-system state and crash after each completed call; each enumerated crash point is checked under a process-crash model (all completed writes survive) and a loss-of-unsynced-state model (called the simulated power-loss model below; only explicitly synced state survives). All filesystem access in simulator tests must use the internal abstraction, including open/create, close, reads, stat, directory listing, rename/unlink, mkdir, truncate, file/directory sync and lock ownership. Extend the existing PosixSyscalls seam incrementally; production defaults still call real syscalls. Simulated termination freezes the old filesystem instance and discards the DB; destructor/error-path I/O must not alter the captured state. It is distinct from returning an ordinary I/O error. Test simulator semantics independently, including atomic modeled rename, inode contents, parent directory creation, and released locks. The test driver records operation responses; a durable operation may exist before its response is observed. Recovery oracle accepts an ordered prefix containing all observed successes and optionally the in-flight operation; it does not require recovered state to equal only the acknowledged map. Repeated writes/deletes are judged by final ordered effects. For crashes before initial publication, there are no acknowledged mutations: the allowed outcomes follow the explicit initialization state table, including preserved fail-closed residue. Add small named checkpoints after in-memory apply and around response delivery; syscall checkpoints alone do not exercise those boundaries.

Maintain a matrix with checkpoint, mechanism, expected result, seed, observed result, and regression test. Cover WAL append/sync/application/response, SST sync/rename, manifest write/sync/rename/directory-sync, obsolete-file removal, and compaction publication. Test old-value resurrection and missing referenced files explicitly. Later-operation coverage must not be mistaken for loss of earlier acknowledged values.

CI runs unit/differential tests with ASan and UBSan. These detect memory/undefined-behavior errors, not proof of crash safety. The crash simulator maintains separate volatile and durable state. Successful file sync persists file contents and size; successful directory sync persists namespace changes. A simulated power-loss crash discards volatile state and reopens from durable state. A mutation check skips SST file-content sync, then crashes after the referencing manifest is durable: the normal control must recover, while the mutated loss-of-unsynced-state run must violate the recovery oracle. A later directory sync cannot substitute for file-content sync. Do not assume every individual directory sync is indispensable when a later sync of the same directory covers it.

This model tests loss of unsynced state; it does not model every possible writeback ordering, torn write, or device failure. Byte-level truncation and corruption tests remain separate. Tests inject I/O behavior through an internal construction path. The public Options type does not expose a filesystem injection hook.

## 11. Benchmark protocol (M4)

Record commit, build flags, CPU/RAM/storage, filesystem, WSL2 details, key/value sizes, dataset size, operation count, seed, and persistence mode. Use release builds without sanitizers for timing; correctness checks remain separate.

Report ops/s and per-operation p50/p99 for inserts, existing-key reads, and missing-key reads. Include flush/compaction stalls in end-to-end latency. Run three trials and report median and range; do not select only the best run. Measurements are warm-cache; no cold-cache claims.

Ablations:
- Durability cost: the same insert workload with fsync disabled through the internal construction path, labeled unsafe and used only for measurement. Define which file and directory syncs the switch bypasses. The latency difference measures the sync-policy effect, not an exact fraction of time spent in fsync; syscall timing would be required for the latter.
- Bloom on/off with identical SST data and missing-key lookup trace: SST probes, data-block reads, and bytes requested per lookup. These are not necessarily physical device reads because OS caching exists.
- Compaction run-threshold sweep, including no compaction: write amplification, total database bytes, SST probes per read, and insert p99.

Write amplification = (WAL bytes + flush SST bytes + compaction SST bytes) / successful user mutation bytes, with user bytes defined as key+value for Put and key for Delete. Exclude empty-byte-only workloads from this ratio. Report manifest/other metadata write bytes separately from that defined amplification numerator. Use the same deterministic operation trace and initial logical state for each threshold; include maintenance latency and measure final size at the same workload boundary, without silently forcing extra maintenance on one variant. Never compare an unsynced competitor against this synchronous baseline without labeling durability differences.

Counters and the exact protocol are defined in docs/m4-design.md sections 4 and 5; measured results are in bench/results/summary.md. Each optimization must include a hypothesis, measurement, and explanation of its cost, including disappointing results.

References:
- LevelDB implementation: https://github.com/google/leveldb/blob/main/doc/impl.md
- LevelDB WAL format: https://github.com/google/leveldb/blob/main/doc/log_format.md
- Linux fsync: https://man7.org/linux/man-pages/man2/fsync.2.html
- Linux write: https://man7.org/linux/man-pages/man2/write.2.html