// Storage and round-robin selection for queued transfers.
#pragma once

#include "logic/transfers/TransferTypes.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

class TransferQueueStore final {
    public:
    using Node = std::unique_ptr<TransferTask>;
    using Nodes = std::vector<Node>;

    [[nodiscard]] TransferTask *find(quint64 taskId);
    [[nodiscard]] const TransferTask *find(quint64 taskId) const;

    void append(TransferTask task);
    void rebuildIndex();

    [[nodiscard]] Nodes &nodes() noexcept { return nodes_; }
    [[nodiscard]] const Nodes &nodes() const noexcept { return nodes_; }
    [[nodiscard]] std::size_t capacity() const noexcept {
        return nodes_.capacity();
    }

    private:
    Nodes nodes_;
    std::unordered_map<quint64, TransferTask *> byId_;
};

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
