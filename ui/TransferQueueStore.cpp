#include "TransferQueueStore.hpp"

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
