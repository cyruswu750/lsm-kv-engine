#include "sst.h"

#include <algorithm>
#include <utility>

#include "bloom.h"
#include "coding.h"
#include "crc32c.h"

namespace kv {

namespace {

bool IsValidType(std::uint8_t type) {
    return type == static_cast<std::uint8_t>(ValueType::kPut) ||
           type == static_cast<std::uint8_t>(ValueType::kDelete);
}

std::string BlockContext(std::uint64_t offset) {
    return "block at offset " + std::to_string(offset) + ": ";
}

// Validates one complete data block (entries and trailer) and returns
// views into it. Checks lengths against the block before slicing, so
// a malformed length can never read or allocate past the block.
Status ParseBlock(std::string_view block, std::vector<SstEntry>& out) {
    if (block.size() < kMinSstBlockSize) {
        return Status::Corruption("block length " +
                                  std::to_string(block.size()) +
                                  " is below the minimum");
    }
    const std::size_t crc_offset = block.size() - 4;
    const std::uint32_t stored = DecodeFixed32(block.data() + crc_offset);
    const std::uint32_t actual = Crc32c(block.substr(0, crc_offset));
    if (stored != actual) {
        return Status::Corruption("block checksum mismatch");
    }

    const std::uint32_t count = DecodeFixed32(block.data() + crc_offset - 4);
    if (count == 0) {
        return Status::Corruption("block has no entries");
    }
    const std::size_t entries_size = block.size() - kSstBlockTrailerSize;
    if (entries_size > kSstBlockTarget && count != 1) {
        return Status::Corruption("oversized block holds " +
                                  std::to_string(count) +
                                  " entries; only one is allowed");
    }

    std::vector<SstEntry> entries;
    std::size_t pos = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::string context =
            "entry " + std::to_string(i) + " at block offset " +
            std::to_string(pos) + ": ";
        if (entries_size - pos < kSstEntryHeaderSize) {
            return Status::Corruption(context + "header extends past entries");
        }
        const char* h = block.data() + pos;
        const std::uint32_t key_len = DecodeFixed32(h);
        const std::uint32_t value_len = DecodeFixed32(h + 4);
        const std::uint64_t sequence = DecodeFixed64(h + 8);
        const auto raw_type = static_cast<std::uint8_t>(h[16]);

        if (key_len > kMaxKeyLength) {
            return Status::Corruption(context + "key length " +
                                      std::to_string(key_len) +
                                      " exceeds limit");
        }
        if (value_len > kMaxValueLength) {
            return Status::Corruption(context + "value length " +
                                      std::to_string(value_len) +
                                      " exceeds limit");
        }
        if (!IsValidType(raw_type)) {
            return Status::Corruption(context + "invalid type " +
                                      std::to_string(raw_type));
        }
        const auto type = static_cast<ValueType>(raw_type);
        if (type == ValueType::kDelete && value_len != 0) {
            return Status::Corruption(context + "Delete has value length " +
                                      std::to_string(value_len));
        }
        if (sequence == 0) {
            return Status::Corruption(context + "sequence is zero");
        }

        // Both lengths are bounded, so the sum cannot overflow.
        const std::size_t size = kSstEntryHeaderSize + key_len + value_len;
        if (size > entries_size - pos) {
            return Status::Corruption(context + "entry extends past entries");
        }
        SstEntry entry;
        entry.key = block.substr(pos + kSstEntryHeaderSize, key_len);
        entry.value =
            block.substr(pos + kSstEntryHeaderSize + key_len, value_len);
        entry.sequence = sequence;
        entry.type = type;
        if (!entries.empty() && !BytewiseLess{}(entries.back().key, entry.key)) {
            return Status::Corruption(context + "key is not greater than "
                                                "the previous key");
        }
        entries.push_back(entry);
        pos += size;
    }
    if (pos != entries_size) {
        return Status::Corruption(
            "block has " + std::to_string(entries_size - pos) +
            " bytes after its last entry");
    }

