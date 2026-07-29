// Round-robin task selection independent of queue execution.
#pragma once

#include "TransferQueueStore.hpp"

#include <functional>
#include <optional>

class TransferScheduler final {
    public:
    using RunnablePredicate = std::function<bool(TransferTask &)>;

    [[nodiscard]] std::optional<std::size_t>
    nextRunnable(TransferQueueStore::Nodes &tasks,
                 const RunnablePredicate &isRunnable);
    void normalizeForSize(std::size_t taskCount);

    private:
    std::size_t cursor_ = 0;
};
