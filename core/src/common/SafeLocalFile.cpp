#include "openscp/SafeLocalFile.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <deque>
#include <filesystem>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace openscp::localfiles {
namespace {

std::string ioError(const char *operation) {
    return std::string(operation) + ": " + std::strerror(errno);
}

#ifndef _WIN32
constexpr int kDirectoryFlags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW;
constexpr int kMaximumTrustedSymlinks = 8;

void prependComponents(std::deque<std::string> &pending,
                       const std::filesystem::path &path) {
    std::vector<std::string> components;
    for (const auto &component : path) {
        const std::string value = component.string();
        if (!value.empty() && value != "/" && value != ".")
            components.push_back(value);
    }
    for (auto it = components.rbegin(); it != components.rend(); ++it)
        pending.push_front(*it);
}

bool isTrustedSystemDirectory(int descriptor) {
    struct stat metadata {};
    return ::fstat(descriptor, &metadata) == 0 && metadata.st_uid == 0 &&
           (metadata.st_mode & (S_IWGRP | S_IWOTH)) == 0 &&
           ::faccessat(descriptor, ".", W_OK, 0) != 0;
}

int openSafeDirectory(const std::filesystem::path &path, bool create,
                      std::string &error) {
    int current = ::open(path.is_absolute() ? "/" : ".", kDirectoryFlags);
    if (current < 0) {
        error = ioError("Could not open local destination directory");
        return -1;
    }

    std::deque<std::string> pending;
    prependComponents(pending, path);
    int followedSymlinks = 0;
    while (!pending.empty()) {
        const std::string component = std::move(pending.front());
        pending.pop_front();
        if (component == "..") {
            ::close(current);
            errno = EINVAL;
            error = "Local destination path contains a parent traversal.";
            return -1;
        }

        int next = ::openat(current, component.c_str(), kDirectoryFlags);
        if (next < 0 && errno == ENOENT && create) {
            if (::mkdirat(current, component.c_str(), 0755) == 0 ||
                errno == EEXIST) {
                next = ::openat(current, component.c_str(), kDirectoryFlags);
            }
        }
        if (next >= 0) {
            ::close(current);
            current = next;
            continue;
        }

        const int openError = errno;
        struct stat entry {};
        if (::fstatat(current, component.c_str(), &entry,
                      AT_SYMLINK_NOFOLLOW) == 0 &&
            S_ISLNK(entry.st_mode)) {
            if (++followedSymlinks > kMaximumTrustedSymlinks ||
                !isTrustedSystemDirectory(current)) {
                ::close(current);
                errno = ELOOP;
                error = "Local destination path contains an untrusted "
                        "symbolic link.";
                return -1;
            }
            std::array<char, 4096> target{};
            const ssize_t size = ::readlinkat(current, component.c_str(),
                                              target.data(), target.size());
            if (size < 0 || static_cast<std::size_t>(size) == target.size()) {
                const int savedError = size < 0 ? errno : ENAMETOOLONG;
                ::close(current);
                errno = savedError;
                error = ioError("Could not resolve system directory link");
                return -1;
            }
            const std::filesystem::path linkTarget(
                std::string(target.data(), static_cast<std::size_t>(size)));
            if (linkTarget.is_absolute()) {
                const int root = ::open("/", kDirectoryFlags);
                if (root < 0) {
                    const int savedError = errno;
                    ::close(current);
                    errno = savedError;
                    error = ioError("Could not open local filesystem root");
                    return -1;
                }
                ::close(current);
                current = root;
            }
            prependComponents(pending, linkTarget);
            continue;
        }

        ::close(current);
        errno = openError;
        error = ioError("Could not safely open local destination directory");
        return -1;
    }
    return current;
}

bool syncParentDirectory(int descriptor, std::string &error) {
    if (::fsync(descriptor) != 0) {
        error = ioError("Could not synchronize destination directory");
        return false;
    }
    return true;
}
#endif

} // namespace

bool ensureLocalDirectories(const std::string &path, std::string &error) {
    error.clear();
#ifdef _WIN32
    errno = EINVAL;
    error = "Safe local directory creation is unavailable on Windows.";
    return false;
#else
    const int descriptor = openSafeDirectory(path, true, error);
    if (descriptor < 0)
        return false;
    ::close(descriptor);
    return true;
#endif
}

