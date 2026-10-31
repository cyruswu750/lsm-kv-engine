#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "options.h"
#include "status.h"

namespace kv {

class DB {
public:
    static Status Open(const std::string& path,
                       const Options& options,
                       std::unique_ptr<DB>* out);

    Status Put(std::string_view key, std::string_view value);
    Status Get(std::string_view key, std::string* out_value);
    Status Delete(std::string_view key);

private:
    DB() = default;
};

}