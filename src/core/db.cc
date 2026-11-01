// M1 stub: no persistence; replace when implementing WAL-backed DB operations.

#include "db.h"

namespace kv {

Status DB::Open(const std::filesystem::path&,
                const Options& options,
                std::unique_ptr<DB>& out) {
    if (out) {
        return Status::InvalidArgument("Open requires an empty output pointer");
    }

    if (options.memtable_bytes == 0) {
        return Status::InvalidArgument("memtable_bytes must be positive");
    }

    out.reset(new DB);
    return Status::Ok();
}

Status DB::Put(std::string_view, std::string_view) {
    return Status::Ok();
}

Status DB::Get(std::string_view, std::string& out_value) const {
    out_value.clear();
    return Status::NotFound();
}

Status DB::Delete(std::string_view) {
    return Status::Ok();
}

DB::~DB() = default;

}