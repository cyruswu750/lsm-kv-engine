#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "core/coding.h"
#include "core/wal_codec.h"

namespace kv {
namespace {

// Golden record bytes were produced by an independent bit-at-a-time
// CRC-32C Python script (reflected poly 0x82F63B78, verified against the
// "123456789" => 0xe3069283 check vector) and struct.pack, not by this
// library. Malformed-record tests re-seal mutated headers with RefCrc32c
// below, which is likewise independent of src/core/crc32c.cc.
// Note: the header CRC covers bytes [4, 28), which includes the payload
// CRC field, so the payload CRC must be computed first.

std::string Bytes(const char* data, std::size_t n) {
    return std::string(data, n);
}

// Put(seq=1, "a", "hi"): header_crc=0x78ca613d, payload_crc=0xe7f8e4d9.
const std::string kPutGolden = Bytes(
    "\x3d\x61\xca\x78"                  // header crc
    "\xd9\xe4\xf8\xe7"                  // payload crc
    "\x01\x00\x00\x00\x00\x00\x00\x00"  // sequence 1
    "\x01\x00\x00\x00"                  // key_len 1
    "\x02\x00\x00\x00"                  // value_len 2
    "\x01"                              // type Put
    "\x00\x00\x00"                      // reserved
    "a"
    "hi",
    31);

// Delete(seq=2, "a"): header_crc=0xabb57998, payload_crc=0xc1d04330.
const std::string kDeleteGolden = Bytes(
    "\x98\x79\xb5\xab"                  // header crc
    "\x30\x43\xd0\xc1"                  // payload crc
    "\x02\x00\x00\x00\x00\x00\x00\x00"  // sequence 2
    "\x01\x00\x00\x00"                  // key_len 1
    "\x00\x00\x00\x00"                  // value_len 0
    "\x02"                              // type Delete
    "\x00\x00\x00"                      // reserved
    "a",
    29);

const std::string kFileHeaderGolden = Bytes(
    "KVWAL001"
    "\x01\x00\x00\x00"
    "\x00\x00\x00\x00",
    16);

const std::string kBinaryKey = Bytes("\x00k\x80\xff\x00", 5);
const std::string kBinaryValue = Bytes("\xff\x00\x7f\x80v\x00", 6);

// Bit-at-a-time reference CRC-32C, used only to re-seal test headers.
std::uint32_t RefCrc32c(std::string_view data) {
    std::uint32_t crc = 0xffffffffu;
    for (char c : data) {
        crc ^= static_cast<unsigned char>(c);
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1) ? (crc >> 1) ^ 0x82F63B78u : crc >> 1;
        }
    }
    return crc ^ 0xffffffffu;
}

// Raw header fields, encoded without using the production encoder so
// tests can express values the encoder refuses to produce.
struct RawHeader {
    std::uint64_t sequence = 1;
    std::uint32_t key_len = 0;
    std::uint32_t value_len = 0;
    std::uint8_t type = 1;
    std::uint8_t reserved[3] = {0, 0, 0};
    std::uint32_t payload_crc = 0;
};

// Returns the 28-byte header with a correct header CRC.
std::string SealedHeader(const RawHeader& h) {
    char buf[28] = {};
    EncodeFixed32(buf + 4, h.payload_crc);
    EncodeFixed64(buf + 8, h.sequence);
    EncodeFixed32(buf + 16, h.key_len);
    EncodeFixed32(buf + 20, h.value_len);
    buf[24] = static_cast<char>(h.type);
    for (int i = 0; i < 3; ++i) {
        buf[25 + i] = static_cast<char>(h.reserved[i]);
    }
    EncodeFixed32(buf, RefCrc32c(std::string_view(buf + 4, 24)));
    return std::string(buf, sizeof(buf));
}

