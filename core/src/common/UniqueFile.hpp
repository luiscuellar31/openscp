// Internal RAII owner for C FILE handles.
#pragma once

#include <cstdio>
#include <utility>

namespace openscp {

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
        if (file_)
            (void)closer_(file_);
        file_ = file;
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
