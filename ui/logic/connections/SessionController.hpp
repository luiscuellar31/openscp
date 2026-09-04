#pragma once

#include "openscp/RemoteClient.hpp"

#include <QObject>

#include <atomic>
#include <memory>
#include <optional>

namespace openscpui {

class SessionController final : public QObject {
    public:
    explicit SessionController(QObject *parent = nullptr);
    ~SessionController() override;

    openscp::RemoteClient *client() const { return client_.get(); }
    void installClient(std::unique_ptr<openscp::RemoteClient> client);
    void disconnectClient();

    const std::optional<openscp::SessionOptions> &options() const {
        return options_;
    }
    void setOptions(openscp::SessionOptions options);
    void clearOptions();

    bool
    beginConnection(const std::shared_ptr<std::atomic<bool>> &cancelRequested);
    bool isConnecting() const { return connecting_; }
    bool requestConnectionCancellation();
    void finishConnection();

    quint64 beginDisconnect();
    bool isDisconnecting() const { return disconnecting_; }
    quint64 disconnectSequence() const { return disconnectSequence_; }
    bool isCurrentDisconnect(quint64 sequence) const;
    void finishDisconnect(quint64 sequence);

    private:
    std::unique_ptr<openscp::RemoteClient> client_;
    std::optional<openscp::SessionOptions> options_;
    std::shared_ptr<std::atomic<bool>> connectionCancelRequested_;
    bool connecting_ = false;
    bool disconnecting_ = false;
    quint64 disconnectSequence_ = 0;
};

} // namespace openscpui