    out = std::move(entries);
    return Status::Ok();
}

struct Footer {
    std::uint32_t version = 0;
    std::uint64_t data_end = 0;  // the data region size D
    std::uint64_t filter_offset = 0;
    std::uint64_t filter_length = 0;
    std::uint64_t index_offset = 0;
    std::uint64_t index_length = 0;
};

std::string Regions(const Footer& f, std::uint64_t file_size) {
    return "filter offset " + std::to_string(f.filter_offset) +
           ", filter length " + std::to_string(f.filter_length) +
           ", index offset " + std::to_string(f.index_offset) +
           ", index length " + std::to_string(f.index_length) +
           ", file size " + std::to_string(file_size);
}

Status DecodeFooter(std::string_view footer,
                    std::uint64_t file_size,
                    Footer& out) {
    if (footer.substr(40, 8) != kSstMagic) {
        return Status::Corruption("footer has bad magic");
    }
    const std::uint32_t version = DecodeFixed32(footer.data() + 36);
    if (version != kSstVersion1 && version != kSstVersion2) {
        return Status::UnsupportedFormat("unsupported SST version " +
                                         std::to_string(version));
    }
    if (DecodeFixed32(footer.data()) != Crc32c(footer.substr(4))) {
        return Status::Corruption("footer checksum mismatch");
    }

    Footer f;
    f.version = version;
    f.index_offset = DecodeFixed64(footer.data() + 4);
    f.index_length = DecodeFixed64(footer.data() + 12);
    f.filter_offset = DecodeFixed64(footer.data() + 20);
    f.filter_length = DecodeFixed64(footer.data() + 28);

    // file_size >= kMinSstFileSize was checked by the caller. Each operand
    // is checked against before_footer first, so no sum can overflow.
    const std::uint64_t before_footer = file_size - kSstFooterSize;
    bool consistent;
    if (version == kSstVersion1) {
        if (f.filter_offset != 0 || f.filter_length != 0) {
            return Status::Corruption("version 1 footer has a filter region");
        }
        f.data_end = f.index_offset;
        consistent = f.index_offset >= kMinSstBlockSize &&
                     f.index_offset <= before_footer &&
                     f.index_length == before_footer - f.index_offset;
    } else {
        if (f.filter_length < kMinSstFilterSize ||
            f.filter_length > kMaxSstFilterSize) {
            return Status::Corruption("filter length " +
                                      std::to_string(f.filter_length) +
                                      " is out of range");
        }
        f.data_end = f.filter_offset;
        consistent = f.filter_offset >= kMinSstBlockSize &&
                     f.filter_offset <= before_footer &&
                     f.filter_length <= before_footer - f.filter_offset &&
                     f.index_offset == f.filter_offset + f.filter_length &&
                     f.index_length == before_footer - f.index_offset;
    }
    if (!consistent) {
        return Status::Corruption("footer regions are inconsistent: " +
                                  Regions(f, file_size));
    }
    if (f.index_length < kMinSstIndexSize) {
        return Status::Corruption("index length " +
                                  std::to_string(f.index_length) +
                                  " is below the minimum");
    }
    // Reject before the index is allocated. Every block takes at least
    // kMinSstBlockSize bytes of the data region, and every index entry at
    // most kSstIndexEntryHeaderSize + kMaxKeyLength bytes. Capping the
    // block count first keeps the product far below 2^64.
    const std::uint64_t max_blocks = std::min<std::uint64_t>(
        f.data_end / kMinSstBlockSize, kMaxSstBlocks);
    const std::uint64_t max_index = std::min<std::uint64_t>(
        max_blocks * (kSstIndexEntryHeaderSize + kMaxKeyLength) +
            kSstIndexTrailerSize,
        kMaxSstIndexSize);
    if (f.index_length > max_index) {
        return Status::Corruption(
            "index length " + std::to_string(f.index_length) +
            " exceeds the limit " + std::to_string(max_index) +
            " for a data region of " + std::to_string(f.data_end) +
            " bytes");
    }
    out = f;
    return Status::Ok();
}

// Validates the index block. data_end is the data region size (the
// filter offset in version 2, the index offset in version 1); the blocks
// must tile [0, data_end) exactly.
Status DecodeIndex(std::string_view index,
                   std::uint64_t data_end,
                   std::vector<SstBlockHandle>& out) {
    const std::size_t crc_offset = index.size() - 4;
    if (DecodeFixed32(index.data() + crc_offset) !=
        Crc32c(index.substr(0, crc_offset))) {
        return Status::Corruption("index checksum mismatch");
    }
    const std::uint32_t count = DecodeFixed32(index.data() + crc_offset - 4);
    if (count == 0) {
        return Status::Corruption("index has no blocks");
    }
    if (count > kMaxSstBlocks) {
        return Status::Corruption("index has " + std::to_string(count) +
                                  " blocks; the limit is " +
                                  std::to_string(kMaxSstBlocks));
    }

    const std::size_t entries_size = index.size() - kSstIndexTrailerSize;
    std::vector<SstBlockHandle> handles;
    std::size_t pos = 0;
    std::uint64_t expected_offset = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::string context = "index entry " + std::to_string(i) + ": ";
        if (entries_size - pos < kSstIndexEntryHeaderSize) {
            return Status::Corruption(context + "extends past the index");
        }
        const char* h = index.data() + pos;
        const std::uint32_t key_len = DecodeFixed32(h);
        const std::uint64_t offset = DecodeFixed64(h + 4);
        const std::uint32_t length = DecodeFixed32(h + 12);
        if (key_len > kMaxKeyLength) {
            return Status::Corruption(context + "key length " +
                                      std::to_string(key_len) +
                                      " exceeds limit");
        }
        if (key_len > entries_size - pos - kSstIndexEntryHeaderSize) {
            return Status::Corruption(context + "key extends past the index");
        }
        if (offset != expected_offset) {
            return Status::Corruption(context + "block offset " +
                                      std::to_string(offset) + ", expected " +
                                      std::to_string(expected_offset));
        }
        if (length < kMinSstBlockSize || length > kMaxSstBlockSize ||
            length > data_end - offset) {
            return Status::Corruption(context + "block length " +
                                      std::to_string(length) +
                                      " is out of range");
        }

        SstBlockHandle handle;
        handle.last_key.assign(h + kSstIndexEntryHeaderSize, key_len);
        handle.offset = offset;
        handle.length = length;
        if (!handles.empty() &&
            !BytewiseLess{}(handles.back().last_key, handle.last_key)) {
            return Status::Corruption(context + "last key is not greater "
                                                "than the previous one");
        }
        handles.push_back(std::move(handle));
        expected_offset = offset + length;
        pos += kSstIndexEntryHeaderSize + key_len;
    }
    if (pos != entries_size) {
        return Status::Corruption("index has " +
                                  std::to_string(entries_size - pos) +
                                  " bytes after its last entry");
    }
    if (expected_offset != data_end) {
        return Status::Corruption("blocks end at " +
                                  std::to_string(expected_offset) +
                                  " but the index starts at " +
                                  std::to_string(data_end));
    }
    out = std::move(handles);
    return Status::Ok();
}

