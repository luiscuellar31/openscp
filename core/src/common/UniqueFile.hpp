// Internal RAII owner for C FILE handles.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <utility>

#ifdef _WIN32
#include "common/Win32Text.hpp"

#include <cerrno>
#endif

namespace openscp {

// Opens a local file by its UTF-8 path, as std::fopen() does. Windows reads
// narrow paths in the system code page, so there the path goes through the
// UTF-16 API, in the \\?\ form that also reaches past MAX_PATH, and child
// processes do not inherit the handle ("N").
inline std::FILE *openLocalFile(const std::string &path, const char *mode) {
#ifdef _WIN32
    std::wstring widePath;
    if (!win32::toExtendedLengthPath(path, widePath)) {
        errno = EINVAL;
        return nullptr;
    }
    std::wstring wideMode = win32::toWide(mode);
    wideMode += L'N';
    return ::_wfopen(widePath.c_str(), wideMode.c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

// The size of an open local file. Unlike a size looked up by path, it belongs
// to the file actually opened.
inline bool localFileSize(std::FILE *file, std::uint64_t &size) {
#ifdef _WIN32
    struct _stat64 status {};
    if (::_fstat64(::_fileno(file), &status) != 0)
        return false;
#else
    struct ::stat status {};
    if (::fstat(::fileno(file), &status) != 0)
        return false;
#endif
    size = static_cast<std::uint64_t>(status.st_size);
    return true;
}

class UniqueFile {
    public:
    using CloseFunction = int (*)(std::FILE *);

    UniqueFile() noexcept = default;
    explicit UniqueFile(std::FILE *file,
                        CloseFunction closer = &std::fclose) noexcept
        : file_(file), closer_(closer) {}

    ~UniqueFile() { reset(); }

    UniqueFile(const UniqueFile &) = delete;
    UniqueFile &operator=(const UniqueFile &) = delete;

    UniqueFile(UniqueFile &&other) noexcept
        : file_(std::exchange(other.file_, nullptr)), closer_(other.closer_) {}

    UniqueFile &operator=(UniqueFile &&other) noexcept {
        if (this == &other)
            return *this;
        if (file_ == other.file_) {
            other.file_ = nullptr;
            closer_ = other.closer_;
            return *this;
        }
        reset();
        file_ = std::exchange(other.file_, nullptr);
        closer_ = other.closer_;
        return *this;
    }

    [[nodiscard]] std::FILE *get() const noexcept { return file_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return file_ != nullptr;
    }

    [[nodiscard]] std::FILE *release() noexcept {
        return std::exchange(file_, nullptr);
    }

    int close() noexcept {
        std::FILE *file = release();
        return file ? closer_(file) : 0;
    }

    void reset(std::FILE *file = nullptr) noexcept {
        if (file_ == file)
            return;
        std::FILE *old = file_;
        file_ = file;
        if (old)
            (void)closer_(old);
    }

    void reset(std::FILE *file, CloseFunction closer) noexcept {
        if (file_ == file) {
            closer_ = closer;
            return;
        }
        std::FILE *old = file_;
        file_ = file;
        CloseFunction oldCloser = closer_;
        closer_ = closer;
        if (old)
            (void)oldCloser(old);
    }

    void swap(UniqueFile &other) noexcept {
        std::swap(file_, other.file_);
        std::swap(closer_, other.closer_);
    }

    private:
    std::FILE *file_ = nullptr;
    CloseFunction closer_ = &std::fclose;
};

inline void swap(UniqueFile &left, UniqueFile &right) noexcept {
    left.swap(right);
}

} // namespace openscp
