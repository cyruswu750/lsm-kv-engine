#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "testing/wal_builder.h"

namespace kv::test {

// Builds SST version 1 bytes (docs/sst-format.md section 3) without the
// production codec, CRC, or little-endian helpers. Each layer exposes its
// fields so tests can craft malformed files with correct checksums.

struct RawSstEntry {
    std::string key;
    std::uint64_t sequence = 1;
    std::uint8_t type = kRawPut;
    std::string value;
    // When set, written instead of the real lengths.
    bool override_lengths = false;
    std::uint32_t key_len = 0;
    std::uint32_t value_len = 0;
};

inline std::string RawSstEntryBytes(const RawSstEntry& e) {
    std::string out;
    AppendLE(out, e.override_lengths ? e.key_len : e.key.size(), 4);
    AppendLE(out, e.override_lengths ? e.value_len : e.value.size(), 4);
    AppendLE(out, e.sequence, 8);
    out.push_back(static_cast<char>(e.type));
    out += e.key;
    out += e.value;
    return out;
}

// entries_bytes followed by count and a correct CRC.
inline std::string SealBlock(const std::string& entries_bytes,
                             std::uint32_t count) {
    std::string out = entries_bytes;
    AppendLE(out, count, 4);
    AppendLE(out, RefCrc32c(out), 4);
    return out;
}

inline std::string RawBlock(const std::vector<RawSstEntry>& entries) {
    std::string bytes;
    for (const RawSstEntry& e : entries) {
        bytes += RawSstEntryBytes(e);
    }
    return SealBlock(bytes, static_cast<std::uint32_t>(entries.size()));
}

struct RawIndexEntry {
    std::string last_key;
    std::uint64_t offset = 0;
    std::uint32_t length = 0;
    bool override_key_len = false;
    std::uint32_t key_len = 0;
};

inline std::string RawIndexBytes(const std::vector<RawIndexEntry>& index,
                                 bool override_count = false,
                                 std::uint32_t count = 0) {
    std::string out;
    for (const RawIndexEntry& e : index) {
        AppendLE(out, e.override_key_len ? e.key_len : e.last_key.size(), 4);
        AppendLE(out, e.offset, 8);
        AppendLE(out, e.length, 4);
        out += e.last_key;
    }
    AppendLE(out, override_count ? count : index.size(), 4);
    AppendLE(out, RefCrc32c(out), 4);
    return out;
}

struct RawFooter {
    std::uint64_t index_offset = 0;
    std::uint64_t index_length = 0;
    std::uint64_t filter_offset = 0;
    std::uint64_t filter_length = 0;
    std::uint32_t version = 1;
    std::string magic = "KVSST001";
};

inline std::string RawFooterBytes(const RawFooter& f) {
    std::string covered;
    AppendLE(covered, f.index_offset, 8);
    AppendLE(covered, f.index_length, 8);
    AppendLE(covered, f.filter_offset, 8);
    AppendLE(covered, f.filter_length, 8);
    AppendLE(covered, f.version, 4);
    covered += f.magic;
    std::string out;
    AppendLE(out, RefCrc32c(covered), 4);
    return out + covered;
}

// A valid SST: one block per inner vector, a matching index and footer.
inline std::string RawSst(const std::vector<std::vector<RawSstEntry>>& blocks) {
    std::string data;
    std::vector<RawIndexEntry> index;
    for (const auto& block : blocks) {
        const std::string bytes = RawBlock(block);
        index.push_back({block.back().key, data.size(),
                         static_cast<std::uint32_t>(bytes.size())});
        data += bytes;
    }
    const std::string index_bytes = RawIndexBytes(index);
    RawFooter footer;
    footer.index_offset = data.size();
    footer.index_length = index_bytes.size();
    return data + index_bytes + RawFooterBytes(footer);
}

}  // namespace kv::test
