# On-Disk Format: SSTables, Manifest, and Directory Rules

The on-disk format implemented in `src/core/`: file names, SSTables,
the manifest, sequence and counter rules, directory states, recovery,
and the flush publication steps. SSTs are version 1 (no filter, read
only) or version 2 (with a Bloom filter, the version written; section
3.7). The manifest is version 2 only (section 4). The WAL record
encoding is in design.md section 6; the engine-level contracts this
format implements are in design.md sections 4, 5, 8 and 9.

Compatibility statements name engine versions by their release tags: M1 is
`v0.1-m1` (WAL only), M2 `v0.2-m2` (SSTs, manifest version 1), M3
`v0.3-m3` (compaction, manifest version 2) and M4 `v0.4-m4` (SST version
2 with Bloom filters).

All integers are unsigned little-endian. Fields are serialized
individually, never by dumping C++ structs. All checksums are standard
CRC-32C as defined in design.md section 6 (no masking). "Corruption",
"UnsupportedFormat", and the other names are `kv::Code` values.

## 1. Limits shared by every format

| Name | Value | Meaning |
|---|---:|---|
| `kMaxKeyLength` | 65,536 | Public API key limit (unchanged) |
| `kMaxValueLength` | 16,777,216 | Public API value limit (unchanged) |
| Sequence | 1 .. 2^64-1 | 0 is never a valid record sequence |
| File ID | 1 .. 2^64-1 | 0 is never a valid file ID |

## 2. File names

| Name | Role |
|---|---|
| `LOCK` | Advisory `flock` target. Contents unused. Never removed. |
| `MANIFEST` | The only authoritative description of the database. |
| `MANIFEST.tmp` | Manifest being published. Never authoritative. |
| `wal-<id>.log` | A WAL file (WAL v1 bytes). |
| `wal-<id>.log.tmp` | A WAL being created. |
| `sst-<id>.sst` | An SSTable. |
| `sst-<id>.sst.tmp` | An SSTable being written. |
| `000001.wal`, `000001.wal.tmp` | M1 names. Recognized only to reject M1 directories. |

`<id>` is the decimal file ID, zero-padded to at least 6 digits. It is
canonical: an ID below 1,000,000 has exactly 6 digits; a larger ID has no
leading zero. So the representation is 6 to 20 digits and the value is in
1 .. 2^64-1. A name with a non-canonical, zero, or out-of-range ID (for
example `wal-0000001.log`, `sst-000000.sst`, or a 21-digit number) is not
an engine name. Such names are unrelated entries.

WAL and SST files share one ID space: one ID names at most one file kind.
A temporary file has the same ID as its final name.

## 3. SSTable format, versions 1 and 2

An SST is immutable once renamed to its final name. It contains:

```
version 1: [data block 0] ... [data block n-1] [index block] [footer]
version 2: [data block 0] ... [data block n-1] [filter block] [index block] [footer]
```

There is no file header. The regions are contiguous in this order, with
no gaps or overlaps. The index locates every data block. Version 1 has no
filter; version 2 always has exactly one filter block (section 3.7). Data
blocks and the index block are identical in both versions. "Data region"
below means the bytes before the filter (version 2) or before the index
(version 1); its size D is filter_offset (version 2) or index_offset
(version 1).

### 3.1 Data block

A data block is one or more entries followed by an 8-byte trailer.

Entry (17-byte header plus key and value):

| Offset | Bytes | Field | Rule |
|---:|---:|---|---|
| 0 | 4 | key_len | at most 65,536 |
| 4 | 4 | value_len | at most 16,777,216; 0 for Delete |
| 8 | 8 | sequence | at least 1 |
| 16 | 1 | type | 1 = Put, 2 = Delete |
| 17 | key_len | key | raw bytes |
| 17+key_len | value_len | value | raw bytes |

Block trailer:

| Offset from trailer | Bytes | Field |
|---:|---:|---|
| 0 | 4 | entry_count, at least 1 |
| 4 | 4 | block_crc32c: CRC-32C of every block byte before this field (entries and entry_count) |

Block size rule. The writer adds entries in key order. An entry goes into
the current block if the block is empty or if the block's entry bytes plus
the new entry stay at most `kSstBlockTarget = 4096` bytes. Otherwise the
current block is finished and a new one starts. So a block whose entry
region exceeds 4096 bytes contains exactly one entry: a legal large entry
gets its own oversized block. The reader enforces exactly that property.
It does not require a particular packing of small entries.

