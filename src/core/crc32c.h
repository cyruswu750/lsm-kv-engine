#pragma once

#include <cstdint>
#include <string_view>

namespace kv {

// Standard CRC-32C (Castagnoli): reflected polynomial 0x82F63B78, initial
// state 0xffffffff, final XOR 0xffffffff, no LevelDB-style masking.
// Crc32c("") == 0 and Crc32c("123456789") == 0xe3069283.

// Returns the finalized checksum of data.
std::uint32_t Crc32c(std::string_view data);

// Given crc == Crc32c(a), returns Crc32c(a followed by data). crc is a
// finalized value, not the internal register; 0 is the checksum of empty input.
std::uint32_t Crc32cExtend(std::uint32_t crc, std::string_view data);

}  // namespace kv
