// Sizes the data passed to each blocking libssh2_sftp_write call.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace openscp::libssh2detail {

inline constexpr std::size_t kMinSftpWriteSize = 32 * 1024;

// libssh2 sends everything passed to one write before it waits for an
// acknowledgement, and the session timeout covers the whole call. Each call
// therefore passes about two seconds of data at the rate the upload has
// reached, at most twice the previous call, so slow links stay within the
// timeout while fast ones still fill the transfer buffer.
inline std::size_t nextSftpWriteSize(std::size_t current,
                                     std::uint64_t bytesWritten, double seconds,
                                     std::size_t maximum) {
    std::size_t next = current > maximum / 2 ? maximum : current * 2;
    if (seconds > 0) {
        const double twoSeconds =
            2.0 * static_cast<double>(bytesWritten) / seconds;
        if (twoSeconds < static_cast<double>(next))
            next = static_cast<std::size_t>(twoSeconds);
    }
    return std::clamp(next, std::min(kMinSftpWriteSize, maximum), maximum);
}

} // namespace openscp::libssh2detail
