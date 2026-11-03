#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "memtable.h"
#include "posix_file.h"
#include "status.h"
#include "types.h"
#include "writable_file.h"

namespace kv {

// SSTable version 1 (docs/sst-format.md section 3).

inline constexpr std::string_view kSstMagic{"KVSST001", 8};
inline constexpr std::uint32_t kSstVersion = 1;
inline constexpr std::size_t kSstBlockTarget = 4096;
inline constexpr std::size_t kSstEntryHeaderSize = 17;
inline constexpr std::size_t kSstBlockTrailerSize = 8;
inline constexpr std::size_t kSstIndexEntryHeaderSize = 16;
inline constexpr std::size_t kSstIndexTrailerSize = 8;
inline constexpr std::size_t kSstFooterSize = 48;
inline constexpr std::size_t kMinSstBlockSize =
    kSstEntryHeaderSize + kSstBlockTrailerSize;
inline constexpr std::size_t kMaxSstBlockSize =
    kSstEntryHeaderSize + kMaxKeyLength + kMaxValueLength +
    kSstBlockTrailerSize;
inline constexpr std::size_t kMinSstIndexSize =
    kSstIndexEntryHeaderSize + kSstIndexTrailerSize;
inline constexpr std::uint64_t kMinSstFileSize =
    kMinSstBlockSize + kMinSstIndexSize + kSstFooterSize;
// Format limits enforced by both the writer and the reader. The index
// size includes its trailer.
inline constexpr std::uint32_t kMaxSstBlocks = 1u << 20;
inline constexpr std::uint64_t kMaxSstIndexSize = 64u << 20;

// One entry of a validated block. The views point into a block buffer
// owned by whoever parsed it.
struct SstEntry {
    std::string_view key;
    std::uint64_t sequence = 0;
    ValueType type = ValueType::kPut;
    std::string_view value;
};

struct SstBlockHandle {
    std::string last_key;
    std::uint64_t offset = 0;
    std::uint32_t length = 0;
};

// What a reader keeps in memory for one SST.
struct SstSummary {
    std::uint64_t file_size = 0;
    std::uint64_t min_sequence = 0;
    std::uint64_t max_sequence = 0;
    std::string smallest_key;
    std::vector<SstBlockHandle> index;

    // The last key of the last block. A valid SST has at least one block.
    const std::string& largest_key() const {
        return index.back().last_key;
    }
};

// Writer limits. Production uses the format limits; tests lower them to
// reach the boundary with small inputs. Larger values are clamped to the
// format limits, so the builder never writes an SST that Open rejects.
struct SstLimits {
    std::uint32_t max_blocks = kMaxSstBlocks;
    std::uint64_t max_index_size = kMaxSstIndexSize;
};

// Streams entries into file as data blocks, then the index and footer.
// It never syncs, closes, or renames the file; the caller publishes it.
// After any error the builder and the file contents are unusable.
class SstBuilder {
public:
    explicit SstBuilder(WritableFile& file, SstLimits limits = {});

    SstBuilder(const SstBuilder&) = delete;
    SstBuilder& operator=(const SstBuilder&) = delete;

    // Keys must be strictly increasing in BytewiseLess order. Invalid
    // input (order, lengths, zero sequence, type, Delete with a value)
    // returns InvalidArgument without writing. When this entry closes the
    // current block, that block is checked against the limits first; see
    // Finish.
    Status Add(std::string_view key,
               std::uint64_t sequence,
               ValueType type,
               std::string_view value);

    // True if Add(key, value) followed by Finish would stay within the
    // limits, so a caller can start a new SST instead. Ignores the other
    // Add rules (order, lengths). An empty builder accepts any legal entry
    // under the format limits.
    bool CanAdd(std::string_view key, std::string_view value) const;

    // Writes the final block, the index, and the footer. An SST must hold
    // at least one entry (InvalidArgument otherwise). A block that would
    // make the SST exceed limits.max_blocks blocks, or its index (trailer
    // included) exceed limits.max_index_size bytes, returns InvalidArgument
    // before that block is written, here or in the Add that closes it.
    Status Finish();

    // Valid after a successful Finish. The summary may be moved out once.
    SstSummary& summary() {
        return summary_;
    }

private:
    Status FlushBlock();

    WritableFile& file_;
    SstLimits limits_;
    std::uint64_t index_size_ = kSstIndexTrailerSize;  // encoded so far
    std::string block_;
    std::uint32_t block_entries_ = 0;
    std::uint64_t offset_ = 0;
    std::string last_key_;
    bool has_entries_ = false;
    bool finished_ = false;
    SstSummary summary_;
};

// Writes every memtable entry, tombstones included, through builder and
// finishes it.
Status BuildSstFromMemtable(const Memtable& memtable, SstBuilder& builder);

// A validated SST. Get opens the file for each probe, so no descriptor
// is held between calls.
class SstTable {
public:
    SstTable(std::string path, PosixSyscalls& syscalls, SstSummary summary)
        : path_(std::move(path)),
          syscalls_(&syscalls),
          summary_(std::move(summary)) {}

    // Opens path and validates it completely by streaming every block
    // (docs/sst-format.md section 3.4, steps 1-4). The file size must equal
    // expected_size. Corruption for any size, structure, or checksum
    // error; UnsupportedFormat for another version; IOError for read
    // errors. On failure out stays empty.
    static Status Open(const std::filesystem::path& path,
                       std::uint64_t expected_size,
                       PosixSyscalls& syscalls,
                       std::unique_ptr<SstTable>& out);

    const SstSummary& summary() const {
        return summary_;
    }

    const std::string& path() const {
        return path_;
    }

    // Probes the one block that may hold key and validates it again. On
    // kFound, *value receives the value; otherwise it is unchanged.
    Status Get(std::string_view key,
               LookupResult& result,
               std::string* value) const;

private:
    friend class SstIterator;

    std::string path_;
    PosixSyscalls* syscalls_;
    SstSummary summary_;
};

// Streams every entry of a validated SST in key order, reading and
// validating one block at a time exactly as SstTable::Open does
// (docs/sst-format.md section 3.4, step 4). Memory is one block. The
// iterator holds one read-only descriptor until Close or destruction; the
// table must outlive it. After any error the iterator is no longer Valid.
class SstIterator {
public:
    SstIterator(const SstIterator&) = delete;
    SstIterator& operator=(const SstIterator&) = delete;

    // Opens the table's file and positions at its first entry. out must
    // be empty; on failure it stays empty.
    static Status Open(const SstTable& table,
                       std::unique_ptr<SstIterator>& out);

    bool Valid() const {
        return position_ < entries_.size();
    }

    // Requires Valid(). The views stay valid until the next Next or Close.
    const SstEntry& entry() const {
        return entries_[position_];
    }

    // Requires Valid(). Moves to the next entry, reading the next block
    // when needed. A read error (IOError) or a block failing validation
    // (Corruption) is returned, and the iterator stops.
    Status Next();

    // Releases the descriptor; later calls return Ok.
    Status Close();

private:
    explicit SstIterator(const SstTable& table) : table_(&table) {}

    Status LoadBlock(std::size_t i);

    const SstTable* table_;
    std::unique_ptr<RandomAccessFile> file_;
    std::string buffer_;
    std::vector<SstEntry> entries_;
    std::size_t position_ = 0;
    std::size_t block_ = 0;
};

}  // namespace kv
