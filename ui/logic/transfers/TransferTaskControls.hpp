// Stop requests and speed limits the queue has set for its tasks.
#pragma once

#include <QtGlobal>

#include <atomic>
#include <memory>
#include <unordered_map>
#include <unordered_set>

// The owner serializes every call with its own mutex. A running task also
// gets Signals, atomics that mirror its stop request and speed limit so its
// worker can check them on every chunk without taking that mutex.
class TransferTaskControls final {
    public:
    struct Signals {
        std::atomic_bool stopRequested{false};
        std::atomic<int> speedLimitKBps{0};
    };

    void cancel(quint64 taskId);
    void pause(quint64 taskId);
    // Clears a pause request, but not a cancellation.
    void resume(quint64 taskId);
    // Clears both requests, for work that is retried or removed.
    void forget(quint64 taskId);

    [[nodiscard]] bool isCanceled(quint64 taskId) const;
    [[nodiscard]] bool isPaused(quint64 taskId) const;

    void startRunning(quint64 taskId, int speedLimitKBps);
    void finishRunning(quint64 taskId);
    [[nodiscard]] std::shared_ptr<const Signals>
    runningSignals(quint64 taskId) const;
    void setSpeedLimit(quint64 taskId, int speedLimitKBps);

    private:
    void publishStop(quint64 taskId);

    std::unordered_set<quint64> canceled_;
    std::unordered_set<quint64> paused_;
    std::unordered_map<quint64, std::shared_ptr<Signals>> running_;
};
