# C++20 LSM KV Engine — Design Contract

Status: proposed design, not an implementation report. Initial scope: one process, one calling thread, Linux/WSL2. Repository destination: docs/design.md.

## 1. Purpose and engineering questions

Build an embedded persistent byte-key/byte-value store without using LevelDB or RocksDB as the storage implementation. The project investigates (1) recovery of acknowledged operations across explicitly tested failures, (2) safe publication of new on-disk state, and (3) the measurable costs of durability, compaction, and negative lookups. Standard LSM techniques are prior art, not claimed inventions.

The distinguishing deliverables are an executable failure matrix, randomized reference-model tests, reproducible benchmark scripts, and design decisions supported by observed results. No production-readiness, hardware power-loss validation, or performance numbers are claimed before verification.

## 2. Scope and milestones

- M1: ordered memtable, versioned checksummed WAL, synchronous Put/Delete, Get, reopen/replay, malformed-input tests, differential tests.
- M2: immutable SSTables, sparse block index, synchronous flush, manifest publication and WAL retirement.
- M3: synchronous L0-to-L1 compaction including overlapping L1 tables, Bloom filters, publication failure tests.
- M4: benchmark ablations, recovery report, documented limitations and README.

No transactions, snapshots, range API, concurrent calls, background workers, replication, SQL, compression, or network service in the baseline. A directory lock rejects a second opener. Later work must not obscure correctness of M1. If leveled compaction exceeds the budget, explicitly revise the design to full-SST merging; do not label that implementation leveled compaction.

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

These are interface proposals, not a complete compilable header. Required standard headers include string, string_view, filesystem, memory, and cstddef. Implementations/internal members come later. C++20 has no std::expected; explicit Status plus output parameters avoids introducing a custom generic result type now.

- Open requires an empty out pointer; a nonempty pointer returns InvalidArgument without replacement. On other failure out remains empty. A DB is exposed only after validation/recovery succeeds. Missing database plus create_if_missing=false returns NotFound. Existing unrelated/nonempty directories are not silently initialized. Initial durable directory creation includes the required parent-directory syncs.
- Keys and values are byte strings: embedded zero bytes and empty strings are valid. Max key length is 64 KiB; max value length is 16 MiB. Reject oversized input before I/O. memtable_bytes must be positive; it is a soft flush threshold, not a strict RSS cap.
- Put replaces the current value. Get returns an owning copy; on any non-Ok result it clears out_value. Thus an empty value is distinct from NotFound. Invalid key length returns InvalidArgument.
- Delete is idempotent: deleting an absent key returns Ok after recording a durable tombstone. Get must not reveal an older value afterward.
- string_view input is borrowed only for the call. The engine copies retained bytes; it never stores views into caller memory. Inputs must not alias mutable output arguments.
- Expected validation, format, and I/O failures use Status. Allocation failure is not promised to be recoverable; if an exception occurs after persistence begins, invalidate the handle before propagating. No further calls on that handle except destruction.
- Destructor releases resources and never throws. Successful mutation durability must not depend on destruction or clean shutdown.

## 4. State and ordering

Use std::map<std::string, Entry, BytewiseLess> initially. Define BytewiseLess as lexicographic unsigned-byte order. Entry owns a value or tombstone and a uint64_t sequence number. std::map supplies an ordered container; implementing a balanced tree is outside this project's purpose.

Sequence numbers begin at 1 and increase for each accepted mutation; reject overflow. They order versions, not keys. Replay is in log order. After M2, next sequence exceeds both the manifest checkpoint and all replayed records. Cross-layer lookup chooses the greatest sequence for a key; a winning tombstone means NotFound. Duplicate identical versions may be tolerated during recovery, but conflicting records with the same sequence are Corruption.

## 5. Durability contract

