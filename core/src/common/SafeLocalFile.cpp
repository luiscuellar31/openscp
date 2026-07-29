#include "SafeLocalFile.hpp"

#include <cerrno>
#include <cstring>
#include <filesystem>

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
bool syncParentDirectory(const std::string &path, std::string &error) {
    std::filesystem::path parent =
        std::filesystem::path(path).parent_path();
    if (parent.empty())
        parent = ".";
#ifdef O_DIRECTORY
    const int descriptor =
        ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
#else
    const int descriptor = ::open(parent.c_str(), O_RDONLY | O_CLOEXEC);
#endif
    if (descriptor < 0) {
        error = ioError("Could not open destination directory");
        return false;
    }
    const int result = ::fsync(descriptor);
    const int savedError = errno;
    ::close(descriptor);
    if (result != 0) {
        errno = savedError;
        error = ioError("Could not synchronize destination directory");
        return false;
    }
    return true;
}
#endif

} // namespace

std::FILE *openRegularFileForWrite(const std::string &path, WriteMode mode,
                                   std::string &error) {
    error.clear();
#ifdef _WIN32
    HANDLE handle =
        CreateFileA(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                    OPEN_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                    nullptr);
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
        error =
            "Local partial path is not a regular non-reparse-point file.";
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

    const int descriptor =
        _open_osfhandle(reinterpret_cast<intptr_t>(handle),
                       _O_BINARY | _O_WRONLY |
                           (mode == WriteMode::Append ? _O_APPEND : 0));
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
#ifdef O_DIRECTORY
    const int parentDescriptor =
        ::open(parentPath.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
#else
    const int parentDescriptor =
        ::open(parentPath.c_str(), O_RDONLY | O_CLOEXEC);
#endif
    if (parentDescriptor < 0) {
        error = ioError("Could not safely open local destination directory");
        return nullptr;
    }
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
    if (::fstat(descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
        metadata.st_uid != ::geteuid() || metadata.st_nlink != 1) {
        const int savedError = errno != 0 ? errno : EINVAL;
        ::close(descriptor);
        errno = savedError;
        error =
            "Local partial path must be a user-owned regular file without "
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
    if (mode == WriteMode::Truncate &&
        (::ftruncate(descriptor, 0) != 0 ||
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

bool flushAndSync(std::FILE *file, std::string &error) {
    if (!file) {
        error = "Invalid local file handle.";
        errno = EINVAL;
        return false;
    }
    if (std::fflush(file) != 0) {
        error = ioError("Could not flush local file");
        return false;
    }
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

bool atomicReplace(const std::string &temporary,
                   const std::string &destination, std::string &error) {
#ifdef _WIN32
    if (!MoveFileExA(temporary.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "Could not atomically finalize local file.";
        return false;
    }
    return true;
#else
    if (::rename(temporary.c_str(), destination.c_str()) != 0) {
        error = ioError("Could not atomically finalize local file");
        return false;
    }
    return syncParentDirectory(destination, error);
#endif
}

} // namespace openscp::localfiles
