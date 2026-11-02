#pragma once

#include <string_view>

#include "status.h"

namespace kv {

// Internal sequential-write boundary. DB code will hold files through this
// interface so tests can substitute fault-injecting implementations later.
//
// None of these calls is a rollback guarantee: after an Append or Sync
// error, the file may contain any prefix of what was appended, and the
// caller must not treat it as usable for further appends. Destruction
// releases resources but never syncs; durability comes only from Sync.
class WritableFile {
public:
    WritableFile() = default;
    virtual ~WritableFile() = default;

    WritableFile(const WritableFile&) = delete;
    WritableFile& operator=(const WritableFile&) = delete;

    // Appends all of data or returns IOError. Empty data succeeds.
    virtual Status Append(std::string_view data) = 0;

    // Flushes appended data to stable storage. A failed Sync must not be
    // retried: a later success would not prove the earlier data persisted.
    virtual Status Sync() = 0;

    // Releases the file. Repeated Close returns Ok without effect. After
    // Close, Append and Sync return IOError.
    virtual Status Close() = 0;
};

}  // namespace kv
