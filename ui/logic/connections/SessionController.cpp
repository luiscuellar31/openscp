#include "logic/connections/SessionController.hpp"

#include <utility>

namespace openscpui {

SessionController::SessionController(QObject *parent) : QObject(parent) {
}

SessionController::~SessionController() {
    disconnectClient();
}

void SessionController::installClient(
    std::unique_ptr<openscp::RemoteClient> client) {
    disconnectClient();
    client_ = std::move(client);
}

void SessionController::disconnectClient() {
    if (client_)
        client_->disconnect();
    client_.reset();
}

void SessionController::setOptions(openscp::SessionOptions options) {
    options_ = std::move(options);
}

void SessionController::clearOptions() {
    options_.reset();
}

bool SessionController::beginConnection(
    const std::shared_ptr<std::atomic<bool>> &cancelRequested) {
    if (connecting_ || disconnecting_ || !cancelRequested)
        return false;
    connectionCancelRequested_ = cancelRequested;
    connectionCancelRequested_->store(false);
    connecting_ = true;
    return true;
}

bool SessionController::requestConnectionCancellation() {
    if (!connecting_ || !connectionCancelRequested_)
        return false;
    connectionCancelRequested_->store(true);
    return true;
}

void SessionController::finishConnection() {
    connectionCancelRequested_.reset();
    connecting_ = false;
}

quint64 SessionController::beginDisconnect() {
    if (disconnecting_)
        return disconnectSequence_;
    disconnecting_ = true;
    return ++disconnectSequence_;
}

bool SessionController::isCurrentDisconnect(quint64 sequence) const {
    return disconnecting_ && sequence == disconnectSequence_;
}

void SessionController::finishDisconnect(quint64 sequence) {
    if (sequence == disconnectSequence_)
        disconnecting_ = false;
}

} // namespace openscpui
