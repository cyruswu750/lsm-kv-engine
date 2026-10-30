#include "memtable.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <utility>

namespace kv {

namespace {

constexpr std::size_t kEntryOverhead = 32;

}

bool BytewiseLess::operator()(std::string_view a,
                              std::string_view b) const {
    const std::size_t n = std::min(a.size(), b.size());

    if (n > 0) {
        const int result = std::memcmp(a.data(), b.data(), n);

        if (result != 0) {
            return result < 0;
        }
    }

    return a.size() < b.size();
}

void Memtable::Apply(std::uint64_t seq,
                    ValueType type,
                    std::string_view key,
                    std::string_view value) {
    assert(type == ValueType::kPut ||
           type == ValueType::kDelete);

    const bool deleted = (type == ValueType::kDelete);

    Entry entry{
        seq,
        deleted,
        deleted ? std::string{} : std::string(value)
    };

    const std::size_t new_size = entry.value.size();
    auto it = table_.find(key);

    if (it == table_.end()) {
        const std::size_t added =
            key.size() + new_size + kEntryOverhead;

        table_.emplace(std::string(key), std::move(entry));
        approximate_bytes_ += added;
    } else {
        const std::size_t old_size = it->second.value.size();

        it->second = std::move(entry);

        approximate_bytes_ -= old_size;
        approximate_bytes_ += new_size;
    }
}

LookupResult Memtable::Lookup(std::string_view key,
                              std::string* value) const {
    assert(value != nullptr);

    const auto it = table_.find(key);

    if (it == table_.end()) {
        return LookupResult::kAbsent;
    }

    if (it->second.is_tombstone) {
        return LookupResult::kDeleted;
    }

    *value = it->second.value;
    return LookupResult::kFound;
}

std::size_t Memtable::ApproximateBytes() const {
    return approximate_bytes_;
}

} 