| Bound | Bytes |
|---|---:|
| Smallest entry (empty key and value) | 17 |
| Largest entry: 17 + 65,536 + 16,777,216 | 16,842,769 |
| Smallest block: one empty entry plus trailer | 25 |
| `kMaxSstBlockSize`: largest entry plus trailer | 16,842,777 |

### 3.2 Index block

One index entry per data block, in block order, followed by a trailer.

Index entry (16-byte header plus key):

| Offset | Bytes | Field | Rule |
|---:|---:|---|---|
| 0 | 4 | key_len | at most 65,536 |
| 4 | 8 | block_offset | the first block is at 0; each later block starts where the previous one ends |
| 12 | 4 | block_length | 25 .. `kMaxSstBlockSize`, trailer included |
| 16 | key_len | last_key | the last (largest) key in that block |

Index trailer:

| Offset from trailer | Bytes | Field |
|---:|---:|---|
| 0 | 4 | block_count, at least 1, equal to the number of index entries |
| 4 | 4 | index_crc32c: CRC-32C of every index byte before this field |

The smallest index block (one entry with an empty key) is 24 bytes.

Index limits, enforced by both the writer and the reader (section 3.6):

| Name | Value | Meaning |
|---|---:|---|
| `kMaxSstBlocks` | 1,048,576 | data blocks, and so index entries, per SST |
| `kMaxSstIndexSize` | 67,108,864 (64 MiB) | encoded index block, trailer included |

### 3.3 Footer (48 bytes, at the end of the file)

| Offset | Bytes | Field | Version 1 rule | Version 2 rule |
|---:|---:|---|---|---|
| 0 | 4 | footer_crc32c | CRC-32C of footer bytes [4, 48) | same |
| 4 | 8 | index_offset | equals the end of the last data block | filter_offset + filter_length |
| 12 | 8 | index_length | index_offset + index_length + 48 = file size | same |
| 20 | 8 | filter_offset | must be 0 | equals the end of the last data block (D) |
| 28 | 8 | filter_length | must be 0 | 28 .. `kMaxSstFilterSize` |
| 36 | 4 | version | 1 | 2 |
| 40 | 8 | magic | ASCII `KVSST001` | same |

The footer has no reserved bytes. Any extension needs a new version.
Every version must keep `version` and `magic` as the last 12 bytes of the
file, so a reader can identify a version before trusting the rest of the
layout.

The smallest SST is 25 + 24 + 48 = 97 bytes (`kMinSstFileSize`), a
version 1 file. The smallest version 2 file is 25 + 28 + 24 + 48 = 125
bytes.

### 3.4 Validation

Validation never trusts a length before checking it against the file
range and the limits above. A buffer is allocated only for a region that
passed those checks, so one allocation is at most `kMaxSstIndexSize`
(index) or `kMaxSstBlockSize` (block), even when the file is sparse and
its declared size costs no storage. For each referenced SST, Open runs
these checks in this order:

1. The entry is a regular file (lstat), and its size equals the size
   recorded in the manifest. Otherwise Corruption. A truncated SST is
   always Corruption. There is no tail repair for SSTs.
2. Footer (the size is at least 97 because the manifest requires it):
   wrong magic gives Corruption; a version other than 1 or 2 gives
   UnsupportedFormat; footer CRC mismatch gives Corruption; a region rule
   violation gives Corruption. Version 1 region rules: both filter
   fields 0, index_offset at least 25, and index_offset + index_length +
   48 = file size. Version 2 region rules: filter_offset at least 25,
   filter_length at least 28 and at most `kMaxSstFilterSize`,
   index_offset = filter_offset + filter_length, and index_offset +
   index_length + 48 = file size. All sums are computed without overflow
   (each operand is first checked against the file size). Then
   index_length must be at least 24 and at most min(floor(D / 25),
   1,048,576) x (16 + 65,536) + 8, and at most `kMaxSstIndexSize`: each
   block takes at least 25 bytes of the data region, and each index entry
   at most 16 + 65,536 bytes. A violation is Corruption, decided from the
   footer alone, before the index or filter is allocated or read.
2a. Version 2 only, the filter (section 3.7): its 16-byte header is read
   first; an unknown filter_version or hash_id gives UnsupportedFormat;
   the key count, probe count, and bit-array size are then checked
   against their bounds (Corruption), all before the bit array is
   allocated; then the whole block is read and its CRC checked
   (Corruption).
