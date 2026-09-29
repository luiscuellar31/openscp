#include "logic/transfers/BandwidthLimiter.hpp"

#include <algorithm>

namespace {
constexpr double kBytesPerKiB = 1024.0;
constexpr std::uint64_t kRateChunkBytes = 16 * 1024;
constexpr double kBurstSeconds = 0.25;
} // namespace

void BandwidthLimiter::setLimitKBps(int limitKBps) {
    const int boundedLimit = std::max(0, limitKBps);
    limitKBps_.store(boundedLimit);
    {
        std::lock_guard lock(mutex_);
        configuredLimitKBps_ = boundedLimit;
        lastRefill_ = std::chrono::steady_clock::now();
        tokens_ = boundedLimit > 0
                      ? double(boundedLimit) * kBytesPerKiB * kBurstSeconds
                      : 0.0;
    }
    changed_.notify_all();
}

std::size_t BandwidthLimiter::queuedWaiters() const {
    std::lock_guard lock(mutex_);
    return waiters_.size();
}

void BandwidthLimiter::removeWaiterLocked(std::uint64_t waiterId) {
    const auto position = std::find_if(
        waiters_.begin(), waiters_.end(),
        [waiterId](const Waiter &w) noexcept { return w.id == waiterId; });
    if (position != waiters_.end()) {
        waiters_.erase(position);
        changed_.notify_all();
    }
}

bool BandwidthLimiter::acquire(
    std::uint64_t taskId, std::uint64_t bytes,
    const std::function<bool(std::uint64_t)> &shouldCancel) {
    std::uint64_t remainingBytes = bytes;
    while (remainingBytes > 0) {
        const int initialLimit = limitKBps_.load();
        if (initialLimit <= 0)
            return !shouldCancel || !shouldCancel(taskId);

        const std::uint64_t burstBytes = std::max<std::uint64_t>(
            1, static_cast<std::uint64_t>(initialLimit * kBytesPerKiB *
                                          kBurstSeconds));
        const std::uint64_t chunkBytes =
            std::min({remainingBytes, kRateChunkBytes, burstBytes});

        std::unique_lock lock(mutex_);
        const std::uint64_t waiterId = ++nextWaiterId_;
        waiters_.push_back(Waiter{waiterId, taskId, chunkBytes});

        struct WaiterScopeGuard {
            BandwidthLimiter &limiter;
            std::unique_lock<std::mutex> &lock;
            std::uint64_t waiterId = 0;
            bool granted = false;

            WaiterScopeGuard(BandwidthLimiter &lim,
                             std::unique_lock<std::mutex> &lk,
                             std::uint64_t id) noexcept
                : limiter(lim), lock(lk), waiterId(id) {}

            ~WaiterScopeGuard() {
                if (granted)
                    return;
                if (lock.owns_lock()) {
                    limiter.removeWaiterLocked(waiterId);
                } else {
                    std::lock_guard<std::mutex> lk(limiter.mutex_);
                    limiter.removeWaiterLocked(waiterId);
                }
            }

            WaiterScopeGuard(const WaiterScopeGuard &) = delete;
            WaiterScopeGuard &operator=(const WaiterScopeGuard &) = delete;
        } guard(*this, lock, waiterId);

        while (true) {
            if (shouldCancel && shouldCancel(taskId)) {
                return false;
            }
            const int currentLimit = limitKBps_.load();
            if (currentLimit <= 0) {
                return true;
            }

            const auto now = std::chrono::steady_clock::now();
            if (configuredLimitKBps_ != currentLimit) {
                configuredLimitKBps_ = currentLimit;
                tokens_ = std::min(tokens_, double(currentLimit) *
                                                kBytesPerKiB * kBurstSeconds);
                lastRefill_ = now;
            }
            if (lastRefill_.time_since_epoch().count() == 0)
                lastRefill_ = now;

            const double elapsedSeconds =
                std::chrono::duration<double>(now - lastRefill_).count();
            const double capacity =
                std::max(double(chunkBytes),
                         double(currentLimit) * kBytesPerKiB * kBurstSeconds);
            tokens_ = std::min(capacity, tokens_ + elapsedSeconds *
                                                       double(currentLimit) *
                                                       kBytesPerKiB);
            lastRefill_ = now;

            if (!waiters_.empty() && waiters_.front().id == waiterId &&
                tokens_ >= double(chunkBytes)) {
                tokens_ -= double(chunkBytes);
                waiters_.pop_front();
                guard.granted = true;
                changed_.notify_all();
                break;
            }
            changed_.wait_for(lock, std::chrono::milliseconds(20));
        }
        remainingBytes -= chunkBytes;
    }
    return true;
}

void BandwidthLimiter::wakeAll() {
    changed_.notify_all();
}
