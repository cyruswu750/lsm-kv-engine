#include "bloom.h"

#include <algorithm>
#include <utility>

#include "coding.h"
#include "crc32c.h"

namespace kv {

namespace {

std::uint64_t Mix(std::uint64_t z) {
    z ^= z >> 30;
    z *= 0xbf58476d1ce4e5b9ull;
    z ^= z >> 27;
    z *= 0x94d049bb133111ebull;
    z ^= z >> 31;
    return z;
}

// Bit array bytes for n keys at bits_per_key: max(8, ceil(n x bpk / 8)).
// n <= kMaxSstFilterKeys and bpk <= 64, so the product fits easily.
std::uint64_t BitBytes(std::uint64_t n, std::uint32_t bits_per_key) {
    return std::max<std::uint64_t>(kMinFilterBitBytes,
                                   (n * bits_per_key + 7) / 8);
}

bool TestBit(const std::string& bits, std::uint64_t b) {
    return (static_cast<unsigned char>(bits[b / 8]) >> (b % 8)) & 1u;
}

}  // namespace

std::uint64_t Fnv1a64(std::string_view data) {
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (const char c : data) {
        h ^= static_cast<unsigned char>(c);
        h *= 0x100000001b3ull;
    }
    return h;
}

KvHash KvHash1FromFnv(std::uint64_t fnv) {
    KvHash h;
    h.fnv = fnv;
    h.h1 = Mix(fnv);
    h.h2 = Mix(fnv ^ 0x9e3779b97f4a7c15ull) | 1u;
    return h;
}

KvHash KvHash1(std::string_view key) {
    return KvHash1FromFnv(Fnv1a64(key));
}

std::uint32_t BloomProbes(std::uint32_t bits_per_key) {
    const std::uint32_t k = (bits_per_key * 69 + 50) / 100;
    return std::clamp<std::uint32_t>(k, 1, kMaxFilterProbes);
}

std::uint64_t BloomFilterBuilder::EncodedSize(std::uint64_t n) const {
    return kFilterHeaderSize + BitBytes(n, bits_per_key_) +
           kFilterTrailerSize;
}

void BloomFilterBuilder::Encode(std::string* dst) const {
    const std::uint32_t k = BloomProbes(bits_per_key_);
    const std::uint64_t bytes = BitBytes(fnv_.size(), bits_per_key_);
    const std::uint64_t m = 8 * bytes;
    std::string bits(static_cast<std::size_t>(bytes), '\0');
    for (const std::uint64_t fnv : fnv_) {
        const KvHash h = KvHash1FromFnv(fnv);
        for (std::uint32_t i = 0; i < k; ++i) {
            const std::uint64_t b = (h.h1 + i * h.h2) % m;
            bits[b / 8] = static_cast<char>(
                static_cast<unsigned char>(bits[b / 8]) | (1u << (b % 8)));
        }
    }
    const std::size_t start = dst->size();
    PutFixed32(dst, kFilterVersion);
    PutFixed32(dst, kKvHash1);
    PutFixed32(dst, static_cast<std::uint32_t>(fnv_.size()));
    PutFixed32(dst, k);
    dst->append(bits);
    PutFixed32(dst, Crc32c(std::string_view(*dst).substr(start)));
}

Status BloomFilter::CheckHeader(std::string_view header,
                                std::uint64_t filter_length,
                                std::uint64_t data_region) {
    if (header.size() < kFilterHeaderSize) {
        return Status::Corruption("filter header is truncated");
    }
    const std::uint32_t version = DecodeFixed32(header.data());
    const std::uint32_t hash = DecodeFixed32(header.data() + 4);
    // Identified before anything else is trusted, like the SST footer.
    if (version != kFilterVersion) {
        return Status::UnsupportedFormat("unsupported filter version " +
                                         std::to_string(version));
    }
    if (hash != kKvHash1) {
        return Status::UnsupportedFormat("unsupported filter hash " +
                                         std::to_string(hash));
    }
    const std::uint32_t n = DecodeFixed32(header.data() + 8);
    const std::uint32_t k = DecodeFixed32(header.data() + 12);
    // Every entry takes at least 17 bytes of the data region.
    if (n == 0 || n > kMaxSstFilterKeys || n > data_region / 17) {
        return Status::Corruption("filter key count " + std::to_string(n) +
                                  " is out of range");
    }
    if (k == 0 || k > kMaxFilterProbes) {
        return Status::Corruption("filter probe count " + std::to_string(k) +
                                  " is out of range");
    }
    if (filter_length < kMinSstFilterSize ||
        filter_length > kMaxSstFilterSize) {
        return Status::Corruption("filter length " +
                                  std::to_string(filter_length) +
                                  " is out of range");
    }
    // At most 64 bits per key; 8 x n fits in 64 bits since n < 2^23.
    const std::uint64_t bytes =
        filter_length - kFilterHeaderSize - kFilterTrailerSize;
    const std::uint64_t max_bytes = std::max<std::uint64_t>(
        kMinFilterBitBytes, std::uint64_t{kMaxFilterBitsPerKey / 8} * n);
    if (bytes > max_bytes) {
        return Status::Corruption("filter has " + std::to_string(bytes) +
                                  " bit bytes for " + std::to_string(n) +
                                  " keys; the limit is " +
                                  std::to_string(max_bytes));
    }
    return Status::Ok();
}

Status BloomFilter::Decode(std::string_view block,
                           std::uint64_t data_region,
                           BloomFilter& out) {
    Status s = CheckHeader(block, block.size(), data_region);
    if (!s.ok()) {
        return s;
    }
    const std::size_t crc_at = block.size() - kFilterTrailerSize;
    if (DecodeFixed32(block.data() + crc_at) !=
        Crc32c(block.substr(0, crc_at))) {
        return Status::Corruption("filter checksum mismatch");
    }
    BloomFilter f;
    f.key_count_ = DecodeFixed32(block.data() + 8);
    f.probes_ = DecodeFixed32(block.data() + 12);
    f.bits_.assign(block.substr(kFilterHeaderSize,
                                crc_at - kFilterHeaderSize));
    out = std::move(f);
    return Status::Ok();
}

bool BloomFilter::MayContain(std::string_view key) const {
    const KvHash h = KvHash1(key);
    const std::uint64_t m = 8 * std::uint64_t{bits_.size()};
    for (std::uint32_t i = 0; i < probes_; ++i) {
        if (!TestBit(bits_, (h.h1 + i * h.h2) % m)) {
            return false;
        }
    }
    return true;
}

}  // namespace kv
