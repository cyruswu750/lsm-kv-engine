# M4 Design: Bloom Filters, Counters, and Benchmarks

Status: specification of M4, which builds on M3 (docs/m3-design.md).
The byte format is in docs/sst-format.md sections 3 and 3.7; design.md
sections 9 and 11 are authoritative. M4 decisions are listed in section 7
and are numbered D-027 onward in docs/decisions.md.

## 1. Scope

- One Bloom filter per SST (SST version 2), covering every stored key,
  tombstones included. Readers accept version 1 (no filter) and version 2
  SSTs in one database. The manifest stays version 2 only: M2 databases
  remain unsupported.
- Internal counters for lookups and successful writes.
- A Release benchmark driver (`bench/kv_bench.cpp`), a runner
  (`scripts/run_benchmarks.sh`), and a summary script
  (`scripts/summarize_bench.py`) for the four experiment groups of
  design.md section 11.

Out of scope: any public Options change, background work, leveled or
partial compaction, and compression.

## 2. Internal switches (InternalOptions, never public Options)

| Field | Default | Meaning |
|---|---|---|
| `compaction_trigger` | 4 | logical run threshold (M3); 0 disables |
| `bloom_bits_per_key` | 10 | filter bits per key for new SSTs (1 .. 64); 0 writes filterless version 1 SSTs |
| `bypass_bloom_queries` | false | benchmark-only: Get ignores filters. Filters are still written and validated at Open, and answers are identical |
| `sst_limits`, `max_manifest_ssts` | format limits | tests lower them (M3); `sst_limits.max_filter_keys` limits keys per filter-bearing SST |

Syncs are skipped only by a benchmark-only `PosixSyscalls` subclass in
`bench/kv_bench.cpp` (`--skip-sync=1`). It turns every fsync, of files and
directories alike, into a no-op that succeeds, and it counts the skipped
calls by target (`wal`, `sst`, `manifest`, `directory`). Production code
has no such mode, and no correctness test uses it.

## 3. Lookup with filters

Get searches the memtable, then the flush-run SSTs from newest to oldest,
then the one base-run SST whose key range contains the key (M3). For
each SST consulted (a candidate):
1. A key outside [smallest key, largest key] is absent without I/O.
2. For a version 2 SST, unless bypassed, a negative filter answer means
   absent without I/O.
3. Otherwise the one block that may hold the key is read and validated;
   the exact answer decides.

A positive filter answer never skips step 3. Lookup stops at the first
value or tombstone, so a flushed tombstone hides an older value in an
older SST, because the tombstone's key is in its SST's filter.

## 4. Counters (`DbStats`, `DBOpener::Stats` and `ResetStats`)

