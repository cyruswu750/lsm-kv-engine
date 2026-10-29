#pragma once

#include <string>
#include <utility>

namespace kv {

enum class Code {
    Ok,
    NotFound,
    InvalidArgument,
    IOError,
    Corruption,
    UnsupportedFormat,
    Busy,
    NeedsRecovery
};

struct Status {
    Code code = Code::Ok;
    std::string message;

    bool ok() const {
        return code == Code::Ok;
    }

    static Status Ok() {
        return {Code::Ok, ""};
    }

    static Status NotFound(std::string msg = "") {
        return {Code::NotFound, std::move(msg)};
    }

    static Status InvalidArgument(std::string msg) {
        return {Code::InvalidArgument, std::move(msg)};
    }

    static Status IOError(std::string msg) {
        return {Code::IOError, std::move(msg)};
    }

    static Status Corruption(std::string msg) {
        return {Code::Corruption, std::move(msg)};
    }

    static Status UnsupportedFormat(std::string msg) {
        return {Code::UnsupportedFormat, std::move(msg)};
    }

    static Status Busy(std::string msg) {
        return {Code::Busy, std::move(msg)};
    }

    static Status NeedsRecovery(std::string msg) {
        return {Code::NeedsRecovery, std::move(msg)};
    }

    std::string ToString() const {
        const char* name = "Unknown";

        switch (code) {
            case Code::Ok:
                name = "Ok";
                break;
            case Code::NotFound:
                name = "NotFound";
                break;
            case Code::InvalidArgument:
                name = "InvalidArgument";
                break;
            case Code::IOError:
                name = "IOError";
                break;
            case Code::Corruption:
                name = "Corruption";
                break;
            case Code::UnsupportedFormat:
                name = "UnsupportedFormat";
                break;
            case Code::Busy:
                name = "Busy";
                break;
            case Code::NeedsRecovery:
                name = "NeedsRecovery";
                break;
        }

        if (message.empty()) {
            return name;
        }

        return std::string(name) + ": " + message;
    }
};

}  // namespace kv