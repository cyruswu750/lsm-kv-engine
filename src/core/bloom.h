#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "status.h"

namespace kv {

// The SST version 2 Bloom filter (docs/sst-format.md section 3.7).

inline constexpr std::uint32_t kFilterVersion = 1;
inline constexpr std::uint32_t kKvHash1 = 1;
inline constexpr std::size_t kFilterHeaderSize = 16;
inline constexpr std::size_t kFilterTrailerSize = 4;
inline constexpr std::size_t kMinFilterBitBytes = 8;
inline constexpr std::uint32_t kMaxSstFilterKeys = 1u << 22;
inline constexpr std::uint32_t kMaxFilterBitsPerKey = 64;
inline constexpr std::uint32_t kMaxFilterProbes = 30;
inline constexpr std::uint32_t kDefaultBloomBitsPerKey = 10;
inline constexpr std::uint64_t kMinSstFilterSize =
    kFilterHeaderSize + kMinFilterBitBytes + kFilterTrailerSize;
inline constexpr std::uint64_t kMaxSstFilterSize =
    std::uint64_t{kMaxFilterBitsPerKey / 8} * kMaxSstFilterKeys +
    kFilterHeaderSize + kFilterTrailerSize;

// KVHASH1: FNV-1a 64 of the key, then two SplitMix64-finalized values. The
// definition is fixed by the format; it must never depend on std::hash.
struct KvHash {
    std::uint64_t fnv = 0;
    std::uint64_t h1 = 0;
    std::uint64_t h2 = 0;  // always odd
};

std::uint64_t Fnv1a64(std::string_view data);
KvHash KvHash1FromFnv(std::uint64_t fnv);
KvHash KvHash1(std::string_view key);

// k = round(bits_per_key x 0.69), clamped to [1, 30].
std::uint32_t BloomProbes(std::uint32_t bits_per_key);

// Accumulates keys and encodes the filter block. Keeps one 64-bit value
// per key until Encode.
class BloomFilterBuilder {
public:
    // bits_per_key must be in [1, kMaxFilterBitsPerKey].
    explicit BloomFilterBuilder(std::uint32_t bits_per_key)
        : bits_per_key_(bits_per_key) {}

    void AddKey(std::string_view key) {
        fnv_.push_back(Fnv1a64(key));
    }

    std::size_t key_count() const {
        return fnv_.size();
    }

    // Encoded filter block size for n keys (header, bits, and CRC).
    std::uint64_t EncodedSize(std::uint64_t n) const;

    // Appends the encoded filter block for the keys added so far.
    void Encode(std::string* dst) const;

private:
    std::uint32_t bits_per_key_;
    std::vector<std::uint64_t> fnv_;
};

// A validated filter block, kept in memory for lookups.
class BloomFilter {
public:
    // Checks the 16-byte header against filter_length (the whole block)
    // and the SST's data region size, before the bit array is allocated.
    // An unknown filter_version or hash_id is UnsupportedFormat; any other
    // violation is Corruption.
    static Status CheckHeader(std::string_view header,
                              std::uint64_t filter_length,
                              std::uint64_t data_region);

    // Validates the complete block (header as above, then the CRC).
    static Status Decode(std::string_view block,
                         std::uint64_t data_region,
                         BloomFilter& out);

    bool empty() const {
        return bits_.empty();
    }

    std::uint32_t key_count() const {
        return key_count_;
    }

    // False only if the key was certainly not added. Requires !empty().
    bool MayContain(std::string_view key) const;

private:
    std::uint32_t key_count_ = 0;
    std::uint32_t probes_ = 0;
    std::string bits_;
};

}  // namespace kv