// Builds a complete, valid record independently of the production encoder.
std::string RawRecord(std::uint64_t sequence,
                      std::uint8_t type,
                      const std::string& key,
                      const std::string& value) {
    RawHeader h;
    h.sequence = sequence;
    h.type = type;
    h.key_len = static_cast<std::uint32_t>(key.size());
    h.value_len = static_cast<std::uint32_t>(value.size());
    h.payload_crc = RefCrc32c(key + value);
    return SealedHeader(h) + key + value;
}

std::string Encode(std::uint64_t sequence,
                   ValueType type,
                   std::string_view key,
                   std::string_view value) {
    std::string out;
    Status s = AppendWalRecord(&out, sequence, type, key, value);
    EXPECT_TRUE(s.ok()) << s.ToString();
    return out;
}

bool Contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

void ExpectCorruption(const WalDecodeResult& r,
                      std::size_t offset,
                      std::string_view what) {
    ASSERT_EQ(r.kind(), WalDecodeKind::kCorruption);
    EXPECT_TRUE(Contains(r.reason(),
                         "offset " + std::to_string(offset) + ":"))
        << r.reason();
    EXPECT_TRUE(Contains(r.reason(), what)) << r.reason();
}

// ---- File header -----------------------------------------------------------

TEST(WalFileHeaderTest, EncodesGoldenBytes) {
    std::string out;
    AppendWalFileHeader(&out);
    EXPECT_EQ(out, kFileHeaderGolden);
}

TEST(WalFileHeaderTest, AppendsAfterExistingContents) {
    std::string out = "prefix";
    AppendWalFileHeader(&out);
    EXPECT_EQ(out, "prefix" + kFileHeaderGolden);
}

TEST(WalFileHeaderTest, AcceptsGoldenHeader) {
    EXPECT_TRUE(ValidateWalFileHeader(kFileHeaderGolden).ok());
}

TEST(WalFileHeaderTest, RejectsEveryTruncatedLength) {
    for (std::size_t n = 0; n < 16; ++n) {
        Status s = ValidateWalFileHeader(kFileHeaderGolden.substr(0, n));
        EXPECT_EQ(s.code, Code::Corruption) << "length " << n;
        EXPECT_TRUE(Contains(s.message, "truncated")) << s.message;
    }
}

TEST(WalFileHeaderTest, RejectsWrongMagic) {
    for (std::size_t i = 0; i < 8; ++i) {
        std::string header = kFileHeaderGolden;
        header[i] ^= 0x01;
        Status s = ValidateWalFileHeader(header);
        EXPECT_EQ(s.code, Code::Corruption) << "byte " << i;
        EXPECT_TRUE(Contains(s.message, "magic")) << s.message;
    }
}

TEST(WalFileHeaderTest, RejectsUnsupportedVersion) {
    for (std::uint32_t version : {0u, 2u, 0xffffffffu}) {
        std::string header = kFileHeaderGolden;
        EncodeFixed32(header.data() + 8, version);
        EXPECT_EQ(ValidateWalFileHeader(header).code, Code::UnsupportedFormat)
            << "version " << version;
    }
}

TEST(WalFileHeaderTest, RejectsNonzeroReserved) {
    for (std::size_t i = 12; i < 16; ++i) {
        std::string header = kFileHeaderGolden;
        header[i] = '\x01';
        Status s = ValidateWalFileHeader(header);
        EXPECT_EQ(s.code, Code::Corruption) << "byte " << i;
        EXPECT_TRUE(Contains(s.message, "reserved")) << s.message;
    }
}

TEST(WalFileHeaderTest, ErrorPrecedence) {
    // Short input with bad magic: length is checked first.
    Status s = ValidateWalFileHeader("XXXXXXXXXX");
    EXPECT_EQ(s.code, Code::Corruption);
    EXPECT_TRUE(Contains(s.message, "truncated")) << s.message;

    // Bad magic, bad version, and nonzero reserved: magic wins.
    std::string header = kFileHeaderGolden;
    header[0] = 'X';
    EncodeFixed32(header.data() + 8, 2);
    EncodeFixed32(header.data() + 12, 7);
    s = ValidateWalFileHeader(header);
    EXPECT_EQ(s.code, Code::Corruption);
    EXPECT_TRUE(Contains(s.message, "magic")) << s.message;

    // Bad version and nonzero reserved: version wins.
    header = kFileHeaderGolden;
    EncodeFixed32(header.data() + 8, 2);
    EncodeFixed32(header.data() + 12, 7);
    EXPECT_EQ(ValidateWalFileHeader(header).code, Code::UnsupportedFormat);
}