3. Index, read as exactly index_length bytes: CRC, block_count at least 1
   and at most `kMaxSstBlocks`, exactly block_count entries consuming exactly
   index_length - 8 bytes, the key length limit, contiguous block offsets
   starting at 0, the block length bounds, the last block ending exactly
   at index_offset, and strictly increasing last_key. Any violation is
   Corruption.
4. Every data block is read once, in order (streaming; the file is never
   held in memory as a whole). For each block: length at least 25, CRC,
   entry_count at least 1, exactly entry_count entries consuming the
   block, each entry within the length limits and the block, type 1 or 2,
   Delete with value_len 0, sequence at least 1, an entry region larger
   than 4096 only with entry_count 1, keys strictly increasing in
   BytewiseLess order across the whole file (one version per key), and
   the block's last key equal to its index last_key. Version 2 also: the
   filter reports every entry's key as possibly present (no false
   negative), and the total entry count equals the filter's key_count.
   Any violation is Corruption.
5. The observed minimum and maximum sequence over all entries equal the
   manifest's min_sequence and max_sequence for that file. Otherwise
   Corruption.

The SST must be nonempty: a valid SST has at least one entry.

Get first rejects a key outside the SST's [smallest key, largest key]
range without I/O. For a version 2 SST it then queries the filter kept in
memory from Open; a negative answer means the key is absent from this SST
(no I/O). Otherwise Get re-reads the one candidate block and validates it
again under step 4, using the index kept in memory from Open. A positive
filter answer never short-cuts that exact lookup. A Get-time block
failure returns Corruption (or IOError for a read error) from that Get.

### 3.5 Compatibility

- Version 1 has no filter. Version 2 places the filter between the last
  data block and the index. M4 writes version 2 (a filter is always
  present: version 2 with filter_length 0 is Corruption) and reads both
  versions, so one database may mix filterless version 1 SSTs and
  filter-bearing version 2 SSTs; lookup simply skips the filter query for
  version 1. This is SST-level compatibility only: the database must still
  have a version 2 manifest (section 4.3), so M2 databases remain
  unsupported.
- Older readers (M2 and M3) accept only SST version 1 and check the
  version (the last 12 bytes) before anything else, so a version 2 SST is
  UnsupportedFormat for them. Because an M3 database already needs an
  M3-or-later reader (manifest version 2), M4 SSTs make a database
  readable only by M4 or later.
- M3 compaction reads SSTs sequentially, block by block, using the same
  block validation as step 4.

### 3.6 Index limits, capacity, and oversized SSTs

The two limits in section 3.2 were added to M2 v1 by B-006 and were
established before the first M2 release. Development versions before
B-006 did not enforce them. An SST that such a version wrote beyond
either limit is rejected as Corruption, like any other file that
violates them (Reader, below).

Writer. `SstBuilder` checks each data block when it closes it: in the
`Add` that starts the next block, or in `Finish` for the last block. The
check uses the block's final last key and runs before that block is
written. If the block would be block 1,048,577, or would make the index
(trailer included) exceed `kMaxSstIndexSize`, the call returns
InvalidArgument. The limits are exact: an SST at either limit is written,
and the reader accepts it.

Reader. Section 3.4, steps 2 and 3. An SST beyond either limit is
Corruption at Open like any other format violation, whoever wrote it. It
is never repaired.

Capacity.
- Blocks. A block is closed only when the next entry does not fit, so any
  two consecutive blocks hold more than 4,096 entry bytes together.
  1,048,576 blocks therefore need more than 2 GiB of entries, and about
  4 GiB with full 4 KiB blocks.
- Index. An index entry is 16 bytes plus the block's last key. With short
  keys the block limit is reached first. With 65,536-byte keys every
  entry has its own block, and the index limit allows 1,023 such blocks
  (about 64 MiB of data).
- Flushes. A valid index is always smaller than the data region: every
  block contains its last key in an entry, so a block is at least 25
  bytes plus that key. The data region is in turn smaller than the
  memtable's `ApproximateBytes()` (an entry costs 17 + key + value bytes
  on disk, 32 + key + value in the memtable, and each block adds 8 bytes).
  A flush starts once `ApproximateBytes() >= memtable_bytes` before a
  mutation is applied, so a flushed memtable is below memtable_bytes +
  16,842,784, the accounting of the largest entry. If every record in the
  active WAL was written with memtable_bytes at most 50,266,080 (64 MiB
  minus 16,842,784, about 47.9 MiB), no flush can reach either limit. The
  default 4 MiB is far below that.

