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

// ---- Version 2 (with a Bloom filter, docs/sst-format.md section 3.7) ----
// KVHASH1 and the filter are written here independently of
// src/core/bloom.cc; their golden vectors come from
// scripts/bloom_reference.py.

inline std::uint64_t RawFnv1a64(std::string_view key) {
    std::uint64_t h = 14695981039346656037ull;  // 0xcbf29ce484222325
    for (unsigned char c : key) {
        h = (h ^ c) * 1099511628211ull;  // 0x100000001b3
    }
    return h;
}

inline std::uint64_t RawMix64(std::uint64_t z) {
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

struct RawFilter {
    std::uint32_t filter_version = 1;
    std::uint32_t hash_id = 1;
    std::uint32_t key_count = 0;
    std::uint32_t probes = 0;
    std::string bits;
    bool corrupt_crc = false;
};

// The filter the format prescribes for keys at bits_per_key.
inline RawFilter RawFilterFor(const std::vector<std::string>& keys,
                              std::uint32_t bits_per_key = 10) {
    RawFilter f;
    f.key_count = static_cast<std::uint32_t>(keys.size());
    std::uint32_t k = (bits_per_key * 69 + 50) / 100;
    f.probes = k < 1 ? 1 : (k > 30 ? 30 : k);
    std::size_t bytes = (keys.size() * bits_per_key + 7) / 8;
    if (bytes < 8) {
        bytes = 8;
    }
    f.bits.assign(bytes, '\0');
    const std::uint64_t m = 8 * bytes;
    for (const std::string& key : keys) {
        const std::uint64_t a = RawFnv1a64(key);
        const std::uint64_t h1 = RawMix64(a);
        const std::uint64_t h2 = RawMix64(a ^ 0x9e3779b97f4a7c15ull) | 1;
        for (std::uint32_t i = 0; i < f.probes; ++i) {
            const std::uint64_t b = (h1 + i * h2) % m;
            f.bits[b / 8] = static_cast<char>(f.bits[b / 8] | (1 << (b % 8)));
        }
    }
    return f;
}

inline std::string RawFilterBytes(const RawFilter& f) {
    std::string out;
    AppendLE(out, f.filter_version, 4);
    AppendLE(out, f.hash_id, 4);
    AppendLE(out, f.key_count, 4);
    AppendLE(out, f.probes, 4);
    out += f.bits;
    AppendLE(out, RefCrc32c(out) ^ (f.corrupt_crc ? 1u : 0u), 4);
    return out;
}

}  // namespace kv::test
