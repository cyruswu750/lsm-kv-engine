#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "status.h"

namespace kv {

inline constexpr std::size_t kMaxKeyLength = 64 * 1024;
inline constexpr std::size_t kMaxValueLength = 16 * 1024 * 1024;

enum class ValueType : std::uint8_t {
    kPut = 1,
    kDelete = 2
};

inline Status ValidateKeyValue(
    std::string_view key,
    std::string_view value
) {
    if (key.size() > kMaxKeyLength) {
        return Status::InvalidArgument(
            "key length " + std::to_string(key.size()) +
            " exceeds limit " + std::to_string(kMaxKeyLength)
        );
    }

    if (value.size() > kMaxValueLength) {
        return Status::InvalidArgument(
            "value length " + std::to_string(value.size()) +
            " exceeds limit " + std::to_string(kMaxValueLength)
        );
    }

    return Status::Ok();
}

}  // namespace kv