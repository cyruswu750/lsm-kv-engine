#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace kv {

// Database file names (docs/sst-format.md section 2).

inline constexpr std::string_view kLockFileName = "LOCK";
inline constexpr std::string_view kManifestFileName = "MANIFEST";
inline constexpr std::string_view kManifestTempFileName = "MANIFEST.tmp";

// M1 names, recognized only so that an M1 directory is rejected.
inline constexpr std::string_view kM1WalFileName = "000001.wal";
inline constexpr std::string_view kM1WalTempFileName = "000001.wal.tmp";

enum class FileKind {
    kWal,
    kWalTemp,
    kSst,
    kSstTemp
};

// "wal-000007.log", "sst-000007.sst". id must be nonzero.
std::string WalFileName(std::uint64_t id);
std::string SstFileName(std::uint64_t id);

// name + ".tmp".
std::string TempFileName(std::string_view name);

// Recognizes wal-<id>.log, sst-<id>.sst, and their .tmp forms. <id> must
// be the canonical form: at least 6 digits, no extra leading zero, and a
// value in [1, 2^64-1]. Any other name returns false and leaves the
// outputs unchanged.
bool ParseFileName(std::string_view name, FileKind& kind, std::uint64_t& id);

}  // namespace kv