TEST(WalFileHeaderTest, IgnoresBytesAfterHeader) {
    EXPECT_TRUE(ValidateWalFileHeader(kFileHeaderGolden + kPutGolden).ok());
    EXPECT_TRUE(ValidateWalFileHeader(kFileHeaderGolden + "garbage").ok());
}

// ---- Record golden vectors -------------------------------------------------

TEST(WalRecordGoldenTest, EncodesPut) {
    EXPECT_EQ(Encode(1, ValueType::kPut, "a", "hi"), kPutGolden);
}

TEST(WalRecordGoldenTest, EncodesDelete) {
    EXPECT_EQ(Encode(2, ValueType::kDelete, "a", ""), kDeleteGolden);
}

TEST(WalRecordGoldenTest, IndependentBuilderMatchesGolden) {
    // Cross-checks the test's own builder before it is trusted elsewhere.
    EXPECT_EQ(RawRecord(1, 1, "a", "hi"), kPutGolden);
    EXPECT_EQ(RawRecord(2, 2, "a", ""), kDeleteGolden);
}

TEST(WalRecordGoldenTest, DecodesPut) {
    WalDecodeResult r = DecodeWalRecord(kPutGolden, 0, 0);
    ASSERT_TRUE(r.ok()) << r.reason();
    EXPECT_EQ(r.record().sequence, 1u);
    EXPECT_EQ(r.record().type, ValueType::kPut);
    EXPECT_EQ(r.record().key, "a");
    EXPECT_EQ(r.record().value, "hi");
    EXPECT_EQ(r.next_offset(), 31u);
    EXPECT_TRUE(r.reason().empty());

    // Decoded slices borrow the input buffer.
    EXPECT_EQ(r.record().key.data(), kPutGolden.data() + 28);
    EXPECT_EQ(r.record().value.data(), kPutGolden.data() + 29);
}

TEST(WalRecordGoldenTest, DecodesDelete) {
    WalDecodeResult r = DecodeWalRecord(kDeleteGolden, 0, 1);
    ASSERT_TRUE(r.ok()) << r.reason();
    EXPECT_EQ(r.record().sequence, 2u);
    EXPECT_EQ(r.record().type, ValueType::kDelete);
    EXPECT_EQ(r.record().key, "a");
    EXPECT_TRUE(r.record().value.empty());
    EXPECT_EQ(r.next_offset(), 29u);
}

TEST(WalRecordEncodeTest, AppendsAfterExistingContents) {
    std::string out = kFileHeaderGolden;
    ASSERT_TRUE(AppendWalRecord(&out, 1, ValueType::kPut, "a", "hi").ok());
    ASSERT_TRUE(AppendWalRecord(&out, 2, ValueType::kDelete, "a", "").ok());
    EXPECT_EQ(out, kFileHeaderGolden + kPutGolden + kDeleteGolden);
}

// ---- Valid cases -----------------------------------------------------------

