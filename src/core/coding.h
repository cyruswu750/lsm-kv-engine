#pragma once

#include <cstdint>
#include <string>

namespace kv {

// Fixed-width unsigned little-endian encoding, independent of host byte
// order and alignment. The raw-buffer helpers do no bounds checking:
// Fixed32 requires 4 and Fixed64 requires 8 accessible bytes at the given
// pointer (writable for Encode, readable for Decode). Parsers must check
// the remaining length before calling Decode.

inline void EncodeFixed32(char* dst, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        dst[i] = static_cast<char>(static_cast<unsigned char>(value >> (8 * i)));
    }
}

inline void EncodeFixed64(char* dst, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        dst[i] = static_cast<char>(static_cast<unsigned char>(value >> (8 * i)));
    }
}

// Each byte goes through unsigned char before widening so that bytes
// >= 0x80 are not sign-extended when char is signed.
inline std::uint32_t DecodeFixed32(const char* src) {
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(static_cast<unsigned char>(src[i]))
                 << (8 * i);
    }
    return value;
}

inline std::uint64_t DecodeFixed64(const char* src) {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(static_cast<unsigned char>(src[i]))
                 << (8 * i);
    }
    return value;
}

// Appends the encoding to *dst, leaving existing contents unchanged.
inline void PutFixed32(std::string* dst, std::uint32_t value) {
    char buf[4];
    EncodeFixed32(buf, value);
    dst->append(buf, sizeof(buf));
}

inline void PutFixed64(std::string* dst, std::uint64_t value) {
    char buf[8];
    EncodeFixed64(buf, value);
    dst->append(buf, sizeof(buf));
}

}  // namespace kv