Oversized flush. With a larger memtable_bytes and long keys, a flush can
reach a limit. The builder's InvalidArgument then occurs during F1, after
the temporary SST was created, so section 10 applies: the triggering
mutation fails with InvalidArgument, the handle moves to NeedsRecovery,
the old generation stays durable, every acknowledged mutation stays in
the active WAL, and the partial `sst-<S>.sst.tmp` is an orphan (section
7). Recovery replays the same WAL into one memtable and a flush writes one
SST, so every later flush of that memtable fails the same way, whatever
memtable_bytes the reopen uses. The database stays readable, but each
mutation that triggers a flush is refused, and each attempt leaves one
more temporary SST until a publication covers its ID. This is a loss of
write availability, not of data. M2 neither splits a flush into several
SSTs nor caps memtable_bytes; either would be a separate change.

### 3.7 Filter block (SST version 2)

One Bloom filter per SST covers every entry's key, including tombstones,
so a negative answer proves that the SST holds no version of the key.

Layout (filter_length bytes at filter_offset):

| Offset | Bytes | Field | Rule |
|---:|---:|---|---|
| 0 | 4 | filter_version | 1 (this layout) |
| 4 | 4 | hash_id | 1 (KVHASH1, below) |
| 8 | 4 | key_count n | 1 .. `kMaxSstFilterKeys` = 4,194,304, and at most floor(D / 17); equals the SST's entry count |
| 12 | 4 | num_probes k | 1 .. 30 |
| 16 | B | bits | B = filter_length - 20; 8 <= B <= max(8, 8 x n) |
| 16 + B | 4 | filter_crc32c | CRC-32C of bytes [0, 16 + B) |

The bit array has m = 8 x B bits. Bit number b (0 <= b < m) is bit
(b mod 8) of byte floor(b / 8), counting from the least significant bit
(mask 1 << (b mod 8)).

KVHASH1 (all arithmetic on unsigned 64-bit integers, modulo 2^64):
- a = FNV-1a 64 of the key bytes: start at 0xcbf29ce484222325; for each
  byte, a = (a XOR byte) x 0x100000001b3. The empty key gives the offset
  basis.
- mix(z) is the SplitMix64 finalizer: z ^= z >> 30;
  z *= 0xbf58476d1ce4e5b9; z ^= z >> 27; z *= 0x94d049bb133111eb;
  z ^= z >> 31.
- h1 = mix(a); h2 = mix(a XOR 0x9e3779b97f4a7c15) OR 1 (always odd).
- Probe i (0 <= i < k) sets or tests bit ((h1 + i x h2) mod 2^64) mod m.

The hash is fixed by this text, never by a library (std::hash may change
between builds). Golden vectors for KVHASH1 and for two complete filters
come from an independent script (`scripts/bloom_reference.py`) and are
checked in `tests/harness/bloom_test.cpp`.

Builder rules (the reader does not assume them):
- bits_per_key is an internal setting, 1 .. 64, default 10. 0 disables
  filters, and the writer then produces version 1 SSTs. Filterless output
  has no filter key limit: only the block and index limits of section
  3.6 apply to it.
- k = round(bits_per_key x 0.69) = floor((bits_per_key x 69 + 50) / 100),
  clamped to 1 .. 30; 7 for the default.
- B = max(8, ceil(n x bits_per_key / 8)) bytes. The minimum filter
  (one key) is 8 bytes of bits, 28 bytes in all.
- The builder keeps one 64-bit FNV-1a value per key (8n bytes, at most
  8 x `kMaxSstFilterKeys` = 32 MiB) until it is destroyed. At Finish it
  also holds, briefly, the bit array, the encoded block (header, a copy of
  the bits, CRC), and the in-memory filter decoded from that block (a
  further copy), so its filter-related peak is 8n + 3B + 20 bytes: at
  most 128 MiB at 64 bits per key and 2^22 keys, about 47 MiB at the
  default 10 bits per key. It refuses a key that would exceed `kMaxSstFilterKeys` or make the
  filter exceed `kMaxSstFilterSize`. Like the block and index limits,
  compaction splits its output before that point (`SstBuilder::CanAdd`);
  a single flush that exceeds it fails (the B-006 large-memtable case).