Status WithPath(const std::string& path, const Status& s) {
    return Status{s.code, path + ": " + s.message};
}

// Reads and validates one block described by handle.
Status ReadBlock(const RandomAccessFile& file,
                 const SstBlockHandle& handle,
                 std::string& buffer,
                 std::vector<SstEntry>& entries) {
    buffer.resize(handle.length);
    Status s = file.Read(handle.offset, handle.length, buffer.data());
    if (!s.ok()) {
        return s;
    }
    s = ParseBlock(buffer, entries);
    if (!s.ok()) {
        return Status::Corruption(file.path() + ": " +
                                  BlockContext(handle.offset) + s.message);
    }
    if (entries.back().key != handle.last_key) {
        return Status::Corruption(file.path() + ": " +
                                  BlockContext(handle.offset) +
                                  "last key differs from the index");
    }
    return Status::Ok();
}

}  // namespace

SstBuilder::SstBuilder(WritableFile& file,
                       SstLimits limits,
                       std::uint32_t bloom_bits_per_key)
    : file_(file),
      limits_{std::min(limits.max_blocks, kMaxSstBlocks),
              std::min(limits.max_index_size, kMaxSstIndexSize),
              std::min(limits.max_filter_keys, kMaxSstFilterKeys)},
      bits_per_key_(std::min(bloom_bits_per_key, kMaxFilterBitsPerKey)),
      filter_(std::max<std::uint32_t>(bits_per_key_, 1)) {}

