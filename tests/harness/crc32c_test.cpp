#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>

#include "core/crc32c.h"

namespace kv {
namespace {

// Expected values below come from published CRC-32C vectors (design.md
// section 6 and RFC 3720 appendix B.4) or, for "binary sample" and
// "all byte values", from an independent bit-at-a-time Python reference
// that reproduces every published vector. None are produced by this library.

std::string Bytes(const char* data, std::size_t n) {
    return std::string(data, n);
}

std::string AscendingBytes(int first, int count) {
    std::string s;
    for (int i = 0; i < count; ++i) {
        s.push_back(static_cast<char>(first + i));
    }
    return s;
}

std::string DescendingBytes(int first, int count) {
    std::string s;
    for (int i = 0; i < count; ++i) {
        s.push_back(static_cast<char>(first - i));
    }
    return s;
}

// Mixes zero bytes, 0x7f/0x80 boundaries, and 0xff.
const std::string kBinarySample =
    Bytes("\x00\x01\x7f\x80\xfe\xff\x00key\x00\x80value\xff", 18);

// ---- Known vectors -----------------------------------------------------------

TEST(Crc32c, EmptyInputIsZero) {
    EXPECT_EQ(Crc32c(""), 0u);
    EXPECT_EQ(Crc32c(std::string_view()), 0u);
}

TEST(Crc32c, StandardCheckVector) {
    EXPECT_EQ(Crc32c("123456789"), 0xe3069283u);
}

TEST(Crc32c, Rfc3720Vectors) {
    EXPECT_EQ(Crc32c(std::string(32, '\x00')), 0x8a9136aau);
    EXPECT_EQ(Crc32c(std::string(32, '\xff')), 0x62a8ab43u);
    EXPECT_EQ(Crc32c(AscendingBytes(0x00, 32)), 0x46dd794eu);
    EXPECT_EQ(Crc32c(DescendingBytes(0x1f, 32)), 0x113fdb5cu);
}

TEST(Crc32c, BinaryInputWithZeroAndHighBitBytes) {
    ASSERT_EQ(kBinarySample.size(), 18u);
    EXPECT_EQ(Crc32c(kBinarySample), 0xcbdf9e16u);
}

TEST(Crc32c, AllByteValues) {
    EXPECT_EQ(Crc32c(AscendingBytes(0x00, 256)), 0x9c44184bu);
}

TEST(Crc32c, EmbeddedZeroBytesAreNotTruncated) {
    // Inputs differing only after a zero byte must differ in checksum.
    const std::string a = Bytes("ab\0cd", 5);
    const std::string b = Bytes("ab\0ce", 5);
    EXPECT_NE(Crc32c(a), Crc32c(b));
    EXPECT_NE(Crc32c(a), Crc32c("ab"));
    // Leading and trailing zero bytes are significant.
    EXPECT_NE(Crc32c(Bytes("\0", 1)), Crc32c(""));
    EXPECT_NE(Crc32c(Bytes("\0\0", 2)), Crc32c(Bytes("\0", 1)));
}

// ---- Extension contract -------------------------------------------------------

TEST(Crc32c, ExtendFromZeroEqualsFullChecksum) {
    EXPECT_EQ(Crc32cExtend(0, ""), 0u);
    EXPECT_EQ(Crc32cExtend(0, "123456789"), 0xe3069283u);
    EXPECT_EQ(Crc32cExtend(0, kBinarySample), Crc32c(kBinarySample));
}

TEST(Crc32c, ExtendWithEmptyInputIsIdentity) {
    for (std::uint32_t crc : {0u, 1u, 0xe3069283u, 0x80000000u, 0xffffffffu}) {
        EXPECT_EQ(Crc32cExtend(crc, ""), crc);
    }
}

TEST(Crc32c, ExtendAtEverySplitPosition) {
    // Includes split 0 (empty first segment) and split size (empty last).
    for (std::size_t split = 0; split <= kBinarySample.size(); ++split) {
        const std::string_view all(kBinarySample);
        const std::uint32_t first = Crc32c(all.substr(0, split));
        EXPECT_EQ(Crc32cExtend(first, all.substr(split)), 0xcbdf9e16u)
            << "split " << split;
    }
}

TEST(Crc32c, ExtendCheckVectorAtEverySplitPosition) {
    const std::string_view all = "123456789";
    for (std::size_t split = 0; split <= all.size(); ++split) {
        EXPECT_EQ(Crc32cExtend(Crc32c(all.substr(0, split)), all.substr(split)),
                  0xe3069283u)
            << "split " << split;
    }
}

TEST(Crc32c, MultipleSuccessiveExtensions) {
    // Byte-at-a-time over all 256 byte values.
    const std::string all = AscendingBytes(0x00, 256);
    std::uint32_t crc = 0;
    for (char c : all) {
        crc = Crc32cExtend(crc, std::string_view(&c, 1));
    }
    EXPECT_EQ(crc, 0x9c44184bu);

    // Uneven chunks with empty pieces interleaved.
    crc = Crc32c("");
    crc = Crc32cExtend(crc, "12");
    crc = Crc32cExtend(crc, "");
    crc = Crc32cExtend(crc, "345");
    crc = Crc32cExtend(crc, "6");
    crc = Crc32cExtend(crc, "");
    crc = Crc32cExtend(crc, "789");
    EXPECT_EQ(crc, 0xe3069283u);
}

TEST(Crc32c, KeyThenValueMatchesConcatenation) {
    // The WAL payload checksum covers key bytes followed by value bytes.
    const std::string key = Bytes("k\0\x80", 3);
    const std::string value = Bytes("\xff\0v", 3);
    EXPECT_EQ(Crc32cExtend(Crc32c(key), value), Crc32c(key + value));
}

}  // namespace
}  // namespace kv
