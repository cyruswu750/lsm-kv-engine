#include "manifest.h"

#include <algorithm>
#include <memory>
#include <unordered_set>
#include <utility>

#include "coding.h"
#include "crc32c.h"
#include "sst.h"

namespace kv {

namespace {

constexpr std::size_t kVersionPrefixSize = 12;

// Checks magic, then version, on the first 12 bytes; data must hold them.
Status CheckMagicAndVersion(std::string_view data) {
    if (data.substr(0, kManifestMagic.size()) != kManifestMagic) {
        return Status::Corruption("manifest has bad magic");
    }
    const std::uint32_t version = DecodeFixed32(data.data() + 8);
    if (version != kManifestVersion) {
        // Not covered by a checksum check first, like the WAL header.
        return Status::UnsupportedFormat(
            "unsupported manifest version " + std::to_string(version) +
            (version == 1 ? " (an M2 database; this version reads only "
                            "manifest version 2 and does not migrate)"
                          : ""));
    }
    return Status::Ok();
}

std::string SstContext(std::size_t i, const ManifestSst& sst) {
    return "SST entry " + std::to_string(i) + " (file " +
           std::to_string(sst.file_id) + "): ";
}

}  // namespace

Status ValidateManifest(const Manifest& m) {
    if (m.generation == 0) {
        return Status::Corruption("manifest generation is zero");
    }
    if (m.active_wal_id == 0) {
        return Status::Corruption("manifest active WAL ID is zero");
    }
    if (m.ssts.size() > kMaxManifestSsts) {
        return Status::Corruption("manifest lists " +
                                  std::to_string(m.ssts.size()) +
                                  " SSTs; the limit is " +
                                  std::to_string(kMaxManifestSsts));
    }
    if (m.base_run_count > m.ssts.size()) {
        return Status::Corruption("manifest base run count " +
                                  std::to_string(m.base_run_count) +
                                  " exceeds its SST count " +
                                  std::to_string(m.ssts.size()));
    }

    std::uint64_t max_id = m.active_wal_id;
    std::uint64_t base_max = 0;  // greatest sequence in the base run
    std::unordered_set<std::uint64_t> ids;
    ids.insert(m.active_wal_id);
    for (std::size_t i = 0; i < m.ssts.size(); ++i) {
        const ManifestSst& sst = m.ssts[i];
        const std::string context = SstContext(i, sst);
        if (sst.file_id == 0) {
            return Status::Corruption(context + "file ID is zero");
        }
        if (!ids.insert(sst.file_id).second) {
            return Status::Corruption(context + "duplicate file ID");
        }
        max_id = std::max(max_id, sst.file_id);
        if (sst.file_size < kMinSstFileSize) {
            return Status::Corruption(context + "file size " +
                                      std::to_string(sst.file_size) +
                                      " is below the minimum SST size");
        }
        if (sst.min_sequence == 0 || sst.min_sequence > sst.max_sequence) {
            return Status::Corruption(
                context + "invalid sequence range [" +
                std::to_string(sst.min_sequence) + ", " +
                std::to_string(sst.max_sequence) + "]");
        }
        if (sst.max_sequence > m.checkpoint) {
            return Status::Corruption(
                context + "max sequence " + std::to_string(sst.max_sequence) +
                " exceeds checkpoint " + std::to_string(m.checkpoint));
        }
        // Base SSTs may overlap one another. Each flush run follows the
        // whole base run and the previous flush run.
        if (i < m.base_run_count) {
            base_max = std::max(base_max, sst.max_sequence);
        } else if (i == m.base_run_count) {
            if (i > 0 && base_max >= sst.min_sequence) {
                return Status::Corruption(
                    context + "sequence range does not follow the base run");
            }
        } else if (m.ssts[i - 1].max_sequence >= sst.min_sequence) {
            return Status::Corruption(
                context + "sequence range does not follow the previous SST");
        }
    }
    if (m.next_file_id <= max_id) {
        return Status::Corruption("manifest next file ID " +
                                  std::to_string(m.next_file_id) +
                                  " does not exceed referenced ID " +
                                  std::to_string(max_id));
    }
    return Status::Ok();
}

void EncodeManifest(const Manifest& m, std::string* dst) {
    const std::size_t start = dst->size();
    dst->append(kManifestMagic);
    PutFixed32(dst, kManifestVersion);
    PutFixed32(dst, 0);
    PutFixed64(dst, m.generation);
    PutFixed64(dst, m.next_file_id);
    PutFixed64(dst, m.checkpoint);
    PutFixed64(dst, m.active_wal_id);
    PutFixed32(dst, static_cast<std::uint32_t>(m.ssts.size()));
    PutFixed32(dst, m.base_run_count);
    for (const ManifestSst& sst : m.ssts) {
        PutFixed64(dst, sst.file_id);
        PutFixed64(dst, sst.file_size);
        PutFixed64(dst, sst.min_sequence);
        PutFixed64(dst, sst.max_sequence);
    }
    PutFixed32(dst, Crc32c(std::string_view(*dst).substr(start)));
}

Status DecodeManifest(std::string_view data, Manifest& out) {
    if (data.size() < kVersionPrefixSize) {
        return Status::Corruption("manifest truncated: " +
                                  std::to_string(data.size()) + " bytes");
    }
    Status s = CheckMagicAndVersion(data);
    if (!s.ok()) {
        return s;
    }
    if (data.size() < kManifestMinSize || data.size() > kMaxManifestSize) {
        return Status::Corruption("manifest size " +
                                  std::to_string(data.size()) +
                                  " is out of range");
    }

    const std::size_t crc_offset = data.size() - 4;
    if (DecodeFixed32(data.data() + crc_offset) !=
        Crc32c(data.substr(0, crc_offset))) {
        return Status::Corruption("manifest checksum mismatch");
    }

    const char* p = data.data();
    if (DecodeFixed32(p + 12) != 0) {
        return Status::Corruption("manifest reserved field is nonzero");
    }
    const std::uint32_t count = DecodeFixed32(p + 48);
    if (count > kMaxManifestSsts) {
        return Status::Corruption("manifest SST count " +
                                  std::to_string(count) + " exceeds limit");
    }
    const std::uint64_t expected_size =
        kManifestMinSize + std::uint64_t{kManifestSstEntrySize} * count;
    if (data.size() != expected_size) {
        return Status::Corruption(
            "manifest size " + std::to_string(data.size()) + " does not match " +
            std::to_string(count) + " SST entries (expected " +
            std::to_string(expected_size) + ")");
    }

    Manifest m;
    m.generation = DecodeFixed64(p + 16);
    m.next_file_id = DecodeFixed64(p + 24);
    m.checkpoint = DecodeFixed64(p + 32);
    m.active_wal_id = DecodeFixed64(p + 40);
    m.base_run_count = DecodeFixed32(p + 52);
    m.ssts.resize(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const char* e = p + kManifestHeaderSize + i * kManifestSstEntrySize;
        m.ssts[i].file_id = DecodeFixed64(e);
        m.ssts[i].file_size = DecodeFixed64(e + 8);
        m.ssts[i].min_sequence = DecodeFixed64(e + 16);
        m.ssts[i].max_sequence = DecodeFixed64(e + 24);
    }
    s = ValidateManifest(m);
    if (!s.ok()) {
        return s;
    }
    out = std::move(m);
    return Status::Ok();
}

Status ReadManifestFile(const std::filesystem::path& path,
                        PosixSyscalls& syscalls,
                        Manifest& out) {
    std::unique_ptr<RandomAccessFile> file;
    Status s = RandomAccessFile::Open(path, syscalls, file);
    if (!s.ok()) {
        return s;
    }
    const std::string& name = file->path();
    const std::uint64_t size = file->size();

    // Read the magic and version first, so an unsupported version is
    // reported before the version 2 size limits are applied.
    if (size < kVersionPrefixSize) {
        return Status::Corruption(name + ": manifest truncated: " +
                                  std::to_string(size) + " bytes");
    }
    std::string data(kVersionPrefixSize, '\0');
    s = file->Read(0, data.size(), data.data());
    if (!s.ok()) {
        return s;
    }
    s = CheckMagicAndVersion(data);
    if (!s.ok()) {
        return Status{s.code, name + ": " + s.message};
    }
    if (size < kManifestMinSize || size > kMaxManifestSize) {
        return Status::Corruption(name + ": manifest size " +
                                  std::to_string(size) + " is out of range");
    }

    data.resize(static_cast<std::size_t>(size));
    s = file->Read(0, data.size(), data.data());
    if (!s.ok()) {
        return s;
    }
    s = file->Close();
    if (!s.ok()) {
        return s;
    }
    s = DecodeManifest(data, out);
    if (!s.ok()) {
        return Status{s.code, name + ": " + s.message};
    }
    return Status::Ok();
}

}  // namespace kv