TEST(WalRecordValidTest, EmptyPutIsDistinctFromDelete) {
    const std::string put = Encode(1, ValueType::kPut, "", "");
    const std::string del = Encode(1, ValueType::kDelete, "", "");
    EXPECT_EQ(put, RawRecord(1, 1, "", ""));
    EXPECT_EQ(del, RawRecord(1, 2, "", ""));
    EXPECT_EQ(put.size(), 28u);
    EXPECT_NE(put, del);

    WalDecodeResult rp = DecodeWalRecord(put, 0, 0);
    WalDecodeResult rd = DecodeWalRecord(del, 0, 0);
    ASSERT_TRUE(rp.ok()) << rp.reason();
    ASSERT_TRUE(rd.ok()) << rd.reason();
    EXPECT_EQ(rp.record().type, ValueType::kPut);
    EXPECT_EQ(rd.record().type, ValueType::kDelete);
    EXPECT_TRUE(rp.record().key.empty());
    EXPECT_TRUE(rp.record().value.empty());
    EXPECT_EQ(rp.next_offset(), 28u);
    EXPECT_EQ(rd.next_offset(), 28u);
}

TEST(WalRecordValidTest, BinaryKeyAndValue) {
    const std::string encoded = Encode(9, ValueType::kPut, kBinaryKey,
                                       kBinaryValue);
    EXPECT_EQ(encoded, RawRecord(9, 1, kBinaryKey, kBinaryValue));

    WalDecodeResult r = DecodeWalRecord(encoded, 0, 0);
    ASSERT_TRUE(r.ok()) << r.reason();
    EXPECT_EQ(r.record().key, kBinaryKey);
    EXPECT_EQ(r.record().value, kBinaryValue);
}

TEST(WalRecordValidTest, StreamFromNonzeroOffsetWithGapsAndMaxSequence) {
    constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
    struct Expected {
        std::uint64_t sequence;
        std::uint8_t type;
        std::string key;
        std::string value;
    };
    const std::vector<Expected> records = {
        {3, 1, "k1", "v1"},
        {7, 2, "k1", ""},
        {8, 1, kBinaryKey, kBinaryValue},
        {kMax - 1, 1, "", "x"},
        {kMax, 2, "z", ""},
    };

    std::string stream = kFileHeaderGolden;
    for (const Expected& e : records) {
        stream += RawRecord(e.sequence, e.type, e.key, e.value);
    }

    std::size_t offset = kWalFileHeaderSize;
    std::uint64_t previous = 0;
    for (const Expected& e : records) {
        WalDecodeResult r = DecodeWalRecord(stream, offset, previous);
        ASSERT_TRUE(r.ok()) << r.reason();
        EXPECT_EQ(r.record().sequence, e.sequence);
        EXPECT_EQ(static_cast<std::uint8_t>(r.record().type), e.type);
        EXPECT_EQ(r.record().key, e.key);
        EXPECT_EQ(r.record().value, e.value);
        EXPECT_EQ(r.next_offset(), offset + 28 + e.key.size() + e.value.size());
        offset = r.next_offset();
        previous = r.record().sequence;
    }
    EXPECT_EQ(offset, stream.size());
}

TEST(WalRecordValidTest, EncodesMaxSequence) {
    constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
    EXPECT_EQ(Encode(kMax, ValueType::kPut, "k", "v"),
              RawRecord(kMax, 1, "k", "v"));
}

TEST(WalRecordValidTest, AcceptsExactLengthLimits) {
    const std::string key(kMaxKeyLength, 'k');
    const std::string value(kMaxValueLength, '\x80');
    std::string encoded;
    ASSERT_TRUE(AppendWalRecord(&encoded, 1, ValueType::kPut, key, value).ok());
    ASSERT_EQ(encoded.size(), 28 + kMaxKeyLength + kMaxValueLength);

    WalDecodeResult r = DecodeWalRecord(encoded, 0, 0);
    ASSERT_TRUE(r.ok()) << r.reason();
    EXPECT_EQ(r.record().key.size(), kMaxKeyLength);
    EXPECT_EQ(r.record().value.size(), kMaxValueLength);
    EXPECT_EQ(r.record().key, key);
    EXPECT_EQ(r.record().value, value);

    std::string del;
    ASSERT_TRUE(AppendWalRecord(&del, 2, ValueType::kDelete, key, "").ok());
    EXPECT_TRUE(DecodeWalRecord(del, 0, 1).ok());
}

// ---- Malformed records -----------------------------------------------------

