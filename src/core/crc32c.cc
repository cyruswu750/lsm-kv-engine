#include "crc32c.h"

#include <array>

namespace kv {

namespace {

constexpr std::uint32_t kPolynomial = 0x82F63B78;
constexpr std::uint32_t kXor = 0xffffffff;

// table[i] is the register after shifting byte i through 8 reflected steps.
constexpr std::array<std::uint32_t, 256> MakeTable() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t crc = i;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1) ? (crc >> 1) ^ kPolynomial : crc >> 1;
        }
        table[i] = crc;
    }
    return table;
}

constexpr std::array<std::uint32_t, 256> kTable = MakeTable();

static_assert(kTable[0] == 0);
static_assert(kTable[1] == 0xf26b8303);
static_assert(kTable[128] == kPolynomial);

}  // namespace

std::uint32_t Crc32cExtend(std::uint32_t crc, std::string_view data) {
    // Undo the final XOR to recover the internal register. For crc == 0 this
    // yields the standard initial state 0xffffffff.
    std::uint32_t state = crc ^ kXor;
    for (char c : data) {
        state = kTable[(state ^ static_cast<unsigned char>(c)) & 0xff] ^
                (state >> 8);
    }
    return state ^ kXor;
}

std::uint32_t Crc32c(std::string_view data) {
    return Crc32cExtend(0, data);
}

}  // namespace kv