Status SstBuilder::Add(std::string_view key,
                       std::uint64_t sequence,
                       ValueType type,
                       std::string_view value) {
    if (finished_) {
        return Status::InvalidArgument("SST builder is already finished");
    }
    Status s = ValidateKeyValue(key, value);
    if (!s.ok()) {
        return s;
    }
    if (sequence == 0) {
        return Status::InvalidArgument("SST entry sequence must be nonzero");
    }
    if (!IsValidType(static_cast<std::uint8_t>(type))) {
        return Status::InvalidArgument("invalid SST entry type");
    }
    if (type == ValueType::kDelete && !value.empty()) {
        return Status::InvalidArgument("Delete entry must have an empty value");
    }
    if (has_entries_ && !BytewiseLess{}(last_key_, key)) {
        return Status::InvalidArgument("SST keys must be strictly increasing");
    }
    if (bits_per_key_ > 0 && filter_.key_count() >= limits_.max_filter_keys) {
        return Status::InvalidArgument(
            "SST would exceed " + std::to_string(limits_.max_filter_keys) +
            " filter keys");
    }

    const std::size_t size = kSstEntryHeaderSize + key.size() + value.size();
    if (block_entries_ > 0 && block_.size() + size > kSstBlockTarget) {
        s = FlushBlock();
        if (!s.ok()) {
            return s;
        }
    }

    PutFixed32(&block_, static_cast<std::uint32_t>(key.size()));
    PutFixed32(&block_, static_cast<std::uint32_t>(value.size()));
    PutFixed64(&block_, sequence);
    block_.push_back(static_cast<char>(type));
    block_.append(key);
    block_.append(value);
    ++block_entries_;

    if (!has_entries_) {
        summary_.smallest_key.assign(key);
        summary_.min_sequence = sequence;
        summary_.max_sequence = sequence;
    }
    summary_.min_sequence = std::min(summary_.min_sequence, sequence);
    summary_.max_sequence = std::max(summary_.max_sequence, sequence);
    last_key_.assign(key);
    has_entries_ = true;
    if (bits_per_key_ > 0) {
        filter_.AddKey(key);
    }
    return Status::Ok();
}

bool SstBuilder::CanAdd(std::string_view key, std::string_view value) const {
    // Mirrors Add: the entry either joins the current block, which then ends
    // with this key, or closes it (indexed by last_key_) and starts a new
    // block ending with this key. Finish would then index the last block.
    const std::size_t size = kSstEntryHeaderSize + key.size() + value.size();
    const bool new_block =
        block_entries_ > 0 && block_.size() + size > kSstBlockTarget;
    std::uint64_t blocks = summary_.index.size() + 1;
    std::uint64_t index_size =
        index_size_ + kSstIndexEntryHeaderSize + key.size();
    if (new_block) {
        blocks += 1;
        index_size += kSstIndexEntryHeaderSize + last_key_.size();
    }
    return blocks <= limits_.max_blocks &&
           index_size <= limits_.max_index_size &&
           (bits_per_key_ == 0 ||
            filter_.key_count() < limits_.max_filter_keys);
}