TEST(WalRecordMalformedTest, BadHeaderCrc) {
    std::string rec = kPutGolden;
    rec[0] ^= 0x01;  // stored CRC itself
    ExpectCorruption(DecodeWalRecord(rec, 0, 0), 0, "header checksum");

    rec = kPutGolden;
    rec[8] ^= 0x02;  // sequence byte, header not re-sealed
    ExpectCorruption(DecodeWalRecord(rec, 0, 0), 0, "header checksum");

    rec = kPutGolden;
    rec[16] = '\x7f';  // key_len, header not re-sealed
    ExpectCorruption(DecodeWalRecord(rec, 0, 0), 0, "header checksum");
}

TEST(WalRecordMalformedTest, BadHeaderCrcReportedBeforeIncompletePayload) {
    // Only the 28 header bytes are present and the CRC is wrong.
    std::string header = kPutGolden.substr(0, 28);
    header[1] ^= 0x10;
    ExpectCorruption(DecodeWalRecord(header, 0, 0), 0, "header checksum");
}

TEST(WalRecordMalformedTest, BadPayloadCrc) {
    for (std::size_t i = 28; i < kPutGolden.size(); ++i) {
        std::string rec = kPutGolden;
        rec[i] ^= 0x01;
        ExpectCorruption(DecodeWalRecord(rec, 0, 0), 0, "payload checksum");
    }

    // Stored payload CRC changed and header re-sealed.
    RawHeader h;
    h.key_len = 1;
    h.value_len = 2;
    h.payload_crc = RefCrc32c("ahi") ^ 1;
    ExpectCorruption(DecodeWalRecord(SealedHeader(h) + "ahi", 0, 0), 0,
                     "payload checksum");
}

TEST(WalRecordMalformedTest, BadPayloadCrcInFinalRecord) {
    std::string stream = kFileHeaderGolden + kPutGolden + kDeleteGolden;
    stream.back() ^= 0x01;

    WalDecodeResult first = DecodeWalRecord(stream, 16, 0);
    ASSERT_TRUE(first.ok()) << first.reason();
    ASSERT_EQ(first.next_offset(), 47u);
    ExpectCorruption(DecodeWalRecord(stream, 47, 1), 47, "payload checksum");
}

TEST(WalRecordMalformedTest, NonzeroReserved) {
    for (int i = 0; i < 3; ++i) {
        RawHeader h;
        h.key_len = 1;
        h.value_len = 2;
        h.payload_crc = RefCrc32c("ahi");
        h.reserved[i] = 0x01;
        ExpectCorruption(DecodeWalRecord(SealedHeader(h) + "ahi", 0, 0), 0,
                         "reserved");
    }
}

TEST(WalRecordMalformedTest, InvalidType) {
    for (std::uint8_t type : {0, 3, 0x80, 0xff}) {
        RawHeader h;
        h.type = type;
        h.key_len = 1;
        h.payload_crc = RefCrc32c("a");
        ExpectCorruption(DecodeWalRecord(SealedHeader(h) + "a", 0, 0), 0,
                         "invalid record type");
    }
}

TEST(WalRecordMalformedTest, DeleteWithValueLength) {
    RawHeader h;
    h.type = 2;
    h.key_len = 1;
    h.value_len = 1;
    h.payload_crc = RefCrc32c("ab");
    ExpectCorruption(DecodeWalRecord(SealedHeader(h) + "ab", 0, 0), 0,
                     "Delete record has value length");
}

TEST(WalRecordMalformedTest, OversizedLengths) {
    struct Case {
        std::uint32_t key_len;
        std::uint32_t value_len;
        const char* what;
    };
    const Case cases[] = {
        {static_cast<std::uint32_t>(kMaxKeyLength + 1), 0, "key length"},
        {0xffffffffu, 0, "key length"},
        {0, static_cast<std::uint32_t>(kMaxValueLength + 1), "value length"},
        {0, 0xffffffffu, "value length"},
        {0xffffffffu, 0xffffffffu, "key length"},
    };
    for (const Case& c : cases) {
        RawHeader h;
        h.key_len = c.key_len;
        h.value_len = c.value_len;
        // Payload is deliberately absent: lengths must be rejected as
        // Corruption, never Incomplete, and nothing may be allocated.
        ExpectCorruption(DecodeWalRecord(SealedHeader(h), 0, 0), 0, c.what);
    }
}

