#include "TransferQueue.hpp"

#include <stdexcept>
#include <utility>

TransferTask *TransferQueueStore::find(quint64 taskId) {
    const auto found = byId_.find(taskId);
    return found == byId_.end() ? nullptr : found->second;
}

const TransferTask *TransferQueueStore::find(quint64 taskId) const {
    const auto found = byId_.find(taskId);
    return found == byId_.end() ? nullptr : found->second;
}

void TransferQueueStore::append(TransferTask task) {
    auto node = std::make_unique<TransferTask>(std::move(task));
    const quint64 taskId = node->taskId;
    TransferTask *taskPointer = node.get();
    nodes_.push_back(std::move(node));
    const auto [unused, inserted] = byId_.emplace(taskId, taskPointer);
    (void)unused;
    if (!inserted) {
        nodes_.pop_back();
        throw std::logic_error("duplicate transfer task id");
    }
}

void TransferQueueStore::rebuildIndex() {
    byId_.clear();
    byId_.reserve(nodes_.size());
    for (const Node &node : nodes_) {
        if (node)
            byId_[node->taskId] = node.get();
    }
}

std::optional<std::size_t>
TransferScheduler::nextRunnable(TransferQueueStore::Nodes &tasks,
                                const RunnablePredicate &isRunnable) {
    normalizeForSize(tasks.size());
    if (tasks.empty())
        return std::nullopt;

    for (std::size_t offset = 0; offset < tasks.size(); ++offset) {
        const std::size_t index = (cursor_ + offset) % tasks.size();
        if (!tasks[index] || !isRunnable(*tasks[index]))
            continue;
        cursor_ = (index + 1) % tasks.size();
        return index;
    }
    return std::nullopt;
}

void TransferScheduler::normalizeForSize(std::size_t taskCount) {
    cursor_ = taskCount == 0 ? 0 : cursor_ % taskCount;
}
