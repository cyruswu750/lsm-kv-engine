#include "wal_codec.h"

#include <cassert>
#include <cstdio>
#include <utility>

#include "coding.h"
#include "crc32c.h"

namespace kv {

namespace {

// Record header field offsets.
constexpr std::size_t kHeaderCrcOffset = 0;
constexpr std::size_t kPayloadCrcOffset = 4;
constexpr std::size_t kSequenceOffset = 8;
constexpr std::size_t kKeyLengthOffset = 16;
constexpr std::size_t kValueLengthOffset = 20;
constexpr std::size_t kTypeOffset = 24;
constexpr std::size_t kReservedOffset = 25;
constexpr std::size_t kReservedSize = 3;

// The header CRC covers everything after itself, including the payload CRC.
constexpr std::size_t kHeaderCrcCoveredOffset = 4;

std::string Hex32(std::uint32_t value) {
    char buf[11];
    std::snprintf(buf, sizeof(buf), "0x%08x", static_cast<unsigned>(value));
    return buf;
}

bool IsValidType(std::uint8_t type) {
    return type == static_cast<std::uint8_t>(ValueType::kPut) ||
           type == static_cast<std::uint8_t>(ValueType::kDelete);
}

std::uint32_t PayloadCrc(std::string_view key, std::string_view value) {
    return Crc32cExtend(Crc32c(key), value);
}

WalDecodeResult RecordCorruption(std::size_t offset, const std::string& what) {
    return WalDecodeResult::Corruption(
        "WAL record at offset " + std::to_string(offset) + ": " + what);
}

}  // namespace

void AppendWalFileHeader(std::string* dst) {
    dst->append(kWalMagic);
    PutFixed32(dst, kWalVersion);
    PutFixed32(dst, 0);
}

Status ValidateWalFileHeader(std::string_view data) {
    if (data.size() < kWalFileHeaderSize) {
        return Status::Corruption(
            "WAL file header truncated: " + std::to_string(data.size()) +
            " bytes, need " + std::to_string(kWalFileHeaderSize));
    }

    if (data.substr(0, kWalMagic.size()) != kWalMagic) {
        return Status::Corruption("WAL file header has bad magic");
    }

    const std::uint32_t version = DecodeFixed32(data.data() + 8);
    if (version != kWalVersion) {
        // The file header has no checksum, so a corrupted version field is
        // indistinguishable from a genuinely newer format.
        return Status::UnsupportedFormat(
            "unsupported WAL version " + std::to_string(version));
    }

    const std::uint32_t reserved = DecodeFixed32(data.data() + 12);
    if (reserved != 0) {
        return Status::Corruption(
            "WAL file header reserved field is nonzero: " + Hex32(reserved));
    }

    return Status::Ok();
}

Status AppendWalRecord(std::string* dst,
                       std::uint64_t sequence,
                       ValueType type,
                       std::string_view key,
                       std::string_view value) {
    if (sequence == 0) {
        return Status::InvalidArgument("WAL record sequence must be nonzero");
    }

    const auto raw_type = static_cast<std::uint8_t>(type);
    if (!IsValidType(raw_type)) {
        return Status::InvalidArgument(
            "invalid WAL record type " + std::to_string(raw_type));
    }

    Status s = ValidateKeyValue(key, value);
    if (!s.ok()) {
        return s;
    }

    if (type == ValueType::kDelete && !value.empty()) {
        return Status::InvalidArgument("Delete record must have an empty value");
    }

    char header[kWalRecordHeaderSize] = {};
    EncodeFixed32(header + kPayloadCrcOffset, PayloadCrc(key, value));
    EncodeFixed64(header + kSequenceOffset, sequence);
    EncodeFixed32(header + kKeyLengthOffset,
                  static_cast<std::uint32_t>(key.size()));
    EncodeFixed32(header + kValueLengthOffset,
                  static_cast<std::uint32_t>(value.size()));
    header[kTypeOffset] = static_cast<char>(raw_type);
    // Reserved bytes stay zero from the initializer.
    EncodeFixed32(header + kHeaderCrcOffset,
                  Crc32c(std::string_view(
                      header + kHeaderCrcCoveredOffset,
                      kWalRecordHeaderSize - kHeaderCrcCoveredOffset)));

    // Reserving first means the appends below cannot reallocate, so an
    // allocation failure throws before *dst is modified.
    dst->reserve(dst->size() + kWalRecordHeaderSize + key.size() + value.size());
    dst->append(header, kWalRecordHeaderSize);
    dst->append(key);
    dst->append(value);
    return Status::Ok();
}

WalDecodeResult WalDecodeResult::Ok(const WalRecord& record,
                                    std::size_t next_offset) {
    WalDecodeResult result(WalDecodeKind::kOk);
    result.record_ = record;
    result.next_offset_ = next_offset;
    return result;
}

WalDecodeResult WalDecodeResult::Incomplete() {
    return WalDecodeResult(WalDecodeKind::kIncomplete);
}

WalDecodeResult WalDecodeResult::Corruption(std::string reason) {
    WalDecodeResult result(WalDecodeKind::kCorruption);
    result.reason_ = std::move(reason);
    return result;
}

const WalRecord& WalDecodeResult::record() const {
    assert(ok());
    return record_;
}

std::size_t WalDecodeResult::next_offset() const {
    assert(ok());
    return next_offset_;
}

WalDecodeResult DecodeWalRecord(std::string_view input,
                                std::size_t offset,
                                std::uint64_t previous_sequence) {
    // Checked before any subtraction or pointer arithmetic.
    if (offset > input.size()) {
        return RecordCorruption(
            offset, "offset beyond input size " + std::to_string(input.size()));
    }

    const std::size_t remaining = input.size() - offset;
    if (remaining < kWalRecordHeaderSize) {
        return WalDecodeResult::Incomplete();
    }

    const char* header = input.data() + offset;

    const std::uint32_t stored_header_crc =
        DecodeFixed32(header + kHeaderCrcOffset);
    const std::uint32_t actual_header_crc = Crc32c(std::string_view(
        header + kHeaderCrcCoveredOffset,
        kWalRecordHeaderSize - kHeaderCrcCoveredOffset));
    if (stored_header_crc != actual_header_crc) {
        return RecordCorruption(
            offset, "header checksum mismatch (stored " +
                        Hex32(stored_header_crc) + ", computed " +
                        Hex32(actual_header_crc) + ")");
    }

    // Fields below are trusted only after the header CRC matched.
    for (std::size_t i = 0; i < kReservedSize; ++i) {
        if (header[kReservedOffset + i] != 0) {
            return RecordCorruption(offset, "reserved header bytes are nonzero");
        }
    }

    const auto raw_type =
        static_cast<std::uint8_t>(header[kTypeOffset]);
    if (!IsValidType(raw_type)) {
        return RecordCorruption(
            offset, "invalid record type " + std::to_string(raw_type));
    }
    const auto type = static_cast<ValueType>(raw_type);

    const std::uint32_t key_len = DecodeFixed32(header + kKeyLengthOffset);
    const std::uint32_t value_len = DecodeFixed32(header + kValueLengthOffset);

    if (type == ValueType::kDelete && value_len != 0) {
        return RecordCorruption(
            offset, "Delete record has value length " +
                        std::to_string(value_len));
    }

    if (key_len > kMaxKeyLength) {
        return RecordCorruption(
            offset, "key length " + std::to_string(key_len) +
                        " exceeds limit " + std::to_string(kMaxKeyLength));
    }

    if (value_len > kMaxValueLength) {
        return RecordCorruption(
            offset, "value length " + std::to_string(value_len) +
                        " exceeds limit " + std::to_string(kMaxValueLength));
    }

    const std::uint64_t sequence = DecodeFixed64(header + kSequenceOffset);
    if (sequence == 0) {
        return RecordCorruption(offset, "sequence is zero");
    }

    if (sequence <= previous_sequence) {
        return RecordCorruption(
            offset, "sequence " + std::to_string(sequence) +
                        " does not exceed previous sequence " +
                        std::to_string(previous_sequence));
    }

    // Both lengths are bounded above, so this 64-bit sum cannot overflow.
    const std::uint64_t total = std::uint64_t{kWalRecordHeaderSize} +
                                key_len + value_len;
    if (total > remaining) {
        return WalDecodeResult::Incomplete();
    }

    const std::string_view key(header + kWalRecordHeaderSize, key_len);
    const std::string_view value(key.data() + key_len, value_len);

    const std::uint32_t stored_payload_crc =
        DecodeFixed32(header + kPayloadCrcOffset);
    const std::uint32_t actual_payload_crc = PayloadCrc(key, value);
    if (stored_payload_crc != actual_payload_crc) {
        return RecordCorruption(
            offset, "payload checksum mismatch (stored " +
                        Hex32(stored_payload_crc) + ", computed " +
                        Hex32(actual_payload_crc) + ")");
    }

    WalRecord record;
    record.sequence = sequence;
    record.type = type;
    record.key = key;
    record.value = value;
    return WalDecodeResult::Ok(record,
                               offset + static_cast<std::size_t>(total));
}

}  // namespace kv
