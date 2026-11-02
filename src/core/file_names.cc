#include "file_names.h"

#include <cassert>
#include <limits>

namespace kv {

namespace {

constexpr std::size_t kMinIdDigits = 6;

std::string FormatId(std::uint64_t id) {
    assert(id != 0);
    std::string digits = std::to_string(id);
    if (digits.size() < kMinIdDigits) {
        digits.insert(0, kMinIdDigits - digits.size(), '0');
    }
    return digits;
}

// Parses a canonical ID; see ParseFileName.
bool ParseId(std::string_view digits, std::uint64_t& id) {
    if (digits.size() < kMinIdDigits) {
        return false;
    }
    if (digits.size() > kMinIdDigits && digits.front() == '0') {
        return false;
    }

    std::uint64_t value = 0;
    for (char c : digits) {
        if (c < '0' || c > '9') {
            return false;
        }
        const auto digit = static_cast<std::uint64_t>(c - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            return false;
        }
        value = value * 10 + digit;
    }
    if (value == 0) {
        return false;
    }
    id = value;
    return true;
}

bool ParseWithAffixes(std::string_view name,
                      std::string_view prefix,
                      std::string_view suffix,
                      std::uint64_t& id) {
    if (name.size() < prefix.size() + suffix.size() ||
        name.substr(0, prefix.size()) != prefix ||
        name.substr(name.size() - suffix.size()) != suffix) {
        return false;
    }
    return ParseId(name.substr(prefix.size(),
                               name.size() - prefix.size() - suffix.size()),
                   id);
}

}  // namespace

std::string WalFileName(std::uint64_t id) {
    return "wal-" + FormatId(id) + ".log";
}

std::string SstFileName(std::uint64_t id) {
    return "sst-" + FormatId(id) + ".sst";
}

std::string TempFileName(std::string_view name) {
    return std::string(name) + ".tmp";
}

bool ParseFileName(std::string_view name, FileKind& kind, std::uint64_t& id) {
    struct Form {
        std::string_view prefix;
        std::string_view suffix;
        FileKind kind;
    };
    static constexpr Form kForms[] = {
        {"wal-", ".log", FileKind::kWal},
        {"wal-", ".log.tmp", FileKind::kWalTemp},
        {"sst-", ".sst", FileKind::kSst},
        {"sst-", ".sst.tmp", FileKind::kSstTemp},
    };
    for (const Form& form : kForms) {
        std::uint64_t parsed = 0;
        if (ParseWithAffixes(name, form.prefix, form.suffix, parsed)) {
            kind = form.kind;
            id = parsed;
            return true;
        }
    }
    return false;
}

}  // namespace kv