Baseline mutations are synchronous. Validate and encode; append the complete WAL record; successfully fsync the WAL; install the entry in memory; then return Ok. No caller may observe an intermediate state because calls are single-threaded. Prepare required memory before appending when practical. Any failure after append begins makes the handle require recovery, including Get, to avoid serving an in-memory state inconsistent with durable state.

write success is not the persistence boundary. Loop for short writes; retry EINTR; handle zero progress and other errors as failure. A successful fsync is the persistence boundary under the filesystem/device contract. For newly created files, directory entries must also be durably published before acknowledging operations that depend on them.

If Put/Delete returns Ok, recovery must preserve its effect unless a later recovered mutation supersedes it. An operation interrupted before its response, or returning an I/O error after persistence began, has an uncertain outcome: it may survive completely or be absent. It must never produce a partially decoded key/value. An error is not a rollback guarantee.

After WAL append/sync failure return IOError and enter NeedsRecovery. Never retry fsync after it fails: on Linux, dirty pages whose writeback failed may already be marked clean, so a later successful fsync does not prove the data reached storage. The handle enters NeedsRecovery immediately.Subsequent calls return NeedsRecovery. Reopen determines valid on-disk state. Do not continue appending past a failed partial record.

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

Use synchronous flush before the next mutation when the soft threshold has been exceeded. This avoids returning an ambiguous flush error for a mutation already acknowledged. Reads remain available between calls; no background tasks. Replay may temporarily exceed the threshold in M1; bounded-memory recovery is later work.

The manifest is the authoritative snapshot of live SST IDs/levels, WAL IDs, active WAL, checkpoint sequence, format version, and generation. It requires a versioned, checksummed encoding and validation before use. Directory listing alone does not decide which files are live.

Publication protocol (one foreground operation at a time):
1. Keep the previous manifest and its referenced files intact.
2. Write a new SST to a unique temporary file; sync it; rename to a unique final name; sync the directory. Create and sync a new empty WAL, then sync its directory entry.
3. Write a new temporary manifest referencing the new SST and new WAL, with a checkpoint covering the flushed memtable; fsync the temporary manifest.
4. Rename over MANIFEST and fsync the directory. Until this completes, do not acknowledge new operations using the new generation.
5. Switch in-memory state; retire the previous WAL only after the new manifest is durable. Remove obsolete files and sync the directory for persistent cleanup.

On failure after publication begins, invalidate the handle and reopen. Recovery accepts the old or new published state under the filesystem model; both must have their dependencies intact. Missing/corrupt referenced files cause failure. Unreferenced files are ignored, then cleaned only after a valid manifest is established. Never delete old files before publication. File IDs are never reused, including IDs occupied by orphan files.

## 9. Compaction and Bloom filters (M3)

L0 tables may overlap. L1 tables are disjoint by key range. Compact selected L0 inputs plus all overlapping L1 inputs, expanding the input set if range closure requires it. Merge by key and descending sequence, keeping the newest version per key. Publish outputs and retire inputs through the same manifest protocol. Selection/overlap and output-boundary rules need dedicated tests before enabling compaction.

Keep tombstones unless the compaction covers every possible older version of that key. Initially retaining all tombstones is acceptable. Dropping one just because its own input lacks an older value is incorrect: an unselected file may resurrect that value. No snapshots means old versions may be dropped once this coverage condition is proved.

Bloom filters summarize all keys present in an SST, including tombstones. Negative means skip that SST; positive means inspect it, not that the key exists. False positives are allowed, false negatives are not. Version the stable hash/encoding; never rely on unspecified std::hash persistence. Corrupt filter metadata must cause a validated fallback to ordinary lookup or an explicit error, never an unchecked negative.

## 10. Verification and evidence

Differential tests: deterministic seeds, operation traces with Put/Get/Delete/overwrite/empty/binary keys, compare to a simple reference map after each operation and after reopen. Extend across forced flush and compaction. Save failing seed and minimized trace. Use separate encoding golden vectors, not only encode/decode round trips.