Reader rules and limits:
- `kMaxSstFilterSize` = 8 x `kMaxSstFilterKeys` + 20 = 33,554,452 bytes
  (32 MiB of bits plus header and CRC) bounds the filter block that Open
  reads. Open then copies the bit array (B bytes) into the in-memory
  filter and frees the block when it returns, so its filter-related peak
  is 2B + 20 bytes (at most about 64 MiB), and B bytes stay resident per
  open SST. The checks of section 3.4 step 2a run before the block is
  allocated: filter_length from
  the footer, then the 16-byte header (versions, then n, k, and B
  against the bounds above, with 8 x n computed in 64 bits). A file
  whose size satisfies the footer but whose header claims impossible
  sizes is rejected without reading the bit array.
- An unknown filter_version or hash_id is UnsupportedFormat. Every other
  violation (CRC, n, k, B, a false negative for a stored key, a count
  mismatch) is Corruption: Open fails, and a damaged filter
  is never used as a negative answer.
- The validated filter (n, k, m, bits) stays in memory with the index.
  Get computes KVHASH1 once per probed SST.

## 4. Manifest format, version 2 (M3)

M3 reads and writes manifest version 2 only. Version 1 (the M2 format)
and every other version are rejected as UnsupportedFormat (section 4.3).
There is no migration, no fallback to version 1, and no downgrade.

The manifest is one record that fills the whole file. Each publication
replaces the file by rename. It is never appended to.

| Offset | Bytes | Field | Rule |
|---:|---:|---|---|
| 0 | 8 | magic | ASCII `KVMAN001` |
| 8 | 4 | version | 2 |
| 12 | 4 | reserved | 0 |
| 16 | 8 | generation | at least 1 |
| 24 | 8 | next_file_id | greater than every file ID referenced below |
| 32 | 8 | checkpoint_sequence | at least every listed max_sequence; 0 before any flush |
| 40 | 8 | active_wal_id | at least 1 |
| 48 | 4 | sst_count | at most `kMaxManifestSsts = 1,048,576` |
| 52 | 4 | base_run_count | at most sst_count (section 4.1) |
| 56 | 32 x sst_count | SST entries | see below |
| 56 + 32 x sst_count | 4 | manifest_crc32c | CRC-32C of every byte before it |

The layout is the version 1 layout. Only the version value and the
meaning of offset 52 (reserved in version 1) differ.

SST entry (32 bytes):

| Offset | Bytes | Field | Rule |
|---:|---:|---|---|
| 0 | 8 | file_id | at least 1 |
| 8 | 8 | file_size | at least 97 (`kMinSstFileSize`) |
| 16 | 8 | min_sequence | at least 1 |
| 24 | 8 | max_sequence | at least min_sequence, at most checkpoint_sequence |

So the file size is exactly 60 + 32 x sst_count bytes: 60 bytes at
minimum and 33,554,492 bytes at most (`kMaxManifestSize`). All file IDs,
including active_wal_id, are distinct. The manifest has no level or
key-range fields.

### 4.1 Runs

A run is the set of SSTs that one publication added. A flush adds a run
of one SST. A compaction adds the base run: zero or more SSTs that
together hold the merged state of every SST that was live before it.
Every live SST is a compaction input, so the base run is always the
oldest run.

- The first `base_run_count` entries form the base run. The remaining
  entries are flush runs of one SST each, oldest first.
- `base_run_count = 0`: there is no base run. Either no compaction has
  published yet, or the last compaction's output was empty.
- `base_run_count = 1`: one base SST.
- `base_run_count = k >= 2`: k base SSTs.
- Base SSTs are listed in key order with strictly non-overlapping key
  ranges: the largest key of entry i is less (BytewiseLess) than the
  smallest key of entry i+1. Key ranges are not stored. Open derives them
  from the validated SSTs (the first entry's key, and the last index
  key) and checks this rule after section 3.4 validation. A violation is
  Corruption.
- Base SST sequence intervals may overlap one another.
- Every flush-run SST is newer than the entire base run: its min_sequence
  exceeds every base max_sequence.
- Flush-run SSTs have strictly ordered, disjoint intervals: entry i's
  max_sequence is less than entry i+1's min_sequence.

Logical run count = (1 if base_run_count > 0, else 0) +
(sst_count - base_run_count). Compaction is triggered by this count, not
by sst_count (docs/m3-design.md section 3).