bool removeLocalPath(const std::string &path, bool directory,
                     std::string &error) {
    error.clear();
#ifdef _WIN32
    (void)path;
    (void)directory;
    errno = EINVAL;
    error = "Safe local removal is unavailable on Windows.";
    return false;
#else
    const std::filesystem::path requested(path);
    const std::filesystem::path name = requested.filename();
    if (name.empty() || name == "." || name == "..") {
        errno = EINVAL;
        error = "Local removal path does not identify an entry.";
        return false;
    }
    const int parent = openSafeDirectory(requested.parent_path(), false, error);
    if (parent < 0) {
        if (errno == ENOENT) {
            error.clear();
            return true;
        }
        return false;
    }
    const int result =
        ::unlinkat(parent, name.c_str(), directory ? AT_REMOVEDIR : 0);
    const int savedError = errno;
    ::close(parent);
    if (result == 0 || savedError == ENOENT)
        return true;
    errno = savedError;
    error = ioError("Could not safely remove local path");
    return false;
#endif
}

bool setLocalModificationTime(const std::string &path,
                              std::int64_t modifiedSeconds,
                              std::string &error) {
    error.clear();
#ifdef _WIN32
    (void)path;
    (void)modifiedSeconds;
    errno = EINVAL;
    error = "Safe local timestamp update is unavailable on Windows.";
    return false;
#else
    const std::filesystem::path requested(path);
    const std::filesystem::path name = requested.filename();
    if (name.empty() || name == "." || name == "..") {
        errno = EINVAL;
        error = "Local timestamp path does not identify a file.";
        return false;
    }
    const int parent = openSafeDirectory(requested.parent_path(), false, error);
    if (parent < 0)
        return false;
    const int file = ::openat(parent, name.c_str(),
                              O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    const int openError = errno;
    ::close(parent);
    if (file < 0) {
        errno = openError;
        error = ioError("Could not safely open local file for timestamp");
        return false;
    }
    struct stat metadata {};
    const int statResult = ::fstat(file, &metadata);
    if (statResult != 0 || !S_ISREG(metadata.st_mode)) {
        const int savedError = statResult != 0 ? errno : EINVAL;
        ::close(file);
        errno = savedError;
        error = "Local timestamp path is not a regular file.";
        return false;
    }
    const timespec times[2] = {{0, UTIME_OMIT},
                               {static_cast<time_t>(modifiedSeconds), 0}};
    const int result = ::futimens(file, times);
    const int savedError = errno;
    ::close(file);
    if (result != 0) {
        errno = savedError;
        error = ioError("Could not set local modification time");
        return false;
    }
    return true;
#endif
}

std::FILE *openRegularFileForWrite(const std::string &path, WriteMode mode,
                                   std::string &error) {
    error.clear();
#ifdef _WIN32
    HANDLE handle = CreateFileA(
        path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        error = "Could not safely open local file for writing.";
        return nullptr;
    }
    FILE_ATTRIBUTE_TAG_INFO tagInfo{};
    if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tagInfo,
                                      sizeof(tagInfo)) ||
        (tagInfo.FileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        CloseHandle(handle);
        error = "Local partial path is not a regular non-reparse-point file.";
        errno = EINVAL;
        return nullptr;
    }
    LARGE_INTEGER position{};
    if (mode == WriteMode::Append) {
        if (!SetFilePointerEx(handle, position, nullptr, FILE_END)) {
            CloseHandle(handle);
            error = "Could not seek local partial file.";
            return nullptr;
        }
    } else if (!SetFilePointerEx(handle, position, nullptr, FILE_BEGIN) ||
               !SetEndOfFile(handle)) {
        CloseHandle(handle);
        error = "Could not truncate local partial file.";
        return nullptr;
    }

    const int descriptor = _open_osfhandle(
        reinterpret_cast<intptr_t>(handle),
        _O_BINARY | _O_WRONLY | (mode == WriteMode::Append ? _O_APPEND : 0));
    if (descriptor < 0) {
        CloseHandle(handle);
        error = ioError("Could not create local file descriptor");
        return nullptr;
    }
    std::FILE *file =
        _fdopen(descriptor, mode == WriteMode::Append ? "ab" : "wb");
    if (!file) {
        const int savedError = errno;
        _close(descriptor);
        errno = savedError;
        error = ioError("Could not create local file stream");
        return nullptr;
    }
    return file;
#else
    int flags = O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW;
    if (mode == WriteMode::Append)
        flags |= O_APPEND;

    const std::filesystem::path requestedPath(path);
    std::filesystem::path parentPath = requestedPath.parent_path();
    const std::filesystem::path fileName = requestedPath.filename();
    if (parentPath.empty())
        parentPath = ".";
    if (fileName.empty() || fileName == "." || fileName == "..") {
        error = "Local partial path does not identify a file.";
        errno = EINVAL;
        return nullptr;
    }
    const int parentDescriptor = openSafeDirectory(parentPath, false, error);
    if (parentDescriptor < 0)
        return nullptr;
    const int descriptor =
        ::openat(parentDescriptor, fileName.c_str(), flags, 0600);
    const int openError = errno;
    ::close(parentDescriptor);
    if (descriptor < 0) {
        errno = openError;
        error = ioError("Could not safely open local file for writing");
        return nullptr;
    }

    struct stat metadata {};
    const int statResult = ::fstat(descriptor, &metadata);
    if (statResult != 0 || !S_ISREG(metadata.st_mode) ||
        metadata.st_uid != ::geteuid() || metadata.st_nlink != 1) {
        const int savedError = statResult != 0 ? errno : EINVAL;
        ::close(descriptor);
        errno = savedError;
        error = "Local partial path must be a user-owned regular file without "
                "additional hard links.";
        return nullptr;
    }
    if (::fchmod(descriptor, S_IRUSR | S_IWUSR) != 0) {
        const int savedError = errno;
        ::close(descriptor);
        errno = savedError;
        error = ioError("Could not restrict local partial file permissions");
        return nullptr;
    }
    if (mode == WriteMode::Truncate && (::ftruncate(descriptor, 0) != 0 ||
                                        ::lseek(descriptor, 0, SEEK_SET) < 0)) {
        const int savedError = errno;
        ::close(descriptor);
        errno = savedError;
        error = ioError("Could not truncate local partial file");
        return nullptr;
    }

    std::FILE *file =
        ::fdopen(descriptor, mode == WriteMode::Append ? "ab" : "wb");
    if (!file) {
        const int savedError = errno;
        ::close(descriptor);
        errno = savedError;
        error = ioError("Could not create local file stream");
        return nullptr;
    }
    return file;
#endif
}

