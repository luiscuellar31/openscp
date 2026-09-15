// Performs one transfer attempt without owning queue or scheduling state.
#pragma once

#include "logic/transfers/TransferTypes.hpp"

#include <functional>
#include <memory>
#include <string>

namespace openscp {
class RemoteClient;
}

class TransferExecutor final {
    public:
    struct Callbacks {
        std::function<bool()> shouldCancel;
        std::function<bool(quint64 bytes)> acquireGlobalBandwidth;
        std::function<int()> taskSpeedLimitKBps;
        std::function<void(std::size_t done, std::size_t total,
                           double measuredKBps, int etaSeconds,
                           bool publishNow)>
            progress;
    };

    [[nodiscard]] static bool
    run(TransferTask &task,
        const std::shared_ptr<openscp::RemoteClient> &remoteClient, bool resume,
        std::string &error, const Callbacks &callbacks);

    [[nodiscard]] static bool
    runPostAction(TransferTask &task,
                  const std::shared_ptr<openscp::RemoteClient> &remoteClient,
                  std::string &error);
};
