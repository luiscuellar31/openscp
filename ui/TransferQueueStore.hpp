// Stable task ownership and O(1) identity lookup for the transfer queue.
#pragma once

#include "TransferTypes.hpp"

#include <memory>
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
