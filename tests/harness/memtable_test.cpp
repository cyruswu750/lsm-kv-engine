#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "core/memtable.h"
#include "core/types.h"

namespace kv {
namespace {

// Builds a byte string with an explicit length so embedded zero bytes are kept.
std::string Bytes(const char* data, std::size_t n) {
    return std::string(data, n);
}

// ---- BytewiseLess -----------------------------------------------------------

TEST(BytewiseLess, ComparesBytesAsUnsigned) {
    const BytewiseLess less;
    const std::string lo(1, '\x7f');
    const std::string hi(1, '\x80');

    // With signed char, 0x80 would sort before 0x7f.
    EXPECT_TRUE(less(lo, hi));
    EXPECT_FALSE(less(hi, lo));

    const std::string zero(1, '\x00');
    const std::string ff(1, '\xff');
    EXPECT_TRUE(less(zero, ff));
    EXPECT_FALSE(less(ff, zero));
}

TEST(BytewiseLess, PrefixSortsFirst) {
    const BytewiseLess less;

    EXPECT_TRUE(less("", "a"));
    EXPECT_FALSE(less("a", ""));
    EXPECT_TRUE(less("ab", "abc"));
    EXPECT_FALSE(less("abc", "ab"));

    // A trailing zero byte still makes the longer key greater.
    const std::string a = "a";
    const std::string a0 = Bytes("a\0", 2);
    EXPECT_TRUE(less(a, a0));
    EXPECT_FALSE(less(a0, a));
    EXPECT_TRUE(less("", Bytes("\0", 1)));
}

TEST(BytewiseLess, EqualKeysAreNotLess) {
    const BytewiseLess less;

    EXPECT_FALSE(less("", ""));
    EXPECT_FALSE(less("abc", "abc"));
    const std::string k = Bytes("a\0b", 3);
    EXPECT_FALSE(less(k, k));
}

TEST(BytewiseLess, FirstDifferingByteDecidesBeforeLength) {
    const BytewiseLess less;

    EXPECT_TRUE(less("abc", "abd"));
    EXPECT_TRUE(less("ab\x7f" "zzz", "ab\x80"));
    EXPECT_FALSE(less("b", "abc"));
}

// ---- Memtable lookup semantics ---------------------------------------------

TEST(Memtable, LookupOnEmptyTableIsAbsent) {
    Memtable mem;
    std::string out;
    EXPECT_EQ(mem.Lookup("missing", &out), LookupResult::kAbsent);
}

TEST(Memtable, PutThenLookupReturnsValue) {
    Memtable mem;
    mem.Apply(1, ValueType::kPut, "key", "value");

    std::string out;
    ASSERT_EQ(mem.Lookup("key", &out), LookupResult::kFound);
    EXPECT_EQ(out, "value");
    EXPECT_EQ(mem.Lookup("other", &out), LookupResult::kAbsent);
}

TEST(Memtable, OverwriteReturnsLatestValue) {
    Memtable mem;
    mem.Apply(1, ValueType::kPut, "key", "first");
    mem.Apply(2, ValueType::kPut, "key", "second-longer");
    mem.Apply(3, ValueType::kPut, "key", "3rd");

    std::string out;
    ASSERT_EQ(mem.Lookup("key", &out), LookupResult::kFound);
    EXPECT_EQ(out, "3rd");
}

TEST(Memtable, DeleteAfterPutIsDeleted) {
    Memtable mem;
    mem.Apply(1, ValueType::kPut, "key", "value");
    mem.Apply(2, ValueType::kDelete, "key", "");

    std::string out;
    EXPECT_EQ(mem.Lookup("key", &out), LookupResult::kDeleted);
}

TEST(Memtable, PutAfterDeleteIsFoundAgain) {
    Memtable mem;
    mem.Apply(1, ValueType::kPut, "key", "old");
    mem.Apply(2, ValueType::kDelete, "key", "");
    mem.Apply(3, ValueType::kPut, "key", "new");

    std::string out;
    ASSERT_EQ(mem.Lookup("key", &out), LookupResult::kFound);
    EXPECT_EQ(out, "new");
}

TEST(Memtable, DeleteOfNeverPresentKeyCreatesTombstone) {
    Memtable mem;
    mem.Apply(1, ValueType::kDelete, "ghost", "");

    std::string out;
    EXPECT_EQ(mem.Lookup("ghost", &out), LookupResult::kDeleted);
    EXPECT_EQ(mem.Lookup("other", &out), LookupResult::kAbsent);
}

TEST(Memtable, EmptyKeyIsAValidKey) {
    Memtable mem;
    mem.Apply(1, ValueType::kPut, "", "for-empty-key");

    std::string out;
    ASSERT_EQ(mem.Lookup("", &out), LookupResult::kFound);
    EXPECT_EQ(out, "for-empty-key");

    // The empty key and a single zero byte are different keys.
    EXPECT_EQ(mem.Lookup(Bytes("\0", 1), &out), LookupResult::kAbsent);
}

TEST(Memtable, EmptyValueIsDistinctFromDeletion) {
    Memtable mem;
    mem.Apply(1, ValueType::kPut, "empty", "");
    mem.Apply(2, ValueType::kDelete, "gone", "");

    std::string out = "sentinel";
    ASSERT_EQ(mem.Lookup("empty", &out), LookupResult::kFound);
    EXPECT_EQ(out, "");
    EXPECT_EQ(mem.Lookup("gone", &out), LookupResult::kDeleted);
}

TEST(Memtable, EmptyKeyWithEmptyValue) {
    Memtable mem;
    mem.Apply(1, ValueType::kPut, "", "");

    std::string out = "sentinel";
    ASSERT_EQ(mem.Lookup("", &out), LookupResult::kFound);
    EXPECT_EQ(out, "");

    mem.Apply(2, ValueType::kDelete, "", "");
    EXPECT_EQ(mem.Lookup("", &out), LookupResult::kDeleted);
}

TEST(Memtable, EmbeddedZeroBytesInKeysAndValues) {
    const std::string k1 = Bytes("a\0b", 3);
    const std::string k2 = Bytes("a\0c", 3);
    const std::string k_prefix = Bytes("a", 1);
    const std::string v1 = Bytes("\0x\0", 3);
    const std::string v2 = Bytes("\0\0\0\0", 4);
    ASSERT_EQ(k1.size(), 3u);
    ASSERT_EQ(v2.size(), 4u);

    Memtable mem;
    mem.Apply(1, ValueType::kPut, k1, v1);
    mem.Apply(2, ValueType::kPut, k2, v2);

    std::string out;
    ASSERT_EQ(mem.Lookup(k1, &out), LookupResult::kFound);
    EXPECT_EQ(out, v1);
    EXPECT_EQ(out.size(), 3u);

    ASSERT_EQ(mem.Lookup(k2, &out), LookupResult::kFound);
    EXPECT_EQ(out, v2);
    EXPECT_EQ(out.size(), 4u);

    // Keys must not be truncated at the first zero byte.
    EXPECT_EQ(mem.Lookup(k_prefix, &out), LookupResult::kAbsent);
}

TEST(Memtable, HighBitKeysAreDistinct) {
    Memtable mem;
    mem.Apply(1, ValueType::kPut, std::string(1, '\x7f'), "lo");
    mem.Apply(2, ValueType::kPut, std::string(1, '\x80'), "hi");

    std::string out;
    ASSERT_EQ(mem.Lookup(std::string(1, '\x7f'), &out), LookupResult::kFound);
    EXPECT_EQ(out, "lo");
    ASSERT_EQ(mem.Lookup(std::string(1, '\x80'), &out), LookupResult::kFound);
    EXPECT_EQ(out, "hi");
}

// ---- Ownership of caller buffers --------------------------------------------

TEST(Memtable, MutatingCallerBuffersAfterApplyDoesNotChangeEntry) {
    std::string key = "key";
    std::string value = "value";

    Memtable mem;
    mem.Apply(1, ValueType::kPut, key, value);

    key[0] = 'X';
    value.assign("clobbered-and-longer-than-before");

    std::string out;
    ASSERT_EQ(mem.Lookup("key", &out), LookupResult::kFound);
    EXPECT_EQ(out, "value");
    EXPECT_EQ(mem.Lookup("Xey", &out), LookupResult::kAbsent);
}

TEST(Memtable, DestroyingCallerBuffersAfterApplyDoesNotChangeEntry) {
    Memtable mem;
    {
        // Heap buffers that are freed before lookup; ASan reports any retained view.
        auto key = std::make_unique<char[]>(4);
        auto value = std::make_unique<char[]>(5);
        key[0] = 'k'; key[1] = '\0'; key[2] = 'e'; key[3] = 'y';
        value[0] = 'v'; value[1] = 'a'; value[2] = '\0'; value[3] = 'u'; value[4] = 'e';
        mem.Apply(1, ValueType::kPut,
                  std::string_view(key.get(), 4),
                  std::string_view(value.get(), 5));
    }

    std::string out;
    ASSERT_EQ(mem.Lookup(Bytes("k\0ey", 4), &out), LookupResult::kFound);
    EXPECT_EQ(out, Bytes("va\0ue", 5));
}

TEST(Memtable, LookupKeyBufferIsNotRetained) {
    Memtable mem;
    mem.Apply(1, ValueType::kPut, "key", "value");

    std::string probe = "key";
    std::string out;
    ASSERT_EQ(mem.Lookup(probe, &out), LookupResult::kFound);

    probe[0] = 'X';
    ASSERT_EQ(mem.Lookup("key", &out), LookupResult::kFound);
    EXPECT_EQ(out, "value");
}

// ---- Lookup output contract -------------------------------------------------
// Memtable::Lookup modifies *value only on kFound; it does not clear it
// otherwise (clearing is DB::Get's responsibility).

TEST(Memtable, LookupAbsentLeavesOutputUntouched) {
    Memtable mem;
    mem.Apply(1, ValueType::kPut, "present", "v");

    std::string out = "sentinel";
    ASSERT_EQ(mem.Lookup("absent", &out), LookupResult::kAbsent);
    EXPECT_EQ(out, "sentinel");
}

TEST(Memtable, LookupDeletedLeavesOutputUntouched) {
    Memtable mem;
    mem.Apply(1, ValueType::kPut, "key", "old-value");
    mem.Apply(2, ValueType::kDelete, "key", "");

    std::string out = "sentinel";
    ASSERT_EQ(mem.Lookup("key", &out), LookupResult::kDeleted);
    EXPECT_EQ(out, "sentinel");
}

TEST(Memtable, LookupFoundReplacesPriorOutputContents) {
    Memtable mem;
    mem.Apply(1, ValueType::kPut, "key", "ab");

    std::string out = "much-longer-previous-contents";
    ASSERT_EQ(mem.Lookup("key", &out), LookupResult::kFound);
    EXPECT_EQ(out, "ab");
}

// ---- ApproximateBytes accounting --------------------------------------------
// The per-entry overhead is an implementation constant, so it is derived
// from a first insert rather than hard-coded. Value-size changes on an
// existing key must be reflected exactly; tombstones carry no value bytes.

class MemtableBytes : public ::testing::Test {
protected:
    // Fixed per-entry overhead observed when inserting a fresh key.
    std::size_t Overhead() {
        Memtable probe;
        probe.Apply(1, ValueType::kPut, "k", "vv");
        const std::size_t total = probe.ApproximateBytes();
        EXPECT_GE(total, 3u);
        return total - 3u;
    }
};

TEST_F(MemtableBytes, EmptyTableIsZero) {
    Memtable mem;
    EXPECT_EQ(mem.ApproximateBytes(), 0u);
}

TEST_F(MemtableBytes, InsertAddsKeyValueAndConstantOverhead) {
    const std::size_t overhead = Overhead();

    Memtable mem;
    mem.Apply(1, ValueType::kPut, "alpha", "12345678");
    EXPECT_EQ(mem.ApproximateBytes(), 5u + 8u + overhead);

    mem.Apply(2, ValueType::kPut, "", "");
    EXPECT_EQ(mem.ApproximateBytes(), 5u + 8u + 2 * overhead);

    mem.Apply(3, ValueType::kPut, "b", std::string(1000, 'x'));
    EXPECT_EQ(mem.ApproximateBytes(), 5u + 8u + 1u + 1000u + 3 * overhead);
}

TEST_F(MemtableBytes, OverwriteAdjustsByValueSizeDifference) {
    Memtable mem;
    mem.Apply(1, ValueType::kPut, "key", "1234567890");
    const std::size_t base = mem.ApproximateBytes();

    mem.Apply(2, ValueType::kPut, "key", "123");  // shorter
    EXPECT_EQ(mem.ApproximateBytes(), base - 7);

    mem.Apply(3, ValueType::kPut, "key", std::string(50, 'z'));  // longer
    EXPECT_EQ(mem.ApproximateBytes(), base + 40);

    mem.Apply(4, ValueType::kPut, "key", std::string(50, 'y'));  // same size
    EXPECT_EQ(mem.ApproximateBytes(), base + 40);
}

TEST_F(MemtableBytes, PutToTombstoneDropsValueBytes) {
    Memtable mem;
    mem.Apply(1, ValueType::kPut, "key", "123456");
    const std::size_t with_value = mem.ApproximateBytes();

    mem.Apply(2, ValueType::kDelete, "key", "");
    EXPECT_EQ(mem.ApproximateBytes(), with_value - 6);
}

TEST_F(MemtableBytes, TombstoneToPutAddsValueBytes) {
    Memtable mem;
    mem.Apply(1, ValueType::kDelete, "key", "");
    const std::size_t tombstone = mem.ApproximateBytes();

    mem.Apply(2, ValueType::kPut, "key", "123456");
    EXPECT_EQ(mem.ApproximateBytes(), tombstone + 6);
}

TEST_F(MemtableBytes, FreshTombstoneCountsKeyAndOverhead) {
    const std::size_t overhead = Overhead();

    Memtable mem;
    mem.Apply(1, ValueType::kDelete, "ghost", "");
    EXPECT_EQ(mem.ApproximateBytes(), 5u + overhead);

    // Repeated deletes of the same key do not grow the estimate.
    mem.Apply(2, ValueType::kDelete, "ghost", "");
    EXPECT_EQ(mem.ApproximateBytes(), 5u + overhead);
}

TEST_F(MemtableBytes, ReturnsToBaselineAfterRoundTrip) {
    Memtable mem;
    mem.Apply(1, ValueType::kPut, "key", "v");
    const std::size_t base = mem.ApproximateBytes();

    mem.Apply(2, ValueType::kPut, "key", std::string(200, 'a'));
    mem.Apply(3, ValueType::kDelete, "key", "");
    mem.Apply(4, ValueType::kPut, "key", "v");
    EXPECT_EQ(mem.ApproximateBytes(), base);
}

}  // namespace
}  // namespace kv
