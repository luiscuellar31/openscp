#pragma once

#include <atomic>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace openscp {

class SecureString {
    public:
    SecureString() = default;
    SecureString(const char *value)
        : SecureString(value ? std::string_view(value) : std::string_view()) {}
    SecureString(const std::string &value)
        : SecureString(std::string_view(value)) {}
    SecureString(std::string_view value) { assign(value); }

    SecureString(const SecureString &other) { assign(other.view()); }
    SecureString &operator=(const SecureString &other) {
        if (this != &other)
            assign(other.view());
        return *this;
    }

    SecureString(SecureString &&other) noexcept
        : data_(std::move(other.data_)), size_(std::exchange(other.size_, 0)) {}
    SecureString &operator=(SecureString &&other) noexcept {
        if (this == &other)
            return *this;
        clear();
        data_ = std::move(other.data_);
        size_ = std::exchange(other.size_, 0);
        return *this;
    }

    SecureString &operator=(std::string_view value) {
        assign(value);
        return *this;
    }

    ~SecureString() { clear(); }

    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] const char *c_str() const noexcept {
        return data_ ? data_.get() : "";
    }
    [[nodiscard]] const char *data() const noexcept { return c_str(); }
    [[nodiscard]] const char *begin() const noexcept { return data(); }
    [[nodiscard]] const char *end() const noexcept { return data() + size_; }
    [[nodiscard]] std::string_view view() const noexcept {
        return {data(), size_};
    }
    void clear() noexcept {
        if (data_)
            secureErase(data_.get(), size_ + 1);
        data_.reset();
        size_ = 0;
    }

    friend bool operator==(const SecureString &left,
                           const SecureString &right) noexcept {
        return left.view() == right.view();
    }
    friend bool operator==(const SecureString &left,
                           std::string_view right) noexcept {
        return left.view() == right;
    }
    friend bool operator==(std::string_view left,
                           const SecureString &right) noexcept {
        return left == right.view();
    }
    friend bool operator==(const SecureString &left,
                           const char *right) noexcept {
        return left.view() ==
               (right ? std::string_view(right) : std::string_view());
    }
    friend bool operator==(const char *left,
                           const SecureString &right) noexcept {
        return right == left;
    }

    private:
    static void secureErase(char *buffer, std::size_t size) noexcept {
        volatile unsigned char *cursor =
            reinterpret_cast<volatile unsigned char *>(buffer);
        while (size-- > 0)
            *cursor++ = 0;
        std::atomic_signal_fence(std::memory_order_seq_cst);
    }

    void assign(std::string_view value) {
        if (value.empty()) {
            clear();
            return;
        }
        auto replacement = std::make_unique<char[]>(value.size() + 1);
        std::memcpy(replacement.get(), value.data(), value.size());
        replacement[value.size()] = '\0';
        clear();
        data_ = std::move(replacement);
        size_ = value.size();
    }

    std::unique_ptr<char[]> data_;
    std::size_t size_ = 0;
};

} // namespace openscp
