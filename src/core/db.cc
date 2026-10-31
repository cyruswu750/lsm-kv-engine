// M1 stub: no persistence; replace when implementing WAL-backed DB operations.

#include "db.h"

namespace kv {

Status DB::Open(const std::string&,
                const Options&,
                std::unique_ptr<DB>* out) {
    out->reset(new DB);
    return Status::Ok();
}

Status DB::Put(std::string_view, std::string_view) {
    return Status::Ok();
}

Status DB::Get(std::string_view, std::string* out_value) {
    out_value->clear();
    return Status::NotFound();
}

Status DB::Delete(std::string_view) {
    return Status::Ok();
}

}