WAL tests: truncate at every byte of selected record streams; mutate header/payload fields; assert the policy above. Test huge lengths, invalid type, sequence overflow/order, duplicate semantics, repair-and-append, short reads/writes, EINTR, ENOSPC/EIO, and sync failure. A crafted corrupted input must never trigger unbounded allocation.

Fault injection uses two distinct mechanisms: an I/O adapter for deterministic short writes/errors and subprocess termination at named checkpoints. Parent process records operation responses; a durable operation may exist before its response is observed. Recovery oracle accepts an ordered prefix containing all observed successes and optionally the in-flight operation; it does not require recovered state to equal only the acknowledged map. Repeated writes/deletes are judged by final ordered effects.

Maintain a matrix with checkpoint, mechanism, expected result, seed, observed result, and regression test. Cover WAL append/sync/application/response, SST sync/rename, manifest write/sync/rename/directory-sync, obsolete-file removal, and compaction publication. Test old-value resurrection and missing referenced files explicitly. Later-operation coverage must not be mistaken for loss of earlier acknowledged values.

CI runs unit/differential tests with ASan and UBSan. These detect memory/undefined-behavior errors, not proof of crash safety. Parser fuzzing is an optional extension after deterministic tests.

## 11. Benchmark protocol (M4)

Record commit, compiler/build flags, CPU/RAM/storage, filesystem, WSL2/host details, key/value sizes, dataset size, operation count, seed, workload distribution, cache procedure, repetitions, and persistence mode. Use release builds without sanitizers for timing; correctness checks remain separate.

Report ops/s and per-operation p50/p99 for inserts, overwrites, existing-key reads, missing-key reads, and mixed workloads. Include flush/compaction stalls in end-to-end latency. Use multiple trials and report spread; do not select only the best run.

Ablations: Bloom on/off with identical SST data and lookup trace; before/after compaction with identical logical contents. Count SST probes, data-block reads, and bytes requested. These are not necessarily physical device reads because OS caching exists. Do not claim cold cache unless the cache-control procedure is verified.

Write amplification = (WAL bytes + flush SST bytes + compaction SST bytes) / successful user mutation bytes, with user bytes defined as key+value for Put and key for Delete. Exclude empty-byte-only workloads from this ratio; report metadata bytes separately. Space measurement reports total database bytes and live key/value bytes, including WAL/tombstone contribution explicitly. Never compare an unsynced competitor against this synchronous baseline without labeling durability differences.

Results start as pending. Each optimization must include a hypothesis, measurement, and explanation of its cost, including disappointing results.

## 12. Decision log and attribution

Initial decisions: std::map reduces unrelated container work; synchronous WAL gives a clear success boundary; separate header/payload CRCs prevent blindly trusting lengths; strict checksum failure prioritizes detectable corruption over availability; foreground maintenance makes publication auditable at the cost of latency spikes.

For subsequent decisions record alternatives, chosen policy, reason, verifying test, and measured consequence. Record real bugs and fixes rather than retroactively inventing a development story. AI may assist build/test scaffolding and review; the owner implements and understands core storage modules under the project plan. This design draft was AI-assisted and must be reviewed by the owner before adoption. Implementation status is tracked separately.

References (design background, not claims of format compatibility):
- LevelDB implementation: https://github.com/google/leveldb/blob/main/doc/impl.md
- LevelDB WAL format: https://github.com/google/leveldb/blob/main/doc/log_format.md
- Linux fsync: https://man7.org/linux/man-pages/man2/fsync.2.html
- Linux write: https://man7.org/linux/man-pages/man2/write.2.html

## 13. Immediate acceptance gate

Before writing M1, the owner can explain: empty value versus deletion; why write is not fsync; why a failed mutation may survive; how a record boundary is located; why a bad CRC differs from an incomplete tail; and why a repaired tail must be truncated before append. Implement only API types and memtable next, then codec, then WAL/recovery. Do not ask an agent to generate the entire engine from this document.
