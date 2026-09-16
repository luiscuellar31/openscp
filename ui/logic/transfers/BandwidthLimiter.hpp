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

    private:
    struct Waiter {
        std::uint64_t taskId = 0;
        std::uint64_t bytes = 0;
    };

    std::atomic<int> limitKBps_{0};
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<Waiter *> waiters_;
    double tokens_ = 0.0;
    std::chrono::steady_clock::time_point lastRefill_{};
    int configuredLimitKBps_ = 0;
};
