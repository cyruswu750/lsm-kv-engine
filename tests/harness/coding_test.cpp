#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

#include "core/coding.h"

namespace kv {
namespace {

// Builds a byte string with an explicit length so zero bytes are kept.
std::string Bytes(const char* data, std::size_t n) {
    return std::string(data, n);
}

std::string Encode32(std::uint32_t value) {
    char buf[4];
    EncodeFixed32(buf, value);
    return std::string(buf, sizeof(buf));
}

std::string Encode64(std::uint64_t value) {
    char buf[8];
    EncodeFixed64(buf, value);
    return std::string(buf, sizeof(buf));
}

// ---- Encoding produces exact little-endian bytes ----------------------------

TEST(Coding, EncodeFixed32ExactBytes) {
    EXPECT_EQ(Encode32(0), Bytes("\x00\x00\x00\x00", 4));
    EXPECT_EQ(Encode32(1), Bytes("\x01\x00\x00\x00", 4));
    EXPECT_EQ(Encode32(0x12345678), Bytes("\x78\x56\x34\x12", 4));
    EXPECT_EQ(Encode32(0x89abcdef), Bytes("\xef\xcd\xab\x89", 4));
    EXPECT_EQ(Encode32(0x80000000), Bytes("\x00\x00\x00\x80", 4));
    EXPECT_EQ(Encode32(std::numeric_limits<std::uint32_t>::max()),
              Bytes("\xff\xff\xff\xff", 4));
}

TEST(Coding, EncodeFixed64ExactBytes) {
    EXPECT_EQ(Encode64(0), Bytes("\x00\x00\x00\x00\x00\x00\x00\x00", 8));
    EXPECT_EQ(Encode64(1), Bytes("\x01\x00\x00\x00\x00\x00\x00\x00", 8));
    EXPECT_EQ(Encode64(0x0102030405060708ull),
              Bytes("\x08\x07\x06\x05\x04\x03\x02\x01", 8));
    EXPECT_EQ(Encode64(0xfedcba9876543210ull),
              Bytes("\x10\x32\x54\x76\x98\xba\xdc\xfe", 8));
    EXPECT_EQ(Encode64(0x8000000000000000ull),
              Bytes("\x00\x00\x00\x00\x00\x00\x00\x80", 8));
    // Upper half must not be lost to a 32-bit shift.
    EXPECT_EQ(Encode64(0x0000000100000000ull),
              Bytes("\x00\x00\x00\x00\x01\x00\x00\x00", 8));
    EXPECT_EQ(Encode64(std::numeric_limits<std::uint64_t>::max()),
              Bytes("\xff\xff\xff\xff\xff\xff\xff\xff", 8));
}

// ---- Decoding independently specified bytes ---------------------------------

TEST(Coding, DecodeFixed32FromSpecifiedBytes) {
    EXPECT_EQ(DecodeFixed32("\x00\x00\x00\x00"), 0u);
    EXPECT_EQ(DecodeFixed32("\x78\x56\x34\x12"), 0x12345678u);
    EXPECT_EQ(DecodeFixed32("\xff\xff\xff\xff"), 0xffffffffu);
    // High-bit bytes in every position must not sign-extend into others.
    EXPECT_EQ(DecodeFixed32("\x80\x00\x00\x00"), 0x00000080u);
    EXPECT_EQ(DecodeFixed32("\xff\x00\x80\x7f"), 0x7f8000ffu);
    EXPECT_EQ(DecodeFixed32("\x00\x00\x00\x80"), 0x80000000u);
}

TEST(Coding, DecodeFixed64FromSpecifiedBytes) {
    EXPECT_EQ(DecodeFixed64("\x00\x00\x00\x00\x00\x00\x00\x00"), 0u);
    EXPECT_EQ(DecodeFixed64("\x08\x07\x06\x05\x04\x03\x02\x01"),
              0x0102030405060708ull);
    EXPECT_EQ(DecodeFixed64("\xff\xff\xff\xff\xff\xff\xff\xff"),
              0xffffffffffffffffull);
    EXPECT_EQ(DecodeFixed64("\x80\x00\x00\x00\x00\x00\x00\x00"),
              0x0000000000000080ull);
    EXPECT_EQ(DecodeFixed64("\x01\x00\x00\x00\x00\x00\x00\x80"),
              0x8000000000000001ull);
    EXPECT_EQ(DecodeFixed64("\x00\x00\x00\x00\xff\x00\x00\x00"),
              0x000000ff00000000ull);
}

// ---- Unaligned positions ----------------------------------------------------

TEST(Coding, EncodeAtUnalignedOffsetsTouchesOnlyTargetBytes) {
    for (std::size_t offset = 1; offset < 8; ++offset) {
        char buf[20];
        std::memset(buf, 0x5a, sizeof(buf));
        EncodeFixed32(buf + offset, 0x89abcdef);
        EncodeFixed64(buf + offset + 4, 0xfedcba9876543210ull);

        const std::string all(buf, sizeof(buf));
        EXPECT_EQ(all.substr(0, offset), std::string(offset, '\x5a'));
        EXPECT_EQ(all.substr(offset, 4), Bytes("\xef\xcd\xab\x89", 4));
        EXPECT_EQ(all.substr(offset + 4, 8),
                  Bytes("\x10\x32\x54\x76\x98\xba\xdc\xfe", 8));
        EXPECT_EQ(all.substr(offset + 12),
                  std::string(sizeof(buf) - offset - 12, '\x5a'));
    }
}

TEST(Coding, DecodeFromUnalignedOffsets) {
    // Separator bytes place the values at unaligned offsets 1, 6 and 14.
    const std::string data = Bytes(
        "\x01"
        "\x78\x56\x34\x12"
        "\x01"
        "\x08\x07\x06\x05\x04\x03\x02\x01"
        "\xff\x00\x80\x7f",
        18);
    EXPECT_EQ(DecodeFixed32(data.data() + 1), 0x12345678u);
    EXPECT_EQ(DecodeFixed64(data.data() + 6), 0x0102030405060708ull);
    EXPECT_EQ(DecodeFixed32(data.data() + 14), 0x7f8000ffu);
    // Overlapping window across field boundaries.
    EXPECT_EQ(DecodeFixed32(data.data() + 3), 0x08011234u);
}

// ---- Append helpers ---------------------------------------------------------

TEST(Coding, PutFixed32AppendsWithoutOverwritingPrefix) {
    std::string dst = Bytes("pre\0", 4);
    PutFixed32(&dst, 0x89abcdef);
    EXPECT_EQ(dst, Bytes("pre\0\xef\xcd\xab\x89", 8));

    PutFixed32(&dst, 0);
    EXPECT_EQ(dst, Bytes("pre\0\xef\xcd\xab\x89\x00\x00\x00\x00", 12));
}

TEST(Coding, PutFixed64AppendsWithoutOverwritingPrefix) {
    std::string dst = "ab";
    PutFixed64(&dst, 0xfedcba9876543210ull);
    EXPECT_EQ(dst, Bytes("ab\x10\x32\x54\x76\x98\xba\xdc\xfe", 10));

    PutFixed64(&dst, 1);
    EXPECT_EQ(dst, Bytes("ab\x10\x32\x54\x76\x98\xba\xdc\xfe"
                         "\x01\x00\x00\x00\x00\x00\x00\x00", 18));
}

TEST(Coding, PutFixedMixedSequenceLayout) {
    std::string dst;
    PutFixed32(&dst, 0x04030201);
    PutFixed64(&dst, 0x0c0b0a0908070605ull);
    PutFixed32(&dst, 0x100f0e0d);
    ASSERT_EQ(dst.size(), 16u);
    for (std::size_t i = 0; i < dst.size(); ++i) {
        const auto byte = static_cast<unsigned char>(dst[i]);
        EXPECT_EQ(static_cast<std::size_t>(byte), i + 1) << "byte " << i;
    }
}

// Round trips supplement, but do not replace, the exact-byte tests above.
TEST(Coding, RoundTripBoundaryValues) {
    for (std::uint32_t v : {0u, 1u, 0x7fu, 0x80u, 0xffu, 0x100u,
                            0x7fffffffu, 0x80000000u, 0xffffffffu}) {
        EXPECT_EQ(DecodeFixed32(Encode32(v).data()), v);
    }
    for (std::uint64_t v : {0ull, 1ull, 0xffffffffull, 0x100000000ull,
                            0x7fffffffffffffffull, 0x8000000000000000ull,
                            0xffffffffffffffffull}) {
        EXPECT_EQ(DecodeFixed64(Encode64(v).data()), v);
    }
}

}  // namespace
}  // namespace kv
