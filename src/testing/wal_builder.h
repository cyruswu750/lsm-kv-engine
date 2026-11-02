#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace kv::test {

// Builds WAL v1 bytes (design.md section 6) without using the production
// codec, CRC, or little-endian helpers, so DB-level tests can craft valid
// and malformed logs independently of the code under test.

// Bit-at-a-time CRC-32C (reflected polynomial 0x82F63B78).
inline std::uint32_t RefCrc32c(std::string_view data) {
    std::uint32_t crc = 0xffffffffu;
    for (char c : data) {
        crc ^= static_cast<unsigned char>(c);
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1) ? (crc >> 1) ^ 0x82F63B78u : crc >> 1;
        }
    }
    return crc ^ 0xffffffffu;
}

inline void AppendLE(std::string& out, std::uint64_t value, int bytes) {
    for (int i = 0; i < bytes; ++i) {
        out.push_back(static_cast<char>((value >> (8 * i)) & 0xff));
    }
}

inline std::string WalFileHeaderBytes(std::uint32_t version = 1,
                                      std::uint32_t reserved = 0) {
    std::string out = "KVWAL001";
    AppendLE(out, version, 4);
    AppendLE(out, reserved, 4);
    return out;
}

inline constexpr std::uint8_t kRawPut = 1;
inline constexpr std::uint8_t kRawDelete = 2;

struct RawRecordHeader {
    std::uint64_t sequence = 1;
    std::uint32_t key_len = 0;
    std::uint32_t value_len = 0;
    std::uint8_t type = kRawPut;
    std::uint8_t reserved[3] = {0, 0, 0};
    std::uint32_t payload_crc = 0;
};

// 28-byte record header with a correctly computed header CRC, whatever
// the (possibly invalid) field values.
inline std::string SealedRecordHeader(const RawRecordHeader& h) {
    std::string covered;
    AppendLE(covered, h.payload_crc, 4);
    AppendLE(covered, h.sequence, 8);
    AppendLE(covered, h.key_len, 4);
    AppendLE(covered, h.value_len, 4);
    covered.push_back(static_cast<char>(h.type));
    for (std::uint8_t b : h.reserved) {
        covered.push_back(static_cast<char>(b));
    }
    std::string out;
    AppendLE(out, RefCrc32c(covered), 4);
    return out + covered;
}

// A complete, valid record.
inline std::string RawRecord(std::uint64_t sequence,
                             std::uint8_t type,
                             std::string_view key,
                             std::string_view value) {
    RawRecordHeader h;
    h.sequence = sequence;
    h.type = type;
    h.key_len = static_cast<std::uint32_t>(key.size());
    h.value_len = static_cast<std::uint32_t>(value.size());
    std::string payload(key);
    payload.append(value);
    h.payload_crc = RefCrc32c(payload);
    return SealedRecordHeader(h) + payload;
}

}  // namespace kv::test
