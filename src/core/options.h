#pragma once

#include <cstddef>

namespace kv {

struct Options {
    bool create_if_missing = true;
    std::size_t memtable_bytes = 4 * 1024 * 1024;
};

}  // namespace kv