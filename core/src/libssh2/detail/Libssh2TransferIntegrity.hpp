#pragma once

#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <string>

#ifndef _WIN32
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#else
#include "common/Win32Text.hpp"

#include <windows.h>
#endif

namespace openscp::libssh2detail {

inline bool seekLocalFile(std::FILE *f, std::uint64_t off,
                          std::string *why = nullptr) {
#ifdef _WIN32
    if (_fseeki64(f, static_cast<__int64>(off), SEEK_SET) != 0) {
        if (why)
            *why = "local seek failed";
        return false;
    }
#else
    if (fseeko(f, static_cast<off_t>(off), SEEK_SET) != 0) {
        if (why)
            *why = "fseeko failed";
        return false;
    }
#endif
    return true;
}

inline bool getLocalFileSize(const std::string &path, std::uint64_t &out,
                             std::string *why = nullptr) {
#ifndef _WIN32
    struct ::stat st {};
    if (::stat(path.c_str(), &st) != 0) {
        if (why)
            *why = "stat(local) failed";
        return false;
    }
    out = static_cast<std::uint64_t>(st.st_size);
    return true;
#else
    std::wstring widePath;
    if (!win32::toExtendedLengthPath(path, widePath)) {
        if (why)
            *why = "local path is not valid UTF-8";
        return false;
    }
    // Attribute access, all a size needs, never conflicts with the sharing
    // mode of a program that has the file open, as read access would.
    HANDLE h =
        CreateFileW(widePath.c_str(), FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (why)
            *why = "CreateFile(local-size) failed";
        return false;
    }
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz)) {
        if (why)
            *why = "GetFileSizeEx(local) failed";
        CloseHandle(h);
        return false;
    }
    CloseHandle(h);
    out = static_cast<std::uint64_t>(sz.QuadPart);
    return true;
#endif
}

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