### 4.2 Validation order

1. MANIFEST that is not a regular file: Corruption.
2. Size below 12: Corruption. Wrong magic: Corruption. Version other than
   2: UnsupportedFormat. The magic and version are not checked against
   the CRC first, so a damaged version field is reported as
   UnsupportedFormat, as with the WAL file header.
3. Size below 60 or above `kMaxManifestSize`: Corruption. This is checked
   with fstat before the whole file is read, which bounds allocation.
4. CRC mismatch, a nonzero reserved field (offset 12), generation 0,
   active_wal_id 0, sst_count above the maximum, file size different from
   60 + 32 x sst_count, base_run_count above sst_count, an invalid SST
   entry, duplicate IDs, a max_sequence above the checkpoint, a violation
   of the section 4.1 sequence rules, or a next_file_id that does not
   exceed every referenced ID: Corruption.
5. After each referenced SST is validated (section 9 step 3), the base
   run's key order: Corruption.

A malformed existing MANIFEST is never replaced or repaired.

### 4.3 Version boundary

An M2 database (manifest version 1) opened by M3, or a manifest of any
version other than 2, fails at step 2 with UnsupportedFormat. This
happens before any WAL is read, truncated, or appended, before any SST is
read, and before any publication or orphan removal, so no existing
database file changes. As for any directory that has MANIFEST, Open takes
the lock first, which creates an empty `LOCK` only if none exists.

An M2 executable opening an M3 database fails the same way: it accepts
only version 1, and it checks the version before anything else.

## 5. Sequence and checkpoint rules

- `checkpoint_sequence` (the checkpoint) is the last sequence whose
  effect is fully captured by the listed SSTs. Every record in the active
  WAL has a sequence greater than the checkpoint. Records are nonzero and
  strictly increasing (WAL v1 rules). The first WAL record is checked
  against the checkpoint before its payload length is trusted. So a
  complete header at or below the checkpoint is Corruption, never a torn
  tail.
- After recovery, last_sequence = max(checkpoint, last replayed WAL
  sequence). The next mutation uses last_sequence + 1. An empty active
  WAL therefore continues above the checkpoint.
- A flush publishes checkpoint = last_sequence at the time of the flush.
  The new SST's sequences are greater than the previous checkpoint
  because the memtable holds only records newer than it. So it is newer
  than every listed SST, including the whole base run.
- A compaction keeps the checkpoint and the active WAL unchanged.
- Lookup order: the memtable; then the flush-run SSTs from newest to
  oldest; then the base run, where at most one SST can hold the key (the
  one whose key range contains it, found by binary search). The first
  value or tombstone found is the answer. This is exact because every
  memtable sequence exceeds every SST sequence, flush runs are newer than
  the base run and ordered among themselves, and a key appears in at most
  one base SST.
- Recovery at a last_sequence of 2^64-1 (from the WAL or the checkpoint)
  succeeds, and Get works. Every later Put or Delete returns
  InvalidArgument before any flush or WAL I/O (D-006).

## 6. Counter and capacity exhaustion

| Counter | Exhausted when | Behavior |
|---|---|---|
| sequence | last_sequence = 2^64-1 | Put/Delete return InvalidArgument before any I/O; the handle stays usable |
| generation | the current generation = 2^64-1 | a flush or compaction that would publish returns InvalidArgument before any I/O; the mutation is rejected; the handle stays usable for reads |
| file ID, flush | the next allocatable ID is above 2^64-3 (a flush needs two IDs, and next_file_id must stay representable) | as for generation |
| file ID, compaction | before any I/O: the next allocatable ID is above 2^64-2 (no ID for even one output). During the merge: an output beyond the first finds no ID | before I/O: as for generation. During the merge: InvalidArgument, NeedsRecovery, durable state unchanged |
| SST count, flush | the manifest already lists the SST-entry limit (`kMaxManifestSsts`) | as for generation |
| SST count, compaction | the merged output needs more than the entry limit minus (compaction_trigger - 1) SSTs, which leaves room for the flushes that lead to the next compaction | InvalidArgument during the merge, NeedsRecovery, durable state unchanged |

No counter wraps around. The checks that need no I/O run before any
maintenance I/O begins. The two in-merge cases are the remaining
capacity limits: the database's live data must fit in about 2^20 SSTs,
each within section 3.6's block and index limits.

