#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/db.h"
#include "core/types.h"
#include "testing/temp_dir.h"
#include "testing/wal_builder.h"

namespace kv {
namespace {

namespace fs = std::filesystem;

using test::kRawDelete;
using test::kRawPut;
using test::RawRecord;
using test::RawRecordHeader;
using test::ReadBytesOrEmpty;
using test::SealedRecordHeader;
using test::WalFileHeaderBytes;
using test::WriteBytes;

bool Contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

const std::string kBinKey("\x00k\xff\x80", 4);
const std::string kBinValue("\xff\x00v\x00\x7f", 5);

struct Op {
    std::uint64_t sequence;
    std::uint8_t type;
    std::string key;
    std::string value;
};

// Five records of varied forms: ordinary Put, empty key and value, binary
// bytes after a sequence gap, Delete, and an overwrite with a longer value.
const std::vector<Op>& FiveOps() {
    static const std::vector<Op> ops = {
        {1, kRawPut, "alpha", "1"},
        {2, kRawPut, "", ""},
        {5, kRawPut, kBinKey, kBinValue},
        {6, kRawDelete, "alpha", ""},
        {7, kRawPut, "alpha", std::string(40, '\xab')},
    };
    return ops;
}

const std::vector<std::string> kUniverse = {"alpha", "", kBinKey, "new"};

using Model = std::map<std::string, std::string>;

// The stream plus record boundaries, computed from the independent
// builder: boundaries[i] is the end offset of the first i records.
struct Stream {
    std::string bytes;
    std::vector<std::size_t> boundaries;
};

Stream BuildStream(const std::vector<Op>& ops) {
    Stream s;
    s.bytes = WalFileHeaderBytes();
    s.boundaries.push_back(s.bytes.size());
    for (const Op& op : ops) {
        s.bytes += RawRecord(op.sequence, op.type, op.key, op.value);
        s.boundaries.push_back(s.bytes.size());
    }
    return s;
}

Model ApplyPrefix(const std::vector<Op>& ops, std::size_t n) {
    Model model;
    for (std::size_t i = 0; i < n; ++i) {
        if (ops[i].type == kRawPut) {
            model[ops[i].key] = ops[i].value;
        } else {
            model.erase(ops[i].key);
        }
    }
    return model;
}

class DBRecoveryTest : public ::testing::Test {
protected:
    fs::path Db() const { return tmp_.path() / "db"; }
    fs::path Wal() const { return Db() / "000001.wal"; }

    void ResetWal(const std::string& bytes) {
        fs::remove_all(Db());
        fs::create_directory(Db());
        ASSERT_TRUE(WriteBytes(Wal(), bytes));
    }

    Status OpenDb(std::unique_ptr<DB>& db, bool create = true) {
        Options options;
        options.create_if_missing = create;
        return DB::Open(Db(), options, db);
    }

    // Opening must fail with code, mention the WAL path, and leave the WAL
    // byte-for-byte unchanged (no truncation, rewrite, or replacement).
    void ExpectOpenFails(const std::string& wal,
                         Code code,
                         std::string_view detail) {
        ResetWal(wal);
        for (bool create : {false, true}) {
            std::unique_ptr<DB> db;
            const Status s = OpenDb(db, create);
            EXPECT_EQ(s.code, code) << s.ToString();
            EXPECT_TRUE(Contains(s.message, Wal().string())) << s.message;
            EXPECT_TRUE(Contains(s.message, detail)) << s.message;
            EXPECT_EQ(db, nullptr);
            EXPECT_EQ(ReadBytesOrEmpty(Wal()), wal);
            EXPECT_FALSE(fs::exists(Db() / "000001.wal.tmp"));
        }
    }

