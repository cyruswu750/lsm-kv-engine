#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/compaction.h"
#include "core/posix_file.h"
#include "core/sst.h"
#include "testing/fault_syscalls.h"
#include "testing/sst_builder.h"
#include "testing/temp_dir.h"

namespace kv {
namespace {

namespace fs = std::filesystem;

using test::kRawDelete;
using test::kRawPut;
using test::RawSst;
using test::RawSstEntry;
using test::WriteBytes;

bool Contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

struct Seen {
    std::string key;
    std::uint64_t sequence;
    ValueType type;
    std::string value;

    bool operator==(const Seen&) const = default;
};

std::ostream& operator<<(std::ostream& os, const Seen& s) {
    return os << "{" << s.key << "," << s.sequence << ","
              << static_cast<int>(s.type) << "," << s.value << "}";
}

Seen Copy(const SstEntry& e) {
    return {std::string(e.key), e.sequence, e.type, std::string(e.value)};
}

class CompactionInputTest : public ::testing::Test {
protected:
    // Writes bytes as an SST file and opens it with full validation.
    std::unique_ptr<SstTable> Table(const std::string& name,
                                    const std::string& bytes,
                                    PosixSyscalls& sys = DefaultPosixSyscalls()) {
        const fs::path path = tmp_.path() / name;
        EXPECT_TRUE(WriteBytes(path, bytes));
        std::unique_ptr<SstTable> table;
        const Status s = SstTable::Open(path, bytes.size(), sys, table);
        EXPECT_TRUE(s.ok()) << s.ToString();
        return table;
    }