bool flushAndSync(std::FILE *file, std::string &error,
                  LocalFileDurability durability) {
    if (!file) {
        error = "Invalid local file handle.";
        errno = EINVAL;
        return false;
    }
    if (std::fflush(file) != 0) {
        error = ioError("Could not flush local file");
        return false;
    }
    durability = normalizeLocalFileDurability(durability);
    if (durability == LocalFileDurability::Buffered)
        return true;
#ifdef _WIN32
    if (_commit(_fileno(file)) != 0) {
#else
    if (::fsync(::fileno(file)) != 0) {
#endif
        error = ioError("Could not synchronize local file");
        return false;
    }
    return true;
}

bool atomicReplace(const std::string &temporary, const std::string &destination,
                   std::string &error, LocalFileDurability durability) {
    durability = normalizeLocalFileDurability(durability);
#ifdef _WIN32
    DWORD flags = MOVEFILE_REPLACE_EXISTING;
    if (durability == LocalFileDurability::FileAndDirectory)
        flags |= MOVEFILE_WRITE_THROUGH;
    if (!MoveFileExA(temporary.c_str(), destination.c_str(), flags)) {
        error = "Could not atomically finalize local file.";
        return false;
    }
    return true;
#else
    const std::filesystem::path temporaryPath(temporary);
    const std::filesystem::path destinationPath(destination);
    const std::filesystem::path temporaryName = temporaryPath.filename();
    const std::filesystem::path destinationName = destinationPath.filename();
    if (temporaryPath.parent_path().lexically_normal() !=
            destinationPath.parent_path().lexically_normal() ||
        temporaryName.empty() || destinationName.empty() ||
        temporaryName == "." || temporaryName == ".." ||
        destinationName == "." || destinationName == "..") {
        errno = EINVAL;
        error = "Local atomic replacement requires sibling file paths.";
        return false;
    }
    const int parent =
        openSafeDirectory(destinationPath.parent_path(), false, error);
    if (parent < 0)
        return false;
    const int result = ::renameat(parent, temporaryName.c_str(), parent,
                                  destinationName.c_str());
    const int savedError = errno;
    if (result != 0) {
        ::close(parent);
        errno = savedError;
        error = ioError("Could not atomically finalize local file");
        return false;
    }
    const bool synced = durability != LocalFileDurability::FileAndDirectory ||
                        syncParentDirectory(parent, error);
    const int syncError = errno;
    ::close(parent);
    if (!synced)
        errno = syncError;
    return synced;
#endif
}

} // namespace openscp::localfiles