TEST(WalRecordMalformedTest, InvalidSequence) {
    // Zero.
    ExpectCorruption(DecodeWalRecord(RawRecord(0, 1, "a", "hi"), 0, 0), 0,
                     "sequence is zero");
    // Duplicate.
    ExpectCorruption(DecodeWalRecord(RawRecord(5, 1, "a", "hi"), 0, 5), 0,
                     "does not exceed previous");
    // Decreasing.
    ExpectCorruption(DecodeWalRecord(RawRecord(5, 1, "a", "hi"), 0, 6), 0,
                     "does not exceed previous");
    // Nothing can follow UINT64_MAX.
    constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
    ExpectCorruption(DecodeWalRecord(RawRecord(kMax, 1, "a", ""), 0, kMax), 0,
                     "does not exceed previous");
}

TEST(WalRecordMalformedTest, DuplicateSequenceInStream) {
    std::string stream = kFileHeaderGolden + RawRecord(4, 1, "a", "1") +
                         RawRecord(4, 1, "b", "2");
    WalDecodeResult first = DecodeWalRecord(stream, 16, 0);
    ASSERT_TRUE(first.ok()) << first.reason();
    ExpectCorruption(DecodeWalRecord(stream, first.next_offset(), 4),
                     first.next_offset(), "does not exceed previous");
}

TEST(WalRecordMalformedTest, InvalidFieldsWithMissingPayloadAreCorruption) {
    // Each header declares a payload that is entirely absent.
    RawHeader zero_seq;
    zero_seq.sequence = 0;
    zero_seq.key_len = 1;
    zero_seq.value_len = 2;
    ExpectCorruption(DecodeWalRecord(SealedHeader(zero_seq), 0, 0), 0,
                     "sequence is zero");

    RawHeader dup_seq;
    dup_seq.sequence = 3;
    dup_seq.key_len = 1;
    dup_seq.value_len = 2;
    ExpectCorruption(DecodeWalRecord(SealedHeader(dup_seq) + "a", 0, 3), 0,
                     "does not exceed previous");

    RawHeader bad_type;
    bad_type.type = 9;
    bad_type.key_len = 10;
    ExpectCorruption(DecodeWalRecord(SealedHeader(bad_type), 0, 0), 0,
                     "invalid record type");

    RawHeader bad_reserved;
    bad_reserved.key_len = 10;
    bad_reserved.reserved[2] = 0xff;
    ExpectCorruption(DecodeWalRecord(SealedHeader(bad_reserved), 0, 0), 0,
                     "reserved");

    RawHeader delete_value;
    delete_value.type = 2;
    delete_value.key_len = 1;
    delete_value.value_len = 5;
    ExpectCorruption(DecodeWalRecord(SealedHeader(delete_value), 0, 0), 0,
                     "Delete record");

    RawHeader big_key;
    big_key.key_len = static_cast<std::uint32_t>(kMaxKeyLength + 1);
    ExpectCorruption(DecodeWalRecord(SealedHeader(big_key), 0, 0), 0,
                     "key length");
}

TEST(WalRecordMalformedTest, OffsetBeyondInput) {
    const std::size_t n = kPutGolden.size();
    WalDecodeResult r = DecodeWalRecord(kPutGolden, n + 1, 0);
    ExpectCorruption(r, n + 1, "offset beyond input");

    r = DecodeWalRecord(kPutGolden, std::numeric_limits<std::size_t>::max(), 0);
    EXPECT_EQ(r.kind(), WalDecodeKind::kCorruption);

    r = DecodeWalRecord(std::string_view(), 1, 0);
    EXPECT_EQ(r.kind(), WalDecodeKind::kCorruption);
}