// Checks the limits before the block is written, so the index never grows
// past what SstTable::Open accepts. Checking each block as it is closed,
// with its final last key, makes the limits exact.
Status SstBuilder::FlushBlock() {
    if (summary_.index.size() >= limits_.max_blocks) {
        return Status::InvalidArgument(
            "SST would exceed " + std::to_string(limits_.max_blocks) +
            " blocks");
    }
    const std::uint64_t index_size =
        index_size_ + kSstIndexEntryHeaderSize + last_key_.size();
    if (index_size > limits_.max_index_size) {
        return Status::InvalidArgument(
            "SST index would exceed " +
            std::to_string(limits_.max_index_size) + " bytes");
    }
    index_size_ = index_size;
    PutFixed32(&block_, block_entries_);
    PutFixed32(&block_, Crc32c(block_));

    SstBlockHandle handle;
    handle.last_key = last_key_;
    handle.offset = offset_;
    handle.length = static_cast<std::uint32_t>(block_.size());
    summary_.index.push_back(std::move(handle));

    Status s = file_.Append(block_);
    if (!s.ok()) {
        return s;
    }
    offset_ += block_.size();
    block_.clear();
    block_entries_ = 0;
    return Status::Ok();
}

Status SstBuilder::Finish() {
    if (finished_) {
        return Status::InvalidArgument("SST builder is already finished");
    }
    if (!has_entries_) {
        return Status::InvalidArgument("an SST must contain an entry");
    }
    Status s = FlushBlock();
    if (!s.ok()) {
        return s;
    }

    std::string index;
    for (const SstBlockHandle& handle : summary_.index) {
        PutFixed32(&index, static_cast<std::uint32_t>(handle.last_key.size()));
        PutFixed64(&index, handle.offset);
        PutFixed32(&index, handle.length);
        index.append(handle.last_key);
    }
    PutFixed32(&index, static_cast<std::uint32_t>(summary_.index.size()));
    PutFixed32(&index, Crc32c(index));

    // Version 2 places the filter between the data blocks and the index.
    std::string filter;
    if (bits_per_key_ > 0) {
        filter_.Encode(&filter);
        s = BloomFilter::Decode(filter, offset_, summary_.filter);
        if (!s.ok()) {
            return s;
        }
    }
    const std::uint64_t index_offset = offset_ + filter.size();

    std::string footer(4, '\0');
    PutFixed64(&footer, index_offset);
    PutFixed64(&footer, index.size());
    PutFixed64(&footer, filter.empty() ? 0 : offset_);  // 0: version 1
    PutFixed64(&footer, filter.size());
    PutFixed32(&footer, filter.empty() ? kSstVersion1 : kSstVersion2);
    footer.append(kSstMagic);
    EncodeFixed32(footer.data(), Crc32c(std::string_view(footer).substr(4)));

    if (!filter.empty()) {
        s = file_.Append(filter);
    }
    if (s.ok()) {
        s = file_.Append(index);
    }
    if (s.ok()) {
        s = file_.Append(footer);
    }
    if (!s.ok()) {
        return s;
    }
    summary_.version = filter.empty() ? kSstVersion1 : kSstVersion2;
    summary_.file_size = index_offset + index.size() + footer.size();
    finished_ = true;
    return Status::Ok();
}

Status BuildSstFromMemtable(const Memtable& memtable, SstBuilder& builder) {
    for (const auto& [key, entry] : memtable) {
        const ValueType type =
            entry.is_tombstone ? ValueType::kDelete : ValueType::kPut;
        Status s = builder.Add(key, entry.sequence, type, entry.value);
        if (!s.ok()) {
            return s;
        }
    }
    return builder.Finish();
}