## 7. File-ID allocation and orphans

- At Open, the allocation floor is the larger of the manifest's
  next_file_id and 1 + the largest engine-name ID visible in the
  directory. The scan includes temporary files, orphans, and entries
  that are not regular files. If the largest observed ID is 2^64-1, file
  IDs are exhausted.
- A flush allocates the SST ID (the floor) and the new WAL ID
  (floor + 1), then publishes next_file_id = floor + 2.
- An orphan is a regular file with an engine name (section 2) that the
  valid manifest does not reference: an obsolete WAL, an unreferenced
  SST, a temporary file, or `MANIFEST.tmp`. At Open, after the manifest,
  SSTs, and WAL are validated and replayed, and after the directory sync
  of section 9 step 6 has made the visible manifest durable:
  - `MANIFEST.tmp` and orphans whose ID is below the manifest's
    next_file_id are removed. The removal is best effort and not followed
    by another directory sync: if an orphan reappears after a crash, its
    ID is still covered by the durable next_file_id, and the next Open
    removes it again.
  - Orphans whose ID is at least next_file_id are kept. They are the only
    durable evidence of that ID until a manifest whose next_file_id
    exceeds them is durable. The next successful publication raises
    next_file_id above them, and that publication's cleanup removes them.
  - A failed removal is not an error; the name is retried at the next
    publication.
- Entries that are not regular files are never removed. Unrelated names
  are never removed.
- Guarantee: published IDs and IDs observed on disk at Open are never
  reused. An ID allocated in a crashed session whose file disappeared
  completely has no recoverable identity, so no stronger promise is made
  for it.

## 8. Directory states

Kinds come from lstat. Symbolic links are never followed. The scan runs
once before taking `LOCK` (so rejected states create nothing) and again
under the lock. The first rule that matches decides:

| # | Directory contents | create_if_missing | Result |
|---:|---|---|---|
| 1 | Path missing | false | NotFound; nothing created |
| 2 | Path missing | true | Create one level, sync the parent, then initialize (row 9) |
| 3 | Path exists but is not a directory, or is a symlink | any | InvalidArgument |
| 4 | `LOCK` exists and is not a regular file | any | InvalidArgument |
| 5 | `MANIFEST` exists (any kind) | any | Existing M2 database: validate and recover (section 9). Unrelated entries and M1 names are ignored and never removed |
| 6 | `000001.wal` exists (any kind), or `000001.wal.tmp` is a regular file | any | Recognized M1 directory: UnsupportedFormat. Nothing created or changed; no migration |
| 7 | A regular file with an M2 engine name, or a regular `MANIFEST.tmp` | any | Interrupted M2 initialization: Corruption. Residue preserved; no automatic repair or reinitialization |
| 8 | Any other entry, including non-regular entries with engine names | any | InvalidArgument; nothing created or changed |
| 9 | Empty, or `LOCK` only | false | NotFound |
| 9 | Empty, or `LOCK` only | true | Initialize: see below |

Initialization (under the lock), with no mutation acknowledged before it
completes:

1. Write the 16-byte WAL header to `wal-000001.log.tmp`, fsync, close,
   rename to `wal-000001.log`, fsync the directory.
2. Write manifest generation 1 (next_file_id 2, checkpoint 0,
   active_wal_id 1, no SSTs) to `MANIFEST.tmp`, fsync, close, rename to
   `MANIFEST`, fsync the directory.
3. Open `wal-000001.log` for append.

An interruption, or any I/O failure, before step 2's rename leaves
residue that row 7 reports as Corruption on the next Open. Such a
directory may need manual cleanup or a fresh directory. If step 2's
directory sync fails, Open returns IOError without a handle. The rename
has already happened, so a later Open finds a valid MANIFEST.

## 9. Recovery of an existing database (row 5)

In order, under the lock:

1. Read and validate MANIFEST (section 4).
2. Check that every referenced file exists and is a regular file:
   `wal-<active_wal_id>.log` and each `sst-<file_id>.sst`. A missing or
   non-regular referenced file is Corruption. A directory listing is
   never used to rebuild or replace missing state.
3. Validate each SST by streaming (section 3.4) and keep its index in
   memory. Then check the base run's key order (section 4.1).
4. Replay the active WAL (design.md section 7, with the checkpoint rule
   from section 5). An incomplete trailing record is truncated and the
   file is synced before any append. Only the active WAL is referenced in
   M2, so no inactive WAL is read.
