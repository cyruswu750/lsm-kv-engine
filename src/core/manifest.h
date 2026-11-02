#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "posix_file.h"
#include "status.h"

namespace kv {

// Manifest version 1 (docs/sst-format.md section 4).

inline constexpr std::string_view kManifestMagic{"KVMAN001", 8};
inline constexpr std::uint32_t kManifestVersion = 1;
inline constexpr std::size_t kManifestHeaderSize = 56;
inline constexpr std::size_t kManifestSstEntrySize = 32;
inline constexpr std::size_t kManifestMinSize = kManifestHeaderSize + 4;
inline constexpr std::uint32_t kMaxManifestSsts = 1u << 20;
inline constexpr std::uint64_t kMaxManifestSize =
    kManifestMinSize +
    std::uint64_t{kManifestSstEntrySize} * kMaxManifestSsts;

struct ManifestSst {
    std::uint64_t file_id = 0;
    std::uint64_t file_size = 0;
    std::uint64_t min_sequence = 0;
    std::uint64_t max_sequence = 0;
};

struct Manifest {
    std::uint64_t generation = 0;
    std::uint64_t next_file_id = 0;
    std::uint64_t checkpoint = 0;
    std::uint64_t active_wal_id = 0;
    std::vector<ManifestSst> ssts;  // oldest first
};

// Checks every semantic rule of section 4 (IDs, intervals, checkpoint,
// limits). Returns Corruption naming the first violation.
Status ValidateManifest(const Manifest& manifest);

// Appends the encoding of a manifest that passes ValidateManifest.
void EncodeManifest(const Manifest& manifest, std::string* dst);

// Decodes and fully validates the complete contents of a MANIFEST file.
// On failure out is unchanged.
Status DecodeManifest(std::string_view data, Manifest& out);

// Reads path with a size bound checked before allocation, then decodes
// it. Messages name the path. Corruption, UnsupportedFormat, or IOError.
Status ReadManifestFile(const std::filesystem::path& path,
                        PosixSyscalls& syscalls,
                        Manifest& out);

}  // namespace kv
