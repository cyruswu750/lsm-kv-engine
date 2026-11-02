#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>

#include "types.h"

namespace kv {

struct BytewiseLess {
    using is_transparent = void;

    bool operator()(std::string_view a,
                    std::string_view b) const;
};

struct Entry {
    std::uint64_t sequence;
    bool is_tombstone;
    std::string value;
};

enum class LookupResult {
    kFound,
    kDeleted,
    kAbsent
};

class Memtable {
public:
    using Table = std::map<std::string, Entry, BytewiseLess>;
    using const_iterator = Table::const_iterator;

    // Inputs must already be validated.
    // Calls must arrive in the intended version order.
    void Apply(std::uint64_t seq,
               ValueType type,
               std::string_view key,
               std::string_view value);

    // value must not be null.
    // Only kFound modifies *value.
    LookupResult Lookup(std::string_view key,
                        std::string* value) const;

    std::size_t ApproximateBytes() const;

    bool empty() const {
        return table_.empty();
    }

    // Every entry, tombstones included, in BytewiseLess key order. Each
    // key appears once, with its latest applied version. Iterators and
    // references stay valid until the next Apply or destruction.
    const_iterator begin() const {
        return table_.begin();
    }

    const_iterator end() const {
        return table_.end();
    }

private:
    Table table_;
    std::size_t approximate_bytes_ = 0;
};

}  