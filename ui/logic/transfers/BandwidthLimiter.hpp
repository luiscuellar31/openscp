// Fair shared token bucket used by concurrent transfer workers.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>

class BandwidthLimiter final {
    public:
    void setLimitKBps(int limitKBps);
    [[nodiscard]] int limitKBps() const noexcept { return limitKBps_.load(); }

    [[nodiscard]] bool
    acquire(std::uint64_t taskId, std::uint64_t bytes,
            const std::function<bool(std::uint64_t)> &shouldCancel);
    void wakeAll();

    [[nodiscard]] std::size_t queuedWaiters() const;

    private:
    struct Waiter {
        std::uint64_t id = 0;
        std::uint64_t taskId = 0;
        std::uint64_t bytes = 0;
    };

    void removeWaiterLocked(std::uint64_t waiterId);

    std::atomic<int> limitKBps_{0};
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<Waiter> waiters_;
    std::uint64_t nextWaiterId_ = 0;
    double tokens_ = 0.0;
    std::chrono::steady_clock::time_point lastRefill_{};
    int configuredLimitKBps_ = 0;
};
