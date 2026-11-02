#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include "options.h"
#include "status.h"

namespace kv {

class DBOpener;

class DB {
public:
    static Status Open(const std::filesystem::path& directory,
                       const Options& options,
                       std::unique_ptr<DB>& out);

    Status Put(std::string_view key, std::string_view value);
    Status Get(std::string_view key, std::string& out_value) const;
    Status Delete(std::string_view key);

    ~DB();

    DB(const DB&) = delete;
    DB& operator=(const DB&) = delete;

private:
    // Internal construction path (db_internal.h).
    friend class DBOpener;

    struct Impl;

    explicit DB(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

}
