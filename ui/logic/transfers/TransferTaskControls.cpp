#include "logic/transfers/TransferTaskControls.hpp"

#include <utility>

void TransferTaskControls::cancel(quint64 taskId) {
    canceled_.insert(taskId);
    paused_.erase(taskId);
    publishStop(taskId);
}

void TransferTaskControls::pause(quint64 taskId) {
    paused_.insert(taskId);
    publishStop(taskId);
}

void TransferTaskControls::resume(quint64 taskId) {
    paused_.erase(taskId);
    publishStop(taskId);
}

void TransferTaskControls::forget(quint64 taskId) {
    canceled_.erase(taskId);
    paused_.erase(taskId);
    publishStop(taskId);
}

bool TransferTaskControls::isCanceled(quint64 taskId) const {
    return canceled_.count(taskId) != 0;
}

bool TransferTaskControls::isPaused(quint64 taskId) const {
    return paused_.count(taskId) != 0;
}

void TransferTaskControls::startRunning(quint64 taskId, int speedLimitKBps) {
    auto taskSignals = std::make_shared<Signals>();
    taskSignals->speedLimitKBps.store(speedLimitKBps);
    running_[taskId] = std::move(taskSignals);
    publishStop(taskId);
}

void TransferTaskControls::finishRunning(quint64 taskId) {
    running_.erase(taskId);
}

std::shared_ptr<const TransferTaskControls::Signals>
TransferTaskControls::runningSignals(quint64 taskId) const {
    const auto found = running_.find(taskId);
    return found == running_.end() ? nullptr : found->second;
}

void TransferTaskControls::setSpeedLimit(quint64 taskId, int speedLimitKBps) {
    const auto found = running_.find(taskId);
    if (found != running_.end())
        found->second->speedLimitKBps.store(speedLimitKBps);
}

void TransferTaskControls::publishStop(quint64 taskId) {
    const auto found = running_.find(taskId);
    if (found != running_.end())
        found->second->stopRequested.store(isCanceled(taskId) ||
                                           isPaused(taskId));
}