TEST(WalRecordMalformedTest, ReportsStreamOffset) {
    std::string stream = kFileHeaderGolden + kPutGolden;
    stream[16] ^= 0x01;
    ExpectCorruption(DecodeWalRecord(stream, 16, 0), 16, "header checksum");
}

// ---- Truncation ------------------------------------------------------------

TEST(WalRecordTruncationTest, EveryPrefixIsIncomplete) {
    const std::vector<std::string> records = {
        kPutGolden,
        kDeleteGolden,
        RawRecord(1, 1, "", ""),
        RawRecord(1, 1, kBinaryKey, kBinaryValue),
    };
    for (const std::string& rec : records) {
        for (std::size_t n = 0; n < rec.size(); ++n) {
            WalDecodeResult r = DecodeWalRecord(rec.substr(0, n), 0, 0);
            EXPECT_EQ(r.kind(), WalDecodeKind::kIncomplete)
                << "prefix " << n << " of " << rec.size() << ": "
                << r.reason();
            EXPECT_TRUE(r.reason().empty());
        }
        WalDecodeResult full = DecodeWalRecord(rec, 0, 0);
        ASSERT_TRUE(full.ok()) << full.reason();
        EXPECT_EQ(full.next_offset(), rec.size());
    }
}

TEST(WalRecordTruncationTest, EveryPrefixAtNonzeroOffset) {
    const std::string stream = kFileHeaderGolden + kPutGolden + kDeleteGolden;
    for (std::size_t end = 47; end < stream.size(); ++end) {
        WalDecodeResult r = DecodeWalRecord(stream.substr(0, end), 47, 1);
        EXPECT_EQ(r.kind(), WalDecodeKind::kIncomplete) << "end " << end;
    }
    EXPECT_TRUE(DecodeWalRecord(stream, 47, 1).ok());
}

TEST(WalRecordTruncationTest, OffsetAtEndIsIncomplete) {
    EXPECT_EQ(DecodeWalRecord(kPutGolden, kPutGolden.size(), 0).kind(),
              WalDecodeKind::kIncomplete);
    EXPECT_EQ(DecodeWalRecord(std::string_view(), 0, 0).kind(),
              WalDecodeKind::kIncomplete);
}

// ---- Encoder rejection -----------------------------------------------------

void ExpectRejected(std::uint64_t sequence,
                    ValueType type,
                    std::string_view key,
                    std::string_view value) {
    std::string out = "prefix";
    Status s = AppendWalRecord(&out, sequence, type, key, value);
    EXPECT_EQ(s.code, Code::InvalidArgument) << s.ToString();
    EXPECT_EQ(out, "prefix");
}

TEST(WalRecordEncodeTest, RejectsZeroSequence) {
    ExpectRejected(0, ValueType::kPut, "a", "hi");
}

TEST(WalRecordEncodeTest, RejectsInvalidType) {
    ExpectRejected(1, static_cast<ValueType>(0), "a", "");
    ExpectRejected(1, static_cast<ValueType>(3), "a", "");
    ExpectRejected(1, static_cast<ValueType>(0xff), "a", "");
}

TEST(WalRecordEncodeTest, RejectsOversizedInputs) {
    const std::string big_key(kMaxKeyLength + 1, 'k');
    const std::string big_value(kMaxValueLength + 1, 'v');
    ExpectRejected(1, ValueType::kPut, big_key, "v");
    ExpectRejected(1, ValueType::kDelete, big_key, "");
    ExpectRejected(1, ValueType::kPut, "k", big_value);
}

TEST(WalRecordEncodeTest, RejectsDeleteWithValue) {
    ExpectRejected(1, ValueType::kDelete, "a", "x");
    ExpectRejected(1, ValueType::kDelete, "a", std::string_view("\0", 1));
}

}  // namespace
}  // namespace kv
