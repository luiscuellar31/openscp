#pragma once

#include "openscp/Protocol.hpp"
#include "openscp/SessionOptions.hpp"

#include <QObject>

#include <atomic>
#include <memory>
#include <optional>

namespace openscpui {

class SessionController final : public QObject {
    public:
    explicit SessionController(QObject *parent = nullptr);
    ~SessionController() override;

    // A session exists from a successful connection until its disconnect
    // completes. Its connections belong to the remote operation lane and the
    // transfer workers; this keeps what the connection reported.
    bool hasSession() const { return capabilities_.has_value(); }
    openscp::ProtocolCapabilities capabilities() const {
        return capabilities_.value_or(openscp::ProtocolCapabilities{});
    }
    void beginSession(openscp::ProtocolCapabilities capabilities);
    void endSession();

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
    std::optional<openscp::ProtocolCapabilities> capabilities_;
    std::optional<openscp::SessionOptions> options_;
    std::shared_ptr<std::atomic<bool>> connectionCancelRequested_;
    bool connecting_ = false;
    bool disconnecting_ = false;
    quint64 disconnectSequence_ = 0;
};

} // namespace openscpui
