#include <gtest/gtest.h>

#include <sys/stat.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/db.h"
#include "core/file_lock.h"
#include "core/types.h"
#include "testing/temp_dir.h"
#include "testing/wal_builder.h"

namespace kv {
namespace {

namespace fs = std::filesystem;

using test::kRawDelete;
using test::kRawPut;
using test::RawRecord;
using test::ReadBytesOrEmpty;
using test::WalFileHeaderBytes;
using test::WriteBytes;

constexpr std::uint64_t kMaxSeq = std::numeric_limits<std::uint64_t>::max();

bool Contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

std::vector<std::string> Entries(const fs::path& dir) {
    std::vector<std::string> names;
    for (const auto& entry : fs::directory_iterator(dir)) {
        names.push_back(entry.path().filename().string());
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::size_t OpenFdCount() {
    std::size_t n = 0;
    for (auto it = fs::directory_iterator("/proc/self/fd");
         it != fs::directory_iterator(); ++it) {
        ++n;
    }
    return n;
}

// Returns the value, std::nullopt for NotFound, and records a failure for
// any other status. out is pre-filled to check that it is overwritten.
std::optional<std::string> Lookup(const DB& db, std::string_view key) {
    std::string out = "stale";
    const Status s = db.Get(key, out);
    if (s.code == Code::NotFound) {
        EXPECT_TRUE(out.empty()) << "Get must clear output on NotFound";
        return std::nullopt;
    }
    EXPECT_TRUE(s.ok()) << s.ToString();
    return out;
}

const std::string kBinKey("\x00k\xff\x80", 4);
const std::string kBinValue("\xff\x00v\x00\x7f", 5);

class DBTest : public ::testing::Test {
protected:
    fs::path Db() const { return tmp_.path() / "db"; }
    fs::path Wal() const { return Db() / "000001.wal"; }
    fs::path Temp() const { return Db() / "000001.wal.tmp"; }
    fs::path LockPath() const { return Db() / "LOCK"; }

    Status OpenDb(std::unique_ptr<DB>& db, bool create = true) {
        Options options;
        options.create_if_missing = create;
        return DB::Open(Db(), options, db);
    }

    std::unique_ptr<DB> MustOpen() {
        std::unique_ptr<DB> db;
        const Status s = OpenDb(db);
        EXPECT_TRUE(s.ok()) << s.ToString();
        return db;
    }

    // Creates the database directory holding exactly these WAL bytes.
    void MakeWal(const std::string& bytes) {
        fs::create_directory(Db());
        ASSERT_TRUE(WriteBytes(Wal(), bytes));
    }

    test::TempDir tmp_;
};

// ---- A. DB behavior --------------------------------------------------------

TEST_F(DBTest, PutGetDelete) {
    auto db = MustOpen();
    ASSERT_NE(db, nullptr);
    ASSERT_TRUE(db->Put("k", "v").ok());
    EXPECT_EQ(Lookup(*db, "k"), "v");
    ASSERT_TRUE(db->Delete("k").ok());
    EXPECT_EQ(Lookup(*db, "k"), std::nullopt);
    EXPECT_EQ(Lookup(*db, "never"), std::nullopt);
}

TEST_F(DBTest, OverwriteReplacesValue) {
    auto db = MustOpen();
    ASSERT_TRUE(db->Put("k", "one").ok());
    ASSERT_TRUE(db->Put("k", "two").ok());
    EXPECT_EQ(Lookup(*db, "k"), "two");
    ASSERT_TRUE(db->Delete("k").ok());
    ASSERT_TRUE(db->Put("k", "three").ok());
    EXPECT_EQ(Lookup(*db, "k"), "three");
}

TEST_F(DBTest, WalBytesMatchIndependentEncoding) {
    auto db = MustOpen();
    ASSERT_TRUE(db->Put("a", "hi").ok());
    ASSERT_TRUE(db->Delete("a").ok());
    ASSERT_TRUE(db->Put(kBinKey, kBinValue).ok());
    ASSERT_TRUE(db->Put("", "").ok());

    const std::string expected = WalFileHeaderBytes() +
                                 RawRecord(1, kRawPut, "a", "hi") +
                                 RawRecord(2, kRawDelete, "a", "") +
                                 RawRecord(3, kRawPut, kBinKey, kBinValue) +
                                 RawRecord(4, kRawPut, "", "");
    EXPECT_EQ(ReadBytesOrEmpty(Wal()), expected);
}

TEST_F(DBTest, DeleteOfAbsentKeyRecordsDurableTombstone) {
    {
        auto db = MustOpen();
        ASSERT_TRUE(db->Delete("absent").ok());
        EXPECT_EQ(Lookup(*db, "absent"), std::nullopt);
    }
    EXPECT_EQ(ReadBytesOrEmpty(Wal()),
              WalFileHeaderBytes() + RawRecord(1, kRawDelete, "absent", ""));
    auto db = MustOpen();
    EXPECT_EQ(Lookup(*db, "absent"), std::nullopt);
}

TEST_F(DBTest, EmptyValueIsDistinctFromDeletion) {
    {
        auto db = MustOpen();
        ASSERT_TRUE(db->Put("empty", "").ok());
        ASSERT_TRUE(db->Put("gone", "x").ok());
        ASSERT_TRUE(db->Delete("gone").ok());
        EXPECT_EQ(Lookup(*db, "empty"), "");
        EXPECT_EQ(Lookup(*db, "gone"), std::nullopt);
    }
    auto db = MustOpen();
    EXPECT_EQ(Lookup(*db, "empty"), "");
    EXPECT_EQ(Lookup(*db, "gone"), std::nullopt);
}

TEST_F(DBTest, EmptyAndBinaryKeysAndValuesSurviveReopen) {
    {
        auto db = MustOpen();
        ASSERT_TRUE(db->Put("", "empty key").ok());
        ASSERT_TRUE(db->Put(kBinKey, kBinValue).ok());
        ASSERT_TRUE(db->Put(std::string(1, '\0'), std::string(3, '\0')).ok());
    }
    auto db = MustOpen();
    EXPECT_EQ(Lookup(*db, ""), "empty key");
    EXPECT_EQ(Lookup(*db, kBinKey), kBinValue);
    EXPECT_EQ(Lookup(*db, std::string(1, '\0')), std::string(3, '\0'));
    EXPECT_EQ(Lookup(*db, std::string(2, '\0')), std::nullopt);
}

TEST_F(DBTest, SizeBoundaries) {
    const std::string max_key(kMaxKeyLength, 'k');
    const std::string big_key(kMaxKeyLength + 1, 'k');
    const std::string max_value(kMaxValueLength, '\x80');
    const std::string big_value(kMaxValueLength + 1, 'v');
    {
        auto db = MustOpen();
        ASSERT_TRUE(db->Put(max_key, "v").ok());
        ASSERT_TRUE(db->Put("big", max_value).ok());
        const auto size = fs::file_size(Wal());

        EXPECT_EQ(db->Put(big_key, "v").code, Code::InvalidArgument);
        EXPECT_EQ(db->Delete(big_key).code, Code::InvalidArgument);
        EXPECT_EQ(db->Put("k", big_value).code, Code::InvalidArgument);
        std::string out = "stale";
        EXPECT_EQ(db->Get(big_key, out).code, Code::InvalidArgument);
        EXPECT_TRUE(out.empty());
        EXPECT_EQ(fs::file_size(Wal()), size);  // rejected before I/O

        // Rejected input does not invalidate the handle.
        ASSERT_TRUE(db->Put("after", "ok").ok());
    }
    auto db = MustOpen();
    EXPECT_EQ(Lookup(*db, max_key), "v");
    EXPECT_EQ(Lookup(*db, "big"), max_value);
    EXPECT_EQ(Lookup(*db, "after"), "ok");
}

TEST_F(DBTest, GetClearsOutputOnEveryNonOkResult) {
    auto db = MustOpen();
    ASSERT_TRUE(db->Put("k", "v").ok());
    ASSERT_TRUE(db->Delete("k").ok());

    std::string out = "stale";
    EXPECT_EQ(db->Get("k", out).code, Code::NotFound);
    EXPECT_TRUE(out.empty());
    out = "stale";
    EXPECT_EQ(db->Get("missing", out).code, Code::NotFound);
    EXPECT_TRUE(out.empty());
    out = "stale";
    EXPECT_EQ(db->Get(std::string(kMaxKeyLength + 1, 'x'), out).code,
              Code::InvalidArgument);
    EXPECT_TRUE(out.empty());
}

TEST_F(DBTest, InvalidOptionsAndPathsCreateNothing) {
    std::unique_ptr<DB> db;
    Options options;
    options.memtable_bytes = 0;
    EXPECT_EQ(DB::Open(Db(), options, db).code, Code::InvalidArgument);
    EXPECT_EQ(db, nullptr);
    EXPECT_FALSE(fs::exists(Db()));

    EXPECT_EQ(DB::Open("", Options{}, db).code, Code::InvalidArgument);
    const fs::path nul = tmp_.path() / std::string("db\0x", 4);
    EXPECT_EQ(DB::Open(nul, Options{}, db).code, Code::InvalidArgument);
    EXPECT_EQ(db, nullptr);
    EXPECT_FALSE(fs::exists(Db()));
}

TEST_F(DBTest, NonEmptyOutputHandleIsPreserved) {
    auto db = MustOpen();
    DB* original = db.get();
    const fs::path other = tmp_.path() / "other";
    EXPECT_EQ(DB::Open(other, Options{}, db).code, Code::InvalidArgument);
    EXPECT_EQ(db.get(), original);
    EXPECT_FALSE(fs::exists(other));
    ASSERT_TRUE(db->Put("still", "usable").ok());
}

// ---- B. Recovery -----------------------------------------------------------

TEST_F(DBTest, ReopenRestoresStateAndContinuesSequence) {
    {
        auto db = MustOpen();
        ASSERT_TRUE(db->Put("a", "1").ok());
        ASSERT_TRUE(db->Put("b", "2").ok());
        ASSERT_TRUE(db->Delete("a").ok());
    }
    {
        auto db = MustOpen();
        EXPECT_EQ(Lookup(*db, "a"), std::nullopt);
        EXPECT_EQ(Lookup(*db, "b"), "2");
        ASSERT_TRUE(db->Put("c", "3").ok());
    }
    EXPECT_EQ(ReadBytesOrEmpty(Wal()),
              WalFileHeaderBytes() + RawRecord(1, kRawPut, "a", "1") +
                  RawRecord(2, kRawPut, "b", "2") +
                  RawRecord(3, kRawDelete, "a", "") +
                  RawRecord(4, kRawPut, "c", "3"));
}

TEST_F(DBTest, HeaderOnlyWalOpensEmpty) {
    MakeWal(WalFileHeaderBytes());
    auto db = MustOpen();
    EXPECT_EQ(Lookup(*db, ""), std::nullopt);
    ASSERT_TRUE(db->Put("k", "v").ok());
    EXPECT_EQ(ReadBytesOrEmpty(Wal()),
              WalFileHeaderBytes() + RawRecord(1, kRawPut, "k", "v"));
}

TEST_F(DBTest, SequenceGapsAreAccepted) {
    const std::string wal = WalFileHeaderBytes() +
                            RawRecord(3, kRawPut, "a", "1") +
                            RawRecord(10, kRawPut, "b", "2") +
                            RawRecord(11, kRawDelete, "a", "");
    MakeWal(wal);
    {
        auto db = MustOpen();
        EXPECT_EQ(Lookup(*db, "a"), std::nullopt);
        EXPECT_EQ(Lookup(*db, "b"), "2");
        ASSERT_TRUE(db->Put("c", "3").ok());
    }
    EXPECT_EQ(ReadBytesOrEmpty(Wal()), wal + RawRecord(12, kRawPut, "c", "3"));
}

TEST_F(DBTest, InvalidSequenceOrderIsCorruption) {
    struct Case {
        const char* name;
        std::string wal;
        std::size_t bad_offset;
    };
    const std::string first = RawRecord(5, kRawPut, "a", "1");
    const std::size_t second_offset = 16 + first.size();
    const Case cases[] = {
        {"zero", WalFileHeaderBytes() + RawRecord(0, kRawPut, "a", "1"), 16},
        {"duplicate",
         WalFileHeaderBytes() + first + RawRecord(5, kRawPut, "b", "2"),
         second_offset},
        {"decreasing",
         WalFileHeaderBytes() + first + RawRecord(4, kRawPut, "b", "2"),
         second_offset},
    };
    for (const Case& c : cases) {
        fs::remove_all(Db());
        MakeWal(c.wal);
        std::unique_ptr<DB> db;
        const Status s = OpenDb(db);
        EXPECT_EQ(s.code, Code::Corruption) << c.name << ": " << s.ToString();
        EXPECT_TRUE(Contains(s.message, Wal().string())) << s.message;
        EXPECT_TRUE(Contains(s.message,
                             "offset " + std::to_string(c.bad_offset)))
            << s.message;
        EXPECT_EQ(db, nullptr);
        EXPECT_EQ(ReadBytesOrEmpty(Wal()), c.wal) << c.name;
    }
}

TEST_F(DBTest, MaxSequenceIsPreservedAndFurtherMutationsRejected) {
    const std::string wal =
        WalFileHeaderBytes() + RawRecord(kMaxSeq, kRawPut, "k", "v");
    MakeWal(wal);
    for (int round = 0; round < 2; ++round) {
        auto db = MustOpen();
        ASSERT_NE(db, nullptr);
        EXPECT_EQ(Lookup(*db, "k"), "v");
        EXPECT_EQ(db->Put("k", "w").code, Code::InvalidArgument);
        EXPECT_EQ(db->Delete("k").code, Code::InvalidArgument);
        // Rejection does no I/O and leaves the handle usable for reads.
        EXPECT_EQ(Lookup(*db, "k"), "v");
        EXPECT_EQ(ReadBytesOrEmpty(Wal()), wal);
    }
}

TEST_F(DBTest, PutCanUseMaxSequenceOnce) {
    const std::string wal =
        WalFileHeaderBytes() + RawRecord(kMaxSeq - 1, kRawPut, "k", "v");
    MakeWal(wal);
    {
        auto db = MustOpen();
        ASSERT_TRUE(db->Put("k", "last").ok());
        EXPECT_EQ(db->Put("k", "overflow").code, Code::InvalidArgument);
        EXPECT_EQ(Lookup(*db, "k"), "last");
    }
    EXPECT_EQ(ReadBytesOrEmpty(Wal()),
              wal + RawRecord(kMaxSeq, kRawPut, "k", "last"));
    auto db = MustOpen();
    EXPECT_EQ(Lookup(*db, "k"), "last");
    EXPECT_EQ(db->Delete("k").code, Code::InvalidArgument);
}

// ---- E. Directory state machine and locking --------------------------------

TEST_F(DBTest, MissingDirectoryWithoutCreateIsNotFound) {
    std::unique_ptr<DB> db;
    EXPECT_EQ(OpenDb(db, false).code, Code::NotFound);
    EXPECT_EQ(db, nullptr);
    EXPECT_FALSE(fs::exists(Db()));
}

TEST_F(DBTest, MissingDirectoryWithCreateInitializes) {
    auto db = MustOpen();
    ASSERT_NE(db, nullptr);
    EXPECT_EQ(Entries(Db()), (std::vector<std::string>{"000001.wal", "LOCK"}));
    EXPECT_EQ(ReadBytesOrEmpty(Wal()), WalFileHeaderBytes());
}

TEST_F(DBTest, TrailingSeparatorInPathIsAccepted) {
    std::unique_ptr<DB> db;
    const fs::path with_slash = Db().string() + "/";
    ASSERT_TRUE(DB::Open(with_slash, Options{}, db).ok());
    ASSERT_TRUE(db->Put("k", "v").ok());
    db.reset();
    ASSERT_TRUE(DB::Open(with_slash, Options{}, db).ok());
    EXPECT_EQ(Lookup(*db, "k"), "v");
}

TEST_F(DBTest, MissingParentIsNotCreated) {
    std::unique_ptr<DB> db;
    const fs::path nested = tmp_.path() / "parent" / "db";
    EXPECT_EQ(DB::Open(nested, Options{}, db).code, Code::IOError);
    EXPECT_EQ(db, nullptr);
    EXPECT_FALSE(fs::exists(tmp_.path() / "parent"));
}

TEST_F(DBTest, PathThatIsNotADirectoryIsRejected) {
    fs::create_directory(tmp_.path() / "real");
    ASSERT_TRUE(WriteBytes(tmp_.path() / "file", "x"));
    fs::create_directory_symlink(tmp_.path() / "real", tmp_.path() / "link");
    fs::create_symlink(tmp_.path() / "nowhere", tmp_.path() / "dangling");

    for (const char* name : {"file", "link", "dangling"}) {
        for (bool create : {false, true}) {
            std::unique_ptr<DB> db;
            Options options;
            options.create_if_missing = create;
            const Status s = DB::Open(tmp_.path() / name, options, db);
            EXPECT_EQ(s.code, Code::InvalidArgument) << name << s.ToString();
            EXPECT_EQ(db, nullptr);
        }
    }
    EXPECT_EQ(ReadBytesOrEmpty(tmp_.path() / "file"), "x");
    EXPECT_TRUE(Entries(tmp_.path() / "real").empty());
    EXPECT_TRUE(fs::is_symlink(tmp_.path() / "dangling"));
}

// Uninitialized states: empty, LOCK only, tmp only, LOCK + tmp.
TEST_F(DBTest, UninitializedStatesFollowCreateIfMissing) {
    struct Case {
        const char* name;
        bool lock;
        bool temp;
    };
    const Case cases[] = {
        {"empty", false, false},
        {"lock only", true, false},
        {"tmp only", false, true},
        {"lock and tmp", true, true},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.name);
        fs::remove_all(Db());
        fs::create_directory(Db());
        if (c.lock) {
            ASSERT_TRUE(WriteBytes(LockPath(), "lock contents"));
        }
        if (c.temp) {
            ASSERT_TRUE(WriteBytes(Temp(), "partial header"));
        }
        const std::vector<std::string> before = Entries(Db());

        std::unique_ptr<DB> db;
        EXPECT_EQ(OpenDb(db, false).code, Code::NotFound);
        EXPECT_EQ(db, nullptr);
        EXPECT_EQ(Entries(Db()), before);  // no LOCK, WAL, or tmp removal
        if (c.temp) {
            EXPECT_EQ(ReadBytesOrEmpty(Temp()), "partial header");
        }

        ASSERT_TRUE(OpenDb(db, true).ok());
        EXPECT_EQ(Entries(Db()),
                  (std::vector<std::string>{"000001.wal", "LOCK"}));
        EXPECT_EQ(ReadBytesOrEmpty(Wal()), WalFileHeaderBytes());
        if (c.lock) {
            EXPECT_EQ(ReadBytesOrEmpty(LockPath()), "lock contents");
        }
        ASSERT_TRUE(db->Put("k", "v").ok());
    }
}

TEST_F(DBTest, UnrelatedEntriesWithoutWalAreRejected) {
    struct Case {
        const char* name;
        bool is_dir;
        bool with_lock;
    };
    const Case cases[] = {
        {"notes.txt", false, false},  {"unrelated.tmp", false, false},
        {".hidden", false, false},    {"000002.wal", false, false},
        {"subdir", true, false},      {"000001.wal.tmp", true, false},
        {"notes.txt", false, true},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.name);
        fs::remove_all(Db());
        fs::create_directory(Db());
        if (c.is_dir) {
            fs::create_directory(Db() / c.name);
        } else {
            ASSERT_TRUE(WriteBytes(Db() / c.name, "keep"));
        }
        if (c.with_lock) {
            ASSERT_TRUE(WriteBytes(LockPath(), ""));
        }
        const std::vector<std::string> before = Entries(Db());

        for (bool create : {false, true}) {
            std::unique_ptr<DB> db;
            const Status s = OpenDb(db, create);
            EXPECT_EQ(s.code, Code::InvalidArgument) << s.ToString();
            EXPECT_TRUE(Contains(s.message, c.name)) << s.message;
            EXPECT_EQ(db, nullptr);
            EXPECT_EQ(Entries(Db()), before);
        }
        if (!c.is_dir) {
            EXPECT_EQ(ReadBytesOrEmpty(Db() / c.name), "keep");
        }
    }
}

TEST_F(DBTest, NonRegularLockIsRejected) {
    for (bool symlink : {false, true}) {
        SCOPED_TRACE(symlink ? "symlink" : "directory");
        fs::remove_all(Db());
        fs::create_directory(Db());
        ASSERT_TRUE(WriteBytes(Wal(), WalFileHeaderBytes()));
        if (symlink) {
            ASSERT_TRUE(WriteBytes(tmp_.path() / "elsewhere", ""));
            fs::create_symlink(tmp_.path() / "elsewhere", LockPath());
        } else {
            fs::create_directory(LockPath());
        }
        std::unique_ptr<DB> db;
        const Status s = OpenDb(db);
        EXPECT_EQ(s.code, Code::InvalidArgument) << s.ToString();
        EXPECT_EQ(db, nullptr);
        EXPECT_EQ(ReadBytesOrEmpty(Wal()), WalFileHeaderBytes());
    }
}

TEST_F(DBTest, ExistingWalToleratesUnrelatedEntries) {
    MakeWal(WalFileHeaderBytes() + RawRecord(1, kRawPut, "k", "v"));
    ASSERT_TRUE(WriteBytes(Db() / "notes.txt", "keep"));
    ASSERT_TRUE(WriteBytes(Db() / "unrelated.tmp", "keep"));
    fs::create_directory(Db() / "subdir");

    for (bool create : {false, true}) {
        std::unique_ptr<DB> db;
        ASSERT_TRUE(OpenDb(db, create).ok());
        EXPECT_EQ(Lookup(*db, "k"), "v");
    }
    EXPECT_EQ(ReadBytesOrEmpty(Db() / "notes.txt"), "keep");
    EXPECT_EQ(ReadBytesOrEmpty(Db() / "unrelated.tmp"), "keep");
    EXPECT_TRUE(fs::is_directory(Db() / "subdir"));
}

TEST_F(DBTest, ExistingWalRemovesOnlyRecognizedLeftoverTemp) {
    MakeWal(WalFileHeaderBytes() + RawRecord(1, kRawPut, "k", "v"));
    ASSERT_TRUE(WriteBytes(Temp(), "stale"));
    {
        std::unique_ptr<DB> db;
        ASSERT_TRUE(OpenDb(db, false).ok());
        EXPECT_EQ(Lookup(*db, "k"), "v");
    }
    EXPECT_FALSE(fs::exists(Temp()));

    // A non-regular entry with that name is not the engine's file.
    fs::create_directory(Temp());
    std::unique_ptr<DB> db;
    ASSERT_TRUE(OpenDb(db).ok());
    EXPECT_TRUE(fs::is_directory(Temp()));
}

TEST_F(DBTest, InvalidFinalWalFailsWithoutReplacement) {
    struct Case {
        const char* name;
        std::string bytes;
        Code code;
    };
    std::string bad_magic = WalFileHeaderBytes();
    bad_magic[0] = 'X';
    const Case cases[] = {
        {"empty file", "", Code::Corruption},
        {"short header", WalFileHeaderBytes().substr(0, 15), Code::Corruption},
        {"bad magic", bad_magic, Code::Corruption},
        {"version 2", WalFileHeaderBytes(2), Code::UnsupportedFormat},
        {"reserved", WalFileHeaderBytes(1, 1), Code::Corruption},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.name);
        fs::remove_all(Db());
        MakeWal(c.bytes);
        for (bool create : {false, true}) {
            std::unique_ptr<DB> db;
            const Status s = OpenDb(db, create);
            EXPECT_EQ(s.code, c.code) << s.ToString();
            EXPECT_TRUE(Contains(s.message, Wal().string())) << s.message;
            EXPECT_EQ(db, nullptr);
            EXPECT_EQ(ReadBytesOrEmpty(Wal()), c.bytes);
            EXPECT_FALSE(fs::exists(Temp()));
        }
    }
}

TEST_F(DBTest, NonRegularWalIsCorruptionWithoutBlocking) {
    struct Case {
        const char* name;
        int kind;  // 0 FIFO, 1 directory, 2 symlink to a valid WAL
    };
    for (const Case& c : {Case{"fifo", 0}, Case{"directory", 1},
                          Case{"symlink", 2}}) {
        SCOPED_TRACE(c.name);
        fs::remove_all(Db());
        fs::create_directory(Db());
        if (c.kind == 0) {
            ASSERT_EQ(::mkfifo(Wal().c_str(), 0600), 0);
        } else if (c.kind == 1) {
            fs::create_directory(Wal());
        } else {
            ASSERT_TRUE(
                WriteBytes(tmp_.path() / "real.wal", WalFileHeaderBytes()));
            fs::create_symlink(tmp_.path() / "real.wal", Wal());
        }
        for (bool create : {false, true}) {
            std::unique_ptr<DB> db;
            const Status s = OpenDb(db, create);
            EXPECT_EQ(s.code, Code::Corruption) << s.ToString();
            EXPECT_TRUE(Contains(s.message, "not a regular file"))
                << s.message;
            EXPECT_EQ(db, nullptr);
        }
        EXPECT_TRUE(fs::exists(fs::symlink_status(Wal())));
    }
}

TEST_F(DBTest, SecondOpenInSameProcessIsBusy) {
    auto first = MustOpen();
    ASSERT_TRUE(first->Put("k", "v").ok());

    std::unique_ptr<DB> second;
    const Status s = OpenDb(second);
    EXPECT_EQ(s.code, Code::Busy) << s.ToString();
    EXPECT_EQ(second, nullptr);
    ASSERT_TRUE(first->Put("k2", "v2").ok());  // first handle unaffected

    first.reset();
    EXPECT_TRUE(fs::exists(LockPath()));
    ASSERT_TRUE(OpenDb(second).ok());
    EXPECT_EQ(Lookup(*second, "k2"), "v2");
}

TEST_F(DBTest, FailedOpenReleasesResources) {
    std::string bad = WalFileHeaderBytes() + RawRecord(1, kRawPut, "k", "v");
    bad[16] ^= 0x01;  // header CRC mismatch
    MakeWal(bad);
    const std::size_t fds_before = OpenFdCount();

    std::unique_ptr<DB> db;
    EXPECT_EQ(OpenDb(db).code, Code::Corruption);
    EXPECT_EQ(db, nullptr);
    EXPECT_EQ(OpenFdCount(), fds_before);

    // The lock was released: it can be taken directly, then a valid WAL opens.
    {
        std::unique_ptr<FileLock> lock;
        ASSERT_TRUE(LockFile(LockPath(), lock).ok());
    }
    ASSERT_TRUE(WriteBytes(Wal(),
                           WalFileHeaderBytes() +
                               RawRecord(1, kRawPut, "k", "v")));
    ASSERT_TRUE(OpenDb(db).ok());
    EXPECT_EQ(Lookup(*db, "k"), "v");
}

}  // namespace
}  // namespace kv