| Counter | Definition |
|---|---|
| `gets` | Get calls that passed validation and searched |
| `candidate_ssts` | SSTs consulted (step 1 reached) |
| `range_rejections` | candidates rejected by key range |
| `filter_rejections` | candidates rejected by a filter |
| `data_block_reads` | data blocks requested (step 3) |
| `data_block_bytes` | bytes requested for those blocks |
| `mutations` | successful Put and Delete calls |
| `user_bytes` | Put: key + value bytes; Delete: key bytes |
| `wal_bytes` | WAL record bytes (each record's 28-byte header included), counted after their sync succeeded; WAL file headers excluded (they are metadata) |
| `flush_sst_bytes` | sizes of SSTs published by flushes, counted after the manifest sync |
| `compaction_sst_bytes` | sizes of compaction outputs, counted after the manifest sync |
| `metadata_bytes` | published manifests and WAL file headers |
| `flushes`, `compactions`, `compaction_outputs` | completed maintenance |

Requests are engine requests (pread calls through the seam). With a warm
page cache they are not physical device reads.

Write amplification = (`wal_bytes` + `flush_sst_bytes` +
`compaction_sst_bytes`) / `user_bytes`, reported only when `user_bytes`
is nonzero. Metadata is reported separately, per user byte. Database size
is the total size of every regular file in the database directory after
the handle is closed (LOCK, MANIFEST, WALs, SSTs, and any leftover
temporary or orphan file); the summary also reports SST and temporary
file counts.

## 5. Benchmark protocol

- Build: Release (`-O3 -DNDEBUG`), `KV_SANITIZE=OFF`,
  `KV_BUILD_BENCH=ON`, in a build directory separate from the sanitizer
  build. Data on the storage being measured (ext4 here).
- Clock: `std::chrono::steady_clock`. Each DB call is timed alone, and
  its time includes the two clock reads around it; the median cost of
  such a pair is measured in every run and reported
  (`timer_overhead_ns`), so sub-microsecond latencies should be read
  with it. Percentiles use the nearest rank. Throughput (`ops_per_s`) is
  calls divided by the sum of the per-call times. The loop's wall time,
  which also includes the harness's untimed work between calls
  (building values, updating the model), is reported separately
  (`wall_ops_per_s`). Open (reported separately), warmup, verification,
  and closing are outside both.
- Keys are "k" plus a zero-padded decimal id (16 bytes); values are 100
  bytes derived from (id, version) with SplitMix64. The seed is fixed (42)
  and every repetition of a configuration uses the same trace.
- Insert latency includes any flush or compaction that the Put or Delete
  triggers. After the timed loop, every key of the keyspace is verified
  against the trace's model (untimed); those lookups also give the final
  SST candidates per lookup.
- Read setup (common to groups 1 and 3): one database with 200,000 keys
  (even ids, seeded order), 256 KiB memtable, run threshold 4, filters
  on. It is written once with the sync bypass, since setup durability is
  not measured. Reads open it, perform 20,000 untimed warmup lookups,
  reset the counters, then time 50,000 existing-key and 50,000
  missing-key lookups. Missing ids are odd, so they fall inside the
  populated key ranges and key-range exclusion alone cannot explain the
  result. Every answer is checked after the timed calls.
- Groups:
  1. Baseline: synchronous inserts (20,000 operations over 20,000 ids,
     64 KiB memtable, threshold 4) and the read phase with filters used.
  2. Durability cost: the same insert trace with the sync bypass, each
     from its own fresh database. Unsafe; a reference point only. The
     difference is not an exact fsync time share.
  3. Bloom: the same prepared files and the same lookup traces, with
     filter queries used or bypassed, alternating.
  4. Run thresholds 2, 4, 8, and disabled: the same trace (20,000
     operations over 10,000 ids, 10% Deletes), each from a fresh
     database, ending at the same operation, with no forced flush or
     compaction. Sync on.
- Three repetitions per configuration, run one at a time. A run that
  fails or does not verify is recorded and excluded; the summary reports
  median and [min, max].
- Runs are sequential, in a fixed order. Time-dependent drift of the
  storage is not controlled, so latency differences between
  configurations are not, by themselves, evidence of a causal effect.
- Not measured: physical device I/O, cold caches, hardware power loss.

## 6. Filter memory and size bounds

From docs/sst-format.md section 3.7: at most 4,194,304 keys per
filter-bearing SST. Builder: 8 bytes per key (at most 32 MiB) for the
whole build, plus three copies of the bit array (B bytes each) briefly at
Finish; peak 8n + 3B + 20 bytes, at most 128 MiB at 64 bits per key and
about 47 MiB at the default. Reader: the filter block (at most
33,554,452 bytes) is allocated only after the footer and the 16-byte
header have passed their bounds; Open copies its bit array once, so the
peak is 2B + 20 bytes, and B bytes stay resident per open SST (about
bits_per_key / 8 bytes per key, 1.25 bytes at the default). Filterless
(version 1) output has no key limit. Reducing the copies is possible but
not done in M4.

## 7. Decisions (D-027 onward in docs/decisions.md)

- D-027 Bloom filters as SST version 2 with a fixed, self-specified hash
  (FNV-1a 64 plus SplitMix64 finalizers, double hashing). Version 1 SSTs
  stay readable; M2 databases are still rejected by the manifest
  version.
- D-028 Damaged filters fail Open: CRC, bounds, a key count different
  from the SST's entries, or any stored key that the filter would deny.
  A filter is never trusted as a negative answer without that check.
- D-029 A per-SST key limit for filter-bearing SSTs (2^22) bounds both
  builder and reader memory; compaction splits at it like at the block
  and index limits.
- D-030 Internal counters and benchmark-only switches (filter-query
  bypass, sync bypass through the syscall seam) stay outside public
  Options; the sync bypass exists only in the benchmark driver.
