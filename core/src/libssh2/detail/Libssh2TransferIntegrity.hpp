#pragma once

#include <cstdint>
#include <limits>
#include <optional>

namespace openscp::libssh2detail {

// SFTP v3 reports modification times in whole seconds, so a same-size write
// in the same second as the last modification leaves the metadata unchanged.
// Files modified this recently may still be changing and need the full
// end-to-end integrity check even under the Optional policy.
inline constexpr std::int64_t kRecentRemoteModificationSeconds = 10 * 60;

// A server clock behind the local clock by more than the window hides recent
// changes; a clock ahead of it only causes extra full checks. An unknown
// modification time is treated as recent.
inline bool
remoteFileMayBeChanging(std::optional<std::uint64_t> modifiedSeconds,
                        std::int64_t nowSeconds) {
    if (!modifiedSeconds ||
        *modifiedSeconds > static_cast<std::uint64_t>(
                               std::numeric_limits<std::int64_t>::max()))
        return true;
    return static_cast<std::int64_t>(*modifiedSeconds) >=
           nowSeconds - kRecentRemoteModificationSeconds;
}

} // namespace openscp::libssh2detail