5. Open the active WAL for append.
6. fsync the database directory. The visible MANIFEST may come from a
   rename whose directory sync never completed (a crash between F5's
   rename and its sync, or a failed sync that invalidated the previous
   handle). Until this sync succeeds, nothing that generation superseded
   is removed and no mutation is acknowledged (design.md section 8,
   step 4). A failure returns IOError with no handle and removes nothing.
7. Compute the allocation floor and clean orphans (section 7).
8. Expose the handle.

Any failure returns the error, releases the lock, and leaves the output
pointer empty.

## 10. Flush and publication

A flush runs synchronously inside Put/Delete. The order is:

1. Run the checks that need no I/O: the handle is not invalidated, the
   key and value lengths, and sequence availability.
2. If the memtable is nonempty and ApproximateBytes() >= memtable_bytes,
   flush before appending this mutation.
3. Run the flush's own counter checks (section 6) before any I/O.

Flush steps:

| Step | Action |
|---:|---|
| F1 | Write the memtable (tombstones included) to `sst-<S>.sst.tmp` block by block; fsync; close; rename to `sst-<S>.sst` |
| F2 | Write the WAL header to `wal-<W>.log.tmp`; fsync; close; rename to `wal-<W>.log`; open it for append |
| F3 | fsync the directory. This one sync makes both F1 and F2 names durable before any manifest refers to them |
| F4 | If Open could not remove a stale `MANIFEST.tmp`, remove it now (a failure here is a publication failure). Encode the next manifest: generation + 1, next_file_id W + 1, checkpoint = last_sequence, active_wal_id W, and the previous SSTs plus S. Write it to `MANIFEST.tmp` (exclusive create); fsync; close |
| F5 | Rename `MANIFEST.tmp` over `MANIFEST`; fsync the directory. Publication is durable when this sync returns |
| F6 | Install the new state in memory: new manifest, new SST reader, new WAL handle, empty memtable. Memory is prepared before F1 where practical, and these steps do not allocate |
| F7 | Cleanup: close the old WAL handle; unlink the old WAL and any deferred orphans now below next_file_id; if any unlink succeeded, fsync the directory |
| F8 | Append and sync the triggering mutation to the new WAL (design.md section 5) |

The SST writer neither syncs nor renames. The flush layer does each of
those exactly once.

Error behavior:

| Failure | Returned | Handle | Durable state |
|---|---|---|---|
| A counter check in section 6 | InvalidArgument | usable; no I/O happened | unchanged |
| Any error in F1-F5 (open, write, fsync, close, rename, directory sync) | that error (IOError) | NeedsRecovery; every later call returns NeedsRecovery without I/O | the old or new generation. Reopen accepts whichever is durable; the files of the other are orphans |
| An SST index limit in F1 (section 3.6) | InvalidArgument | NeedsRecovery, as above | the old generation; the partial temporary SST is an orphan |
| An exception from F1 through F6 | propagated | NeedsRecovery | as above |
| An unlink failure in F7 | not reported; the mutation proceeds | usable | new generation; the file stays as garbage and is retried later |
| Old WAL close failure in F7 | not reported | usable | new generation (the old WAL was already synced) |
| Directory fsync failure in F7 | IOError; the mutation is not attempted | NeedsRecovery; the fsync is not retried | new generation |
| An exception in F7 | propagated | NeedsRecovery | new generation |

A reopened handle after any of these preserves every acknowledged
mutation. A reopen validates only what the filesystem returns. It does
not prove that a real device writeback error was repaired (design.md
section 5).

## 11. Read path errors

Get reads SST blocks through the same syscall seam. A Get-time read error
or block corruption returns that status, clears the output, and does not
invalidate the handle, because nothing durable changed. Each probe opens
the SST read-only, reads one block with pread, and closes it. No
descriptor is held per SST between calls.

## 12. Syscall mediation (seam inventory)

Every filesystem operation goes through `PosixSyscalls` (injectable
through `DBOpener`): open, read, pread, write, fstat, ftruncate, fsync
(files and directories), close, rename, unlink, lstat (`InspectPath`),
directory listing (`ListDirectory`), mkdir (`CreateDirectory`), and the
open, flock, and close of `LOCK` (`LockFile`). The engine also reports
named non-syscall boundaries through `PosixSyscalls::Checkpoint`
(docs/m3-design.md section 2).
