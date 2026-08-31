// Shared resource budget for directory listings received from remote servers.
#pragma once

#include <cstddef>

namespace openscp {

inline constexpr std::size_t kMaxRemoteListingEntries = 100'000;
inline constexpr std::size_t kMaxRemoteListingNameBytes = 64 * 1024 * 1024;

class RemoteListingBudget final {
    public:
    explicit constexpr RemoteListingBudget(
        std::size_t maxEntries = kMaxRemoteListingEntries,
        std::size_t maxNameBytes = kMaxRemoteListingNameBytes) noexcept
        : maxEntries_(maxEntries), maxNameBytes_(maxNameBytes) {}

    [[nodiscard]] constexpr bool tryConsume(std::size_t nameBytes) noexcept {
        if (entries_ >= maxEntries_ || nameBytes_ > maxNameBytes_ ||
            nameBytes > maxNameBytes_ - nameBytes_) {
            return false;
        }
        ++entries_;
        nameBytes_ += nameBytes;
        return true;
    }

    [[nodiscard]] constexpr std::size_t entries() const noexcept {
        return entries_;
    }
    [[nodiscard]] constexpr std::size_t nameBytes() const noexcept {
        return nameBytes_;
    }

    private:
    std::size_t maxEntries_;
    std::size_t maxNameBytes_;
    std::size_t entries_ = 0;
    std::size_t nameBytes_ = 0;
};

} // namespace openscp
