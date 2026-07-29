#include "TransferScheduler.hpp"

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
