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

private:
    std::map<std::string, Entry, BytewiseLess> table_;
    std::size_t approximate_bytes_ = 0;
};

}  