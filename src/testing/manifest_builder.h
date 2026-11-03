#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "testing/temp_dir.h"
#include "testing/wal_builder.h"

namespace kv::test {

// Builds and parses manifest bytes (docs/sst-format.md section 4; version
// 2 by default) without the production codec, CRC, or little-endian
// helpers, so tests can craft valid and malformed manifests independently
// and read a published manifest as evidence. Setting version = 1 builds the
// M2 layout, where offset 52 was reserved and zero.

struct RawManifestSst {
    std::uint64_t file_id = 0;
    std::uint64_t file_size = 0;
    std::uint64_t min_sequence = 0;
    std::uint64_t max_sequence = 0;
};

struct RawManifest {
    std::string magic = "KVMAN001";
    std::uint32_t version = 2;
    std::uint32_t reserved1 = 0;
    std::uint64_t generation = 1;
    std::uint64_t next_file_id = 2;
    std::uint64_t checkpoint = 0;
    std::uint64_t active_wal_id = 1;
    std::uint32_t base_run_count = 0;  // offset 52
    std::vector<RawManifestSst> ssts;
    // When set, written instead of ssts.size().
    bool override_count = false;
    std::uint32_t sst_count = 0;
};

// Bytes before the CRC.
inline std::string ManifestBody(const RawManifest& m) {
    std::string out = m.magic;
    AppendLE(out, m.version, 4);
    AppendLE(out, m.reserved1, 4);
    AppendLE(out, m.generation, 8);
    AppendLE(out, m.next_file_id, 8);
    AppendLE(out, m.checkpoint, 8);
    AppendLE(out, m.active_wal_id, 8);
    AppendLE(out, m.override_count ? m.sst_count : m.ssts.size(), 4);
    AppendLE(out, m.base_run_count, 4);
    for (const RawManifestSst& sst : m.ssts) {
        AppendLE(out, sst.file_id, 8);
        AppendLE(out, sst.file_size, 8);
        AppendLE(out, sst.min_sequence, 8);
        AppendLE(out, sst.max_sequence, 8);
    }
    return out;
}

// Complete manifest with a correct CRC, whatever the field values.
inline std::string ManifestBytes(const RawManifest& m) {
    std::string out = ManifestBody(m);
    AppendLE(out, RefCrc32c(out), 4);
    return out;
}

inline std::uint64_t ReadLE(std::string_view bytes,
                            std::size_t offset,
                            int width) {
    std::uint64_t value = 0;
    for (int i = 0; i < width; ++i) {
        value |= std::uint64_t{static_cast<unsigned char>(bytes[offset + i])}
                 << (8 * i);
    }
    return value;
}

// Parses a well-formed version 2 manifest (checks magic, version, size, and
// CRC only). Returns false for anything else.
inline bool ParseManifestBytes(std::string_view bytes, RawManifest& out) {
    if (bytes.size() < 60 || bytes.substr(0, 8) != "KVMAN001" ||
        ReadLE(bytes, 8, 4) != 2) {
        return false;
    }
    const std::uint64_t count = ReadLE(bytes, 48, 4);
    if (bytes.size() != 60 + 32 * count ||
        ReadLE(bytes, bytes.size() - 4, 4) !=
            RefCrc32c(bytes.substr(0, bytes.size() - 4))) {
        return false;
    }
    RawManifest m;
    m.reserved1 = static_cast<std::uint32_t>(ReadLE(bytes, 12, 4));
    m.generation = ReadLE(bytes, 16, 8);
    m.next_file_id = ReadLE(bytes, 24, 8);
    m.checkpoint = ReadLE(bytes, 32, 8);
    m.active_wal_id = ReadLE(bytes, 40, 8);
    m.base_run_count = static_cast<std::uint32_t>(ReadLE(bytes, 52, 4));
    for (std::uint64_t i = 0; i < count; ++i) {
        const std::size_t e = 56 + 32 * i;
        m.ssts.push_back({ReadLE(bytes, e, 8), ReadLE(bytes, e + 8, 8),
                          ReadLE(bytes, e + 16, 8), ReadLE(bytes, e + 24, 8)});
    }
    out = m;
    return true;
}

inline bool ReadManifest(const std::filesystem::path& db, RawManifest& out) {
    std::string bytes;
    return ReadBytes(db / "MANIFEST", bytes) && ParseManifestBytes(bytes, out);
}

// "wal-000007.log" / "sst-000007.sst", formatted independently.
inline std::string RawFileName(const char* prefix,
                               std::uint64_t id,
                               const char* suffix) {
    std::string digits = std::to_string(id);
    if (digits.size() < 6) {
        digits.insert(0, 6 - digits.size(), '0');
    }
    return std::string(prefix) + digits + suffix;
}

inline std::string RawWalName(std::uint64_t id) {
    return RawFileName("wal-", id, ".log");
}

inline std::string RawSstName(std::uint64_t id) {
    return RawFileName("sst-", id, ".sst");
}

// Creates db (if needed) holding the given active WAL bytes and a manifest
// that references it, as an M2 database whose state lives only in the WAL.
inline bool MakeWalOnlyDb(const std::filesystem::path& db,
                          std::string_view wal_bytes,
                          std::uint64_t checkpoint = 0,
                          std::uint64_t wal_id = 1) {
    std::error_code ec;
    std::filesystem::create_directory(db, ec);
    RawManifest m;
    m.checkpoint = checkpoint;
    m.active_wal_id = wal_id;
    m.next_file_id = wal_id + 1;
    return WriteBytes(db / RawWalName(wal_id), wal_bytes) &&
           WriteBytes(db / "MANIFEST", ManifestBytes(m));
}

}  // namespace kv::test