Status SstTable::Open(const std::filesystem::path& path,
                      std::uint64_t expected_size,
                      PosixSyscalls& syscalls,
                      std::unique_ptr<SstTable>& out) {
    if (out) {
        return Status::InvalidArgument(path.string() +
                                       ": SST output is not empty");
    }
    std::unique_ptr<RandomAccessFile> file;
    Status s = RandomAccessFile::Open(path, syscalls, file);
    if (!s.ok()) {
        return s;
    }
    const std::string& name = file->path();
    const std::uint64_t size = file->size();
    if (size != expected_size) {
        return Status::Corruption(name + ": size " + std::to_string(size) +
                                  " differs from the expected " +
                                  std::to_string(expected_size));
    }
    if (size < kMinSstFileSize) {
        return Status::Corruption(name + ": size " + std::to_string(size) +
                                  " is below the minimum SST size");
    }

    std::string footer(kSstFooterSize, '\0');
    s = file->Read(size - kSstFooterSize, kSstFooterSize, footer.data());
    if (!s.ok()) {
        return s;
    }
    Footer f;
    s = DecodeFooter(footer, size, f);
    if (!s.ok()) {
        return WithPath(name, s);
    }

    SstSummary summary;
    summary.version = f.version;
    if (f.version == kSstVersion2) {
        // The header's bounds are checked before the bit array is
        // allocated; DecodeFooter bounded filter_length.
        std::string header(kFilterHeaderSize, '\0');
        s = file->Read(f.filter_offset, header.size(), header.data());
        if (!s.ok()) {
            return s;
        }
        s = BloomFilter::CheckHeader(header, f.filter_length, f.data_end);
        if (!s.ok()) {
            return WithPath(name, s);
        }
        std::string block(static_cast<std::size_t>(f.filter_length), '\0');
        s = file->Read(f.filter_offset, block.size(), block.data());
        if (!s.ok()) {
            return s;
        }
        s = BloomFilter::Decode(block, f.data_end, summary.filter);
        if (!s.ok()) {
            return WithPath(name, s);
        }
    }

    // DecodeFooter bounded index_length by the data region and
    // kMaxSstIndexSize.
    std::string index(static_cast<std::size_t>(f.index_length), '\0');
    s = file->Read(f.index_offset, index.size(), index.data());
    if (!s.ok()) {
        return s;
    }
    s = DecodeIndex(index, f.data_end, summary.index);
    if (!s.ok()) {
        return WithPath(name, s);
    }

    // Stream every block once; only one block is held at a time.
    std::string buffer;
    std::string previous_key;
    bool first = true;
    std::uint64_t entry_count = 0;
    for (const SstBlockHandle& handle : summary.index) {
        std::vector<SstEntry> entries;
        s = ReadBlock(*file, handle, buffer, entries);
        if (!s.ok()) {
            return s;
        }
        if (!first && !BytewiseLess{}(previous_key, entries.front().key)) {
            return Status::Corruption(name + ": " +
                                      BlockContext(handle.offset) +
                                      "first key is not greater than the "
                                      "previous block's last key");
        }
        for (const SstEntry& entry : entries) {
            // A filter that would deny a stored key is damaged; it must
            // never be used as a negative answer.
            if (!summary.filter.empty() &&
                !summary.filter.MayContain(entry.key)) {
                return Status::Corruption(name + ": " +
                                          BlockContext(handle.offset) +
                                          "the filter does not contain a "
                                          "stored key");
            }
            ++entry_count;
            if (first) {
                summary.smallest_key.assign(entry.key);
                summary.min_sequence = entry.sequence;
                summary.max_sequence = entry.sequence;
                first = false;
            }
            summary.min_sequence = std::min(summary.min_sequence,
                                            entry.sequence);
            summary.max_sequence = std::max(summary.max_sequence,
                                            entry.sequence);
        }
        previous_key.assign(entries.back().key);
    }

    if (!summary.filter.empty() &&
        entry_count != summary.filter.key_count()) {
        return Status::Corruption(
            name + ": the filter counts " +
            std::to_string(summary.filter.key_count()) + " keys, but the SST "
            "holds " + std::to_string(entry_count));
    }

    s = file->Close();
    if (!s.ok()) {
        return s;
    }
    summary.file_size = size;
    out = std::make_unique<SstTable>(name, syscalls, std::move(summary));
    return Status::Ok();
}

