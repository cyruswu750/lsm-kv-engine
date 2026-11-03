#include "compaction.h"

#include <string>
#include <string_view>
#include <utility>

#include "memtable.h"

namespace kv {

Status MergingIterator::Open(const std::vector<const SstTable*>& tables,
                             std::unique_ptr<MergingIterator>& out) {
    if (out) {
        return Status::InvalidArgument("merge output is not empty");
    }
    std::unique_ptr<MergingIterator> merge(new MergingIterator());
    merge->inputs_.reserve(tables.size());
    merge->at_key_.reserve(tables.size());
    for (const SstTable* table : tables) {
        std::unique_ptr<SstIterator> it;
        Status s = SstIterator::Open(*table, it);
        if (!s.ok()) {
            return s;
        }
        merge->inputs_.push_back(std::move(it));
    }
    Status s = merge->Settle();
    if (!s.ok()) {
        return s;
    }
    out = std::move(merge);
    return Status::Ok();
}

Status MergingIterator::Settle() {
    winner_ = inputs_.size();
    at_key_.clear();
    for (std::size_t i = 0; i < inputs_.size(); ++i) {
        if (!inputs_[i]->Valid()) {
            continue;
        }
        const std::string_view key = inputs_[i]->entry().key;
        if (!at_key_.empty()) {
            const std::string_view current = inputs_[at_key_[0]]->entry().key;
            if (BytewiseLess{}(current, key)) {
                continue;
            }
            if (BytewiseLess{}(key, current)) {
                at_key_.clear();
            }
        }
        at_key_.push_back(i);
    }
    if (at_key_.empty()) {
        return Status::Ok();
    }

    // Every pair is compared, not only each version against the current
    // winner: a newer version must not hide two conflicting older ones.
    for (std::size_t a = 0; a < at_key_.size(); ++a) {
        for (std::size_t b = a + 1; b < at_key_.size(); ++b) {
            const std::uint64_t seq = inputs_[at_key_[a]]->entry().sequence;
            if (seq == inputs_[at_key_[b]]->entry().sequence) {
                return Status::Corruption(
                    "two versions of one key have sequence " +
                    std::to_string(seq) + " (inputs " +
                    std::to_string(at_key_[a]) + " and " +
                    std::to_string(at_key_[b]) + ")");
            }
        }
    }

    std::size_t best = at_key_[0];
    for (const std::size_t i : at_key_) {
        if (inputs_[i]->entry().sequence > inputs_[best]->entry().sequence) {
            best = i;
        }
    }
    winner_ = best;
    return Status::Ok();
}

Status MergingIterator::Next() {
    if (!Valid()) {
        return Status::InvalidArgument("merge is not positioned");
    }
    for (const std::size_t i : at_key_) {
        Status s = inputs_[i]->Next();
        if (!s.ok()) {
            winner_ = inputs_.size();
            at_key_.clear();
            return s;
        }
    }
    return Settle();
}

Status MergingIterator::Close() {
    winner_ = inputs_.size();
    at_key_.clear();
    Status first = Status::Ok();
    for (auto& input : inputs_) {
        Status s = input->Close();
        if (first.ok() && !s.ok()) {
            first = s;
        }
    }
    return first;
}

}  // namespace kv