    test::TempDir tmp_;
};

void ExpectState(const DB& db, const Model& model) {
    for (const std::string& key : kUniverse) {
        std::string out = "stale";
        const Status s = db.Get(key, out);
        const auto it = model.find(key);
        if (it == model.end()) {
            EXPECT_EQ(s.code, Code::NotFound) << "key size " << key.size();
            EXPECT_TRUE(out.empty());
        } else {
            EXPECT_TRUE(s.ok()) << s.ToString();
            EXPECT_EQ(out, it->second);
        }
    }
}

// ---- C. Exhaustive byte truncation -----------------------------------------

TEST_F(DBRecoveryTest, EveryCutRecoversCompletePrefixAndAcceptsAppend) {
    const std::vector<Op>& ops = FiveOps();
    const Stream stream = BuildStream(ops);
    std::size_t cases = 0;

    for (std::size_t cut = 16; cut <= stream.bytes.size(); ++cut) {
        SCOPED_TRACE("cut " + std::to_string(cut));
        std::size_t complete = 0;
        while (complete < ops.size() &&
               stream.boundaries[complete + 1] <= cut) {
            ++complete;
        }
        const std::size_t good = stream.boundaries[complete];
        Model model = ApplyPrefix(ops, complete);

        ResetWal(stream.bytes.substr(0, cut));
        {
            std::unique_ptr<DB> db;
            const Status s = OpenDb(db, false);
            ASSERT_TRUE(s.ok()) << s.ToString();
            ExpectState(*db, model);
            EXPECT_EQ(fs::file_size(Wal()), good);  // torn tail removed

            ASSERT_TRUE(db->Put("new", "after repair").ok());
            model["new"] = "after repair";
            ExpectState(*db, model);
        }

        const std::uint64_t next_seq =
            complete == 0 ? 1 : ops[complete - 1].sequence + 1;
        EXPECT_EQ(ReadBytesOrEmpty(Wal()),
                  stream.bytes.substr(0, good) +
                      RawRecord(next_seq, kRawPut, "new", "after repair"));

        std::unique_ptr<DB> db;
        ASSERT_TRUE(OpenDb(db, false).ok());
        ExpectState(*db, model);
        ++cases;
    }
    EXPECT_EQ(cases, stream.bytes.size() - 16 + 1);
    RecordProperty("cuts_at_or_after_header", static_cast<int>(cases));
}

TEST_F(DBRecoveryTest, EveryCutInsideFileHeaderIsCorruption) {
    const Stream stream = BuildStream(FiveOps());
    for (std::size_t cut = 0; cut < 16; ++cut) {
        SCOPED_TRACE("cut " + std::to_string(cut));
        ExpectOpenFails(stream.bytes.substr(0, cut), Code::Corruption,
                        "truncated");
    }
}

// ---- D. Corruption ----------------------------------------------------------

TEST_F(DBRecoveryTest, EveryRecordHeaderByteMutationIsCorruption) {
    const Stream stream = BuildStream(FiveOps());
    for (std::size_t r = 0; r < FiveOps().size(); ++r) {
        const std::size_t start = stream.boundaries[r];
        for (std::size_t i = 0; i < 28; ++i) {
            SCOPED_TRACE("record " + std::to_string(r) + " byte " +
                         std::to_string(i));
            std::string wal = stream.bytes;
            wal[start + i] ^= 0x01;
            ExpectOpenFails(wal, Code::Corruption,
                            "offset " + std::to_string(start) +
                                ": header checksum");
        }
    }
}

TEST_F(DBRecoveryTest, EveryPayloadByteMutationIsCorruption) {
    const Stream stream = BuildStream(FiveOps());
    std::size_t mutated = 0;
    for (std::size_t r = 0; r < FiveOps().size(); ++r) {
        const std::size_t start = stream.boundaries[r];
        for (std::size_t pos = start + 28; pos < stream.boundaries[r + 1];
             ++pos) {
            SCOPED_TRACE("record " + std::to_string(r) + " pos " +
                         std::to_string(pos));
            std::string wal = stream.bytes;
            wal[pos] ^= 0x80;
            ExpectOpenFails(wal, Code::Corruption,
                            "offset " + std::to_string(start) +
                                ": payload checksum");
            ++mutated;
        }
    }
    // Includes the final record's bytes.
    EXPECT_GT(mutated, 40u);
}

// Header fields that are wrong but carry a correct header CRC, placed after
// one valid record so the reported offset is that of the bad record.
TEST_F(DBRecoveryTest, MalformedFieldsWithValidHeaderCrcAreCorruption) {
    const std::string prefix =
        WalFileHeaderBytes() + RawRecord(1, kRawPut, "a", "1");
    const std::string at = "offset " + std::to_string(prefix.size()) + ": ";

    auto record = [](RawRecordHeader h, const std::string& payload) {
        h.payload_crc = test::RefCrc32c(payload);
        return SealedRecordHeader(h) + payload;
    };

    RawRecordHeader reserved;
    reserved.sequence = 2;
    reserved.key_len = 1;
    reserved.reserved[1] = 0x01;
    ExpectOpenFails(prefix + record(reserved, "b"), Code::Corruption,
                    at + "reserved");

    for (std::uint8_t type : {0, 3, 0xff}) {
        RawRecordHeader bad_type;
        bad_type.sequence = 2;
        bad_type.type = type;
        bad_type.key_len = 1;
        ExpectOpenFails(prefix + record(bad_type, "b"), Code::Corruption,
                        at + "invalid record type");
    }

    RawRecordHeader delete_value;
    delete_value.sequence = 2;
    delete_value.type = kRawDelete;
    delete_value.key_len = 1;
    delete_value.value_len = 1;
    ExpectOpenFails(prefix + record(delete_value, "bv"), Code::Corruption,
                    at + "Delete record has value length");

    RawRecordHeader wrong_payload_crc;
    wrong_payload_crc.sequence = 2;
    wrong_payload_crc.key_len = 1;
    wrong_payload_crc.payload_crc = test::RefCrc32c("b") ^ 1;
    ExpectOpenFails(prefix + SealedRecordHeader(wrong_payload_crc) + "b",
                    Code::Corruption, at + "payload checksum");
}

// Invalid fields in a complete header at the tail, with the declared
// payload absent: this must fail closed, never be treated as a torn tail.
TEST_F(DBRecoveryTest, InvalidTailHeaderIsCorruptionNotIncomplete) {
    const std::string prefix =
        WalFileHeaderBytes() + RawRecord(1, kRawPut, "a", "1");
    const std::string at = "offset " + std::to_string(prefix.size()) + ": ";

    RawRecordHeader key_65537;
    key_65537.sequence = 2;
    key_65537.key_len = static_cast<std::uint32_t>(kMaxKeyLength + 1);
    ExpectOpenFails(prefix + SealedRecordHeader(key_65537), Code::Corruption,
                    at + "key length 65537");

    RawRecordHeader huge_value;
    huge_value.sequence = 2;
    huge_value.value_len = static_cast<std::uint32_t>(kMaxValueLength + 1);
    ExpectOpenFails(prefix + SealedRecordHeader(huge_value), Code::Corruption,
                    at + "value length");

    RawRecordHeader max_lengths;
    max_lengths.sequence = 2;
    max_lengths.key_len = 0xffffffffu;
    max_lengths.value_len = 0xffffffffu;
    ExpectOpenFails(prefix + SealedRecordHeader(max_lengths) + "partial",
                    Code::Corruption, at + "key length");

    RawRecordHeader duplicate_seq;
    duplicate_seq.sequence = 1;
    duplicate_seq.key_len = 10;
    ExpectOpenFails(prefix + SealedRecordHeader(duplicate_seq) + "abc",
                    Code::Corruption,
                    at + "sequence 1 does not exceed previous sequence 1");

    RawRecordHeader zero_seq;
    zero_seq.sequence = 0;
    zero_seq.key_len = 10;
    ExpectOpenFails(prefix + SealedRecordHeader(zero_seq), Code::Corruption,
                    at + "sequence is zero");
}

TEST_F(DBRecoveryTest, UnsupportedVersionIsDistinctFromCorruption) {
    const std::string records = RawRecord(1, kRawPut, "a", "1");
    ExpectOpenFails(WalFileHeaderBytes(2) + records, Code::UnsupportedFormat,
                    "unsupported WAL version 2");
    ExpectOpenFails(WalFileHeaderBytes(0) + records, Code::UnsupportedFormat,
                    "unsupported WAL version 0");
    // Version is checked before reserved bytes.
    ExpectOpenFails(WalFileHeaderBytes(2, 7) + records,
                    Code::UnsupportedFormat, "version");
    ExpectOpenFails(WalFileHeaderBytes(1, 7) + records, Code::Corruption,
                    "reserved");
}

}  // namespace
}  // namespace kv
