#pragma once

#include <utility>

struct _LIBSSH2_SFTP_HANDLE;
using LIBSSH2_SFTP_HANDLE = struct _LIBSSH2_SFTP_HANDLE;

namespace openscp::libssh2detail {

/// Default closer that invokes libssh2_sftp_close_handle.
int defaultCloseSftpHandle(LIBSSH2_SFTP_HANDLE *handle) noexcept;

/// RAII wrapper for LIBSSH2_SFTP_HANDLE*.
/// Automatically closes the remote SFTP handle upon destruction or
/// reassignment, ensuring remote handles are never leaked on cancellation,
/// transfer errors, or early exits.
class UniqueSftpHandle {
    public:
    using CloseFunction = int (*)(LIBSSH2_SFTP_HANDLE *);

    UniqueSftpHandle() noexcept = default;

    explicit UniqueSftpHandle(
        LIBSSH2_SFTP_HANDLE *handle,
        CloseFunction closer = &defaultCloseSftpHandle) noexcept
        : handle_(handle), closer_(closer) {}

    ~UniqueSftpHandle() { reset(); }

    UniqueSftpHandle(const UniqueSftpHandle &) = delete;
    UniqueSftpHandle &operator=(const UniqueSftpHandle &) = delete;

    UniqueSftpHandle(UniqueSftpHandle &&other) noexcept
        : handle_(std::exchange(other.handle_, nullptr)),
          closer_(other.closer_) {}

    UniqueSftpHandle &operator=(UniqueSftpHandle &&other) noexcept {
        if (this == &other)
            return *this;
        if (handle_ == other.handle_) {
            other.handle_ = nullptr;
            closer_ = other.closer_;
            return *this;
        }
        reset();
        handle_ = std::exchange(other.handle_, nullptr);
        closer_ = other.closer_;
        return *this;
    }

    [[nodiscard]] LIBSSH2_SFTP_HANDLE *get() const noexcept { return handle_; }

    [[nodiscard]] explicit operator bool() const noexcept {
        return handle_ != nullptr;
    }

    [[nodiscard]] LIBSSH2_SFTP_HANDLE *release() noexcept {
        return std::exchange(handle_, nullptr);
    }

    int close() noexcept {
        LIBSSH2_SFTP_HANDLE *handle = release();
        return (handle && closer_) ? closer_(handle) : 0;
    }

    void reset(LIBSSH2_SFTP_HANDLE *handle = nullptr) noexcept {
        if (handle_ == handle)
            return;
        LIBSSH2_SFTP_HANDLE *old = handle_;
        handle_ = handle;
        if (old && closer_)
            (void)closer_(old);
    }

    private:
    LIBSSH2_SFTP_HANDLE *handle_ = nullptr;
    CloseFunction closer_ = &defaultCloseSftpHandle;
};

} // namespace openscp::libssh2detail