    test::TempDir tmp_;
};

std::vector<Seen> Drain(SstIterator& it) {
    std::vector<Seen> out;
    while (it.Valid()) {
        out.push_back(Copy(it.entry()));
        const Status s = it.Next();
        EXPECT_TRUE(s.ok()) << s.ToString();
    }
    return out;
}

std::vector<Seen> Drain(MergingIterator& it) {
    std::vector<Seen> out;
    while (it.Valid()) {
        out.push_back(Copy(it.entry()));
        const Status s = it.Next();
        EXPECT_TRUE(s.ok()) << s.ToString();
    }
    return out;
}

// ---- SstIterator ----------------------------------------------------------------

TEST_F(CompactionInputTest, IteratorStreamsEveryEntryAcrossBlocks) {
    const std::string big(3000, 'v');
    const std::string bytes = RawSst({
        {{std::string("\x00", 1), 1, kRawPut, "z"}, {"a", 9, kRawDelete, ""}},
        {{"b", 2, kRawPut, big}},
        {{"c", 5, kRawPut, ""}, {std::string("\xff", 1), 3, kRawPut, "x"}},
    });
    auto table = Table("t.sst", bytes);
    ASSERT_NE(table, nullptr);
    std::unique_ptr<SstIterator> it;
    ASSERT_TRUE(SstIterator::Open(*table, it).ok());
    const std::vector<Seen> want = {
        {std::string("\x00", 1), 1, ValueType::kPut, "z"},
        {"a", 9, ValueType::kDelete, ""},
        {"b", 2, ValueType::kPut, big},
        {"c", 5, ValueType::kPut, ""},
        {std::string("\xff", 1), 3, ValueType::kPut, "x"},
    };
    EXPECT_EQ(Drain(*it), want);
    EXPECT_FALSE(it->Valid());
    EXPECT_EQ(it->Next().code, Code::InvalidArgument);  // not positioned
    EXPECT_TRUE(it->Close().ok());
}

TEST_F(CompactionInputTest, IteratorRevalidatesEachBlockAndStopsOnError) {
    const std::string b1 = test::RawBlock({{"a", 1, kRawPut, "1"}});
    const std::string bytes = RawSst({{{"a", 1, kRawPut, "1"}},
                                      {{"b", 2, kRawPut, "2"}}});
    auto table = Table("t.sst", bytes);
    ASSERT_NE(table, nullptr);

    // Corrupt the second block after Open validated the file.
    std::string corrupt = bytes;
    corrupt[b1.size() + 17] ^= 0x01;  // the key byte of "b"
    ASSERT_TRUE(WriteBytes(tmp_.path() / "t.sst", corrupt));
    std::unique_ptr<SstIterator> it;
    ASSERT_TRUE(SstIterator::Open(*table, it).ok());
    ASSERT_TRUE(it->Valid());
    EXPECT_EQ(it->entry().key, "a");
    const Status s = it->Next();
    EXPECT_EQ(s.code, Code::Corruption) << s.ToString();
    EXPECT_TRUE(Contains(s.message, "block checksum mismatch")) << s.message;
    EXPECT_FALSE(it->Valid());

    // A file whose size changed since Open is rejected before reading.
    ASSERT_TRUE(WriteBytes(tmp_.path() / "t.sst", bytes + "x"));
    std::unique_ptr<SstIterator> other;
    EXPECT_EQ(SstIterator::Open(*table, other).code, Code::Corruption);
    EXPECT_EQ(other, nullptr);
}

TEST_F(CompactionInputTest, IteratorReadsThroughTheSeamAndReportsErrors) {
    const std::string bytes = RawSst({{{"a", 1, kRawPut, "1"}},
                                      {{"b", 2, kRawPut, "2"}}});
    test::FaultSyscalls sys;
    auto table = Table("t.sst", bytes, sys);
    ASSERT_NE(table, nullptr);
    sys.armed = true;
    std::unique_ptr<SstIterator> it;
    ASSERT_TRUE(SstIterator::Open(*table, it).ok());
    EXPECT_EQ(sys.opens, 1u);
    sys.FailOn("pread", "t.sst", EIO);
    const Status s = it->Next();  // reads the second block
    EXPECT_EQ(s.code, Code::IOError) << s.ToString();
    EXPECT_EQ(sys.rules[0].fired, 1u);
    EXPECT_FALSE(it->Valid());
    it.reset();
    EXPECT_EQ(sys.closes, 1u);  // the descriptor is released

    sys.FailOn("open", "t.sst", EMFILE);
    EXPECT_EQ(SstIterator::Open(*table, it).code, Code::IOError);
    EXPECT_EQ(it, nullptr);
}

// ---- MergingIterator ------------------------------------------------------------

TEST_F(CompactionInputTest, MergeKeepsTheGreatestSequencePerKey) {
    // Oldest first, with disjoint sequence intervals as in a manifest.
    auto t1 = Table("1.sst", RawSst({{{"a", 1, kRawPut, "a1"},
                                      {"b", 2, kRawPut, "b1"},
                                      {"d", 3, kRawPut, "d1"}}}));
    auto t2 = Table("2.sst", RawSst({{{"a", 4, kRawPut, "a2"},
                                      {"c", 5, kRawPut, "c2"}}}));
    auto t3 = Table("3.sst", RawSst({{{"a", 7, kRawDelete, ""},
                                      {"b", 6, kRawPut, "b3"},
                                      {"e", 8, kRawDelete, ""}}}));
    std::unique_ptr<MergingIterator> merge;
    ASSERT_TRUE(
        MergingIterator::Open({t1.get(), t2.get(), t3.get()}, merge).ok());
    const std::vector<Seen> want = {
        {"a", 7, ValueType::kDelete, ""},  // tombstones are yielded
        {"b", 6, ValueType::kPut, "b3"},
        {"c", 5, ValueType::kPut, "c2"},
        {"d", 3, ValueType::kPut, "d1"},
        {"e", 8, ValueType::kDelete, ""},
    };
    EXPECT_EQ(Drain(*merge), want);
    EXPECT_TRUE(merge->Close().ok());

    // Input order does not decide the winner; the sequence does.
    merge.reset();
    ASSERT_TRUE(
        MergingIterator::Open({t3.get(), t1.get(), t2.get()}, merge).ok());
    EXPECT_EQ(Drain(*merge), want);
}

TEST_F(CompactionInputTest, MergeOrdersBinaryKeysByUnsignedBytes) {
    const std::string k0("\x00", 1), k1("\x00\x00", 2), k2("\x7f", 1),
        k3("\x80", 1), k4("\xff\x00", 2);
    auto t1 = Table("1.sst", RawSst({{{k0, 1, kRawPut, "0"},
                                      {k3, 2, kRawPut, "3"}}}));
    auto t2 = Table("2.sst", RawSst({{{k1, 3, kRawPut, "1"},
                                      {k2, 4, kRawPut, "2"},
                                      {k4, 5, kRawPut, "4"}}}));
    std::unique_ptr<MergingIterator> merge;
    ASSERT_TRUE(MergingIterator::Open({t1.get(), t2.get()}, merge).ok());
    std::vector<std::string> keys;
    for (const Seen& s : Drain(*merge)) {
        keys.push_back(s.key);
    }
    EXPECT_EQ(keys, (std::vector<std::string>{k0, k1, k2, k3, k4}));
}

TEST_F(CompactionInputTest, MergeRejectsTwoVersionsWithOneSequence) {
    auto t1 = Table("1.sst", RawSst({{{"a", 1, kRawPut, "x"},
                                      {"k", 5, kRawPut, "old"}}}));
    auto t2 = Table("2.sst", RawSst({{{"k", 5, kRawPut, "new"}}}));
    std::unique_ptr<MergingIterator> merge;
    ASSERT_TRUE(MergingIterator::Open({t1.get(), t2.get()}, merge).ok());
    ASSERT_TRUE(merge->Valid());
    EXPECT_EQ(merge->entry().key, "a");
    const Status s = merge->Next();
    EXPECT_EQ(s.code, Code::Corruption) << s.ToString();
    EXPECT_TRUE(Contains(s.message, "sequence 5")) << s.message;
    EXPECT_FALSE(merge->Valid());

    // Detected while positioning at the first key as well.
    std::unique_ptr<MergingIterator> first;
    auto t3 = Table("3.sst", RawSst({{{"k", 5, kRawDelete, ""}}}));
    EXPECT_EQ(MergingIterator::Open({t2.get(), t3.get()}, first).code,
              Code::Corruption);
    EXPECT_EQ(first, nullptr);
}

// Two versions of one key with the same sequence are Corruption even when
// a newer version of the key shadows both, and whatever the input order.
TEST_F(CompactionInputTest, MergeRejectsDuplicateSequencesBehindANewerVersion) {
    auto newer = Table("newer.sst", RawSst({{{"k", 10, kRawPut, "ten"}}}));
    auto first = Table("first.sst", RawSst({{{"k", 5, kRawPut, "five-a"}}}));
    auto second = Table("second.sst", RawSst({{{"k", 5, kRawPut, "five-b"}}}));
    std::vector<const SstTable*> inputs = {newer.get(), first.get(),
                                           second.get()};
    std::sort(inputs.begin(), inputs.end());
    std::size_t orders = 0;
    do {
        std::string order;
        for (const SstTable* t : inputs) {
            order += t == newer.get() ? "10 " : "5 ";
        }
        SCOPED_TRACE("sequences in input order: " + order);
        std::unique_ptr<MergingIterator> merge;
        const Status s = MergingIterator::Open(inputs, merge);
        EXPECT_EQ(s.code, Code::Corruption) << s.ToString();
        EXPECT_TRUE(Contains(s.message, "sequence 5")) << s.message;
        EXPECT_EQ(merge, nullptr);
        ++orders;
    } while (std::next_permutation(inputs.begin(), inputs.end()));
    EXPECT_EQ(orders, 6u);  // includes [10, 5, 5] and [5, 10, 5]

    // The same conflict found after the first key, from Next.
    auto a_newer = Table("a_newer.sst", RawSst({{{"a", 1, kRawPut, "a"},
                                                 {"k", 10, kRawPut, "ten"}}}));
    std::unique_ptr<MergingIterator> merge;
    ASSERT_TRUE(MergingIterator::Open({a_newer.get(), first.get(),
                                       second.get()},
                                      merge)
                    .ok());
    EXPECT_EQ(merge->entry().key, "a");
    const Status s = merge->Next();
    EXPECT_EQ(s.code, Code::Corruption) << s.ToString();
    EXPECT_FALSE(merge->Valid());
}

TEST_F(CompactionInputTest, MergeOfNoInputsIsEmpty) {
    std::unique_ptr<MergingIterator> merge;
    ASSERT_TRUE(MergingIterator::Open({}, merge).ok());
    EXPECT_FALSE(merge->Valid());
    EXPECT_TRUE(merge->Close().ok());
}

}  // namespace
}  // namespace kv
