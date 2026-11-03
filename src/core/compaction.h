#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include "sst.h"
#include "status.h"

namespace kv {

// Merges several validated SSTs into one stream in BytewiseLess key order,
// yielding for each key only the version with the greatest sequence
// (design.md section 9). Tombstones are yielded like any other winner; the
// caller decides whether to keep them. Two versions of one key with the
// same sequence are Corruption. Each input is read through its own
// SstIterator, so memory is one block per input.
class MergingIterator {
public:
    MergingIterator(const MergingIterator&) = delete;
    MergingIterator& operator=(const MergingIterator&) = delete;

    // Opens every table and positions at the first key. The tables must
    // outlive out, which must be empty; on failure it stays empty.
    static Status Open(const std::vector<const SstTable*>& tables,
                       std::unique_ptr<MergingIterator>& out);

    bool Valid() const {
        return winner_ < inputs_.size();
    }

    // Requires Valid(). The winning version of the current key; the views
    // stay valid until the next Next or Close.
    const SstEntry& entry() const {
        return inputs_[winner_]->entry();
    }

    // Requires Valid(). Advances past every version of the current key.
    // An input error or a same-sequence conflict is returned, and the
    // iterator stops.
    Status Next();

    // Closes every input; returns the first close error.
    Status Close();

private:
    MergingIterator() = default;

    // Positions winner_ at the greatest sequence among the inputs holding
    // the smallest current key.
    Status Settle();

    std::vector<std::unique_ptr<SstIterator>> inputs_;
    std::vector<std::size_t> at_key_;  // inputs positioned at the current key
    std::size_t winner_ = 0;
};

}  // namespace kv