Status SstTable::Get(std::string_view key,
                     LookupResult& result,
                     std::string* value,
                     SstReadStats* stats,
                     bool use_filter) const {
    const auto& index = summary_.index;
    if (BytewiseLess{}(key, summary_.smallest_key) ||
        BytewiseLess{}(summary_.largest_key(), key)) {
        if (stats != nullptr) {
            ++stats->range_rejections;
        }
        result = LookupResult::kAbsent;
        return Status::Ok();
    }
    // Every stored key, tombstones included, is in the filter, so a
    // negative answer proves this SST holds no version of the key.
    if (use_filter && !summary_.filter.empty() &&
        !summary_.filter.MayContain(key)) {
        if (stats != nullptr) {
            ++stats->filter_rejections;
        }
        result = LookupResult::kAbsent;
        return Status::Ok();
    }
    // key <= the largest key, so some block's last key is >= key.
    const auto it = std::lower_bound(
        index.begin(), index.end(), key,
        [](const SstBlockHandle& handle, std::string_view k) {
            return BytewiseLess{}(handle.last_key, k);
        });
    if (stats != nullptr) {
        ++stats->block_reads;
        stats->block_bytes += it->length;
    }

    std::unique_ptr<RandomAccessFile> file;
    Status s = RandomAccessFile::Open(path_, *syscalls_, file);
    if (!s.ok()) {
        return s;
    }
    std::string buffer;
    std::vector<SstEntry> entries;
    s = ReadBlock(*file, *it, buffer, entries);
    if (!s.ok()) {
        return s;
    }
    s = file->Close();
    if (!s.ok()) {
        return s;
    }

    const auto match = std::lower_bound(
        entries.begin(), entries.end(), key,
        [](const SstEntry& e, std::string_view k) {
            return BytewiseLess{}(e.key, k);
        });
    if (match == entries.end() || match->key != key) {
        result = LookupResult::kAbsent;
    } else if (match->type == ValueType::kDelete) {
        result = LookupResult::kDeleted;
    } else {
        value->assign(match->value);
        result = LookupResult::kFound;
    }
    return Status::Ok();
}

Status SstIterator::Open(const SstTable& table,
                         std::unique_ptr<SstIterator>& out) {
    if (out) {
        return Status::InvalidArgument(table.path() +
                                       ": SST iterator output is not empty");
    }
    std::unique_ptr<SstIterator> it(new SstIterator(table));
    Status s = RandomAccessFile::Open(table.path(), *table.syscalls_, it->file_);
    if (!s.ok()) {
        return s;
    }
    if (it->file_->size() != table.summary().file_size) {
        return Status::Corruption(table.path() + ": size " +
                                  std::to_string(it->file_->size()) +
                                  " differs from the expected " +
                                  std::to_string(table.summary().file_size));
    }
    s = it->LoadBlock(0);
    if (!s.ok()) {
        return s;
    }
    out = std::move(it);
    return Status::Ok();
}

// Reads and validates block i (as at Open), or ends the iteration when i
// is past the last block.
Status SstIterator::LoadBlock(std::size_t i) {
    const auto& index = table_->summary().index;
    entries_.clear();
    position_ = 0;
    block_ = i;
    if (i >= index.size()) {
        return Status::Ok();
    }
    Status s = ReadBlock(*file_, index[i], buffer_, entries_);
    if (!s.ok()) {
        entries_.clear();
        block_ = index.size();
        return s;
    }
    if (i > 0 && !BytewiseLess{}(index[i - 1].last_key, entries_.front().key)) {
        entries_.clear();
        block_ = index.size();
        return Status::Corruption(file_->path() + ": " +
                                  BlockContext(index[i].offset) +
                                  "first key is not greater than the "
                                  "previous block's last key");
    }
    return Status::Ok();
}

Status SstIterator::Next() {
    if (!Valid()) {
        return Status::InvalidArgument(table_->path() +
                                       ": SST iterator is not positioned");
    }
    if (++position_ < entries_.size()) {
        return Status::Ok();
    }
    return LoadBlock(block_ + 1);
}

Status SstIterator::Close() {
    entries_.clear();
    block_ = table_->summary().index.size();
    return file_ ? file_->Close() : Status::Ok();
}

}  // namespace kv
