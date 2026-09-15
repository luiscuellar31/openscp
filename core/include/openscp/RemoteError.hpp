// Backend-independent failure metadata for retry policy and user messaging.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace openscp {

enum class RemoteErrorKind {
    None,
    Canceled,
    InvalidRequest,
    Unsupported,
    NotFound,
    Authentication,
    PermissionDenied,
    Certificate,
    Timeout,
    Connection,
    RateLimited,
    Conflict,
    Integrity,
    InsufficientSpace,
    LocalIo,
    RemoteIo,
    Protocol,
    Unknown,
};

struct RemoteError {
    RemoteErrorKind kind = RemoteErrorKind::None;
    std::string message;
    std::int64_t native_code = 0;
    std::optional<std::uint32_t> retry_after_seconds;
    bool transient = false;
    bool commit_uncertain = false;

    explicit operator bool() const noexcept {
        return kind != RemoteErrorKind::None;
    }
};

} // namespace openscp
