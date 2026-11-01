#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "status.h"
#include "types.h"

namespace kv {

// Pure byte codec for WAL v1 (design.md section 6). No I/O happens here.

inline constexpr std::string_view kWalMagic{"KVWAL001", 8};
inline constexpr std::uint32_t kWalVersion = 1;
inline constexpr std::size_t kWalFileHeaderSize = 16;
inline constexpr std::size_t kWalRecordHeaderSize = 28;

// Appends the 16-byte v1 file header to *dst; existing contents are kept.
void AppendWalFileHeader(std::string* dst);

// Validates the first 16 bytes of data. Bytes after the header (records)
// are ignored. Checks, in order: length, magic, version, reserved.
// Returns Corruption, UnsupportedFormat, or Ok.
Status ValidateWalFileHeader(std::string_view data);

// Appends one encoded record to *dst. On success *dst grows by
// 28 + key.size() + value.size() bytes. On failure (InvalidArgument for
// zero sequence, invalid type, oversized key/value, or Delete with a
// nonempty value) *dst is left unchanged. key and value must not point
// into *dst, because appending may reallocate it.
Status AppendWalRecord(std::string* dst,
                       std::uint64_t sequence,
                       ValueType type,
                       std::string_view key,
                       std::string_view value);

// A decoded record. key and value borrow the input passed to
// DecodeWalRecord and are valid only while that buffer is alive and
// unmodified; callers must copy anything they retain.
struct WalRecord {
    std::uint64_t sequence = 0;
    ValueType type = ValueType::kPut;
    std::string_view key;
    std::string_view value;
};

enum class WalDecodeKind {
    kOk,
    kIncomplete,
    kCorruption
};

// Outcome of decoding one record. record() and next_offset() may be
// called only when kind() == kOk; reason() is nonempty only for
// kCorruption and names the record offset.
class WalDecodeResult {
public:
    static WalDecodeResult Ok(const WalRecord& record,
                              std::size_t next_offset);
    static WalDecodeResult Incomplete();
    static WalDecodeResult Corruption(std::string reason);

    WalDecodeKind kind() const {
        return kind_;
    }

    bool ok() const {
        return kind_ == WalDecodeKind::kOk;
    }

    const WalRecord& record() const;
    std::size_t next_offset() const;

    const std::string& reason() const {
        return reason_;
    }

private:
    explicit WalDecodeResult(WalDecodeKind kind) : kind_(kind) {}

    WalDecodeKind kind_;
    WalRecord record_;
    std::size_t next_offset_ = 0;
    std::string reason_;
};

// Decodes the record starting at input[offset]. previous_sequence is the
// sequence of the preceding record in this WAL, or 0 before the first.
//
// Returns kIncomplete only when fewer than 28 bytes remain, or when a
// header that passed every check declares more payload than remains.
// A complete header that fails any check is kCorruption even if its
// payload is missing. An offset greater than input.size() is kCorruption
// (a caller error that must not be mistaken for a truncated tail).
WalDecodeResult DecodeWalRecord(std::string_view input,
                                std::size_t offset,
                                std::uint64_t previous_sequence);

}  // namespace kv
