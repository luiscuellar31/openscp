#include "ConnectionStatusCoordinator.hpp"

#include "TimeUtils.hpp"

#include <algorithm>
#include <utility>

namespace openscpui {

ConnectionStatusCoordinator::ConnectionStatusCoordinator() {
    timer_.setInterval(1000);
    QObject::connect(&timer_, &QTimer::timeout, [this] { refresh(); });
}

void ConnectionStatusCoordinator::setRenderCallback(RenderCallback callback) {
    render_ = std::move(callback);
    publish(resolvedNow(-1));
}

void ConnectionStatusCoordinator::start(QString connectionType, qint64 nowMs) {
    connectionType_ = std::move(connectionType);
    startedAtMs_ = resolvedNow(nowMs);
    connected_ = true;
    timer_.start();
    publish(startedAtMs_);
}

void ConnectionStatusCoordinator::reset() {
    timer_.stop();
    connectionType_.clear();
    startedAtMs_ = 0;
    connected_ = false;
    publish(resolvedNow(-1));
}

void ConnectionStatusCoordinator::refresh(qint64 nowMs) {
    publish(resolvedNow(nowMs));
}

void ConnectionStatusCoordinator::publish(qint64 nowMs) {
    if (!render_)
        return;

    Snapshot snapshot;
    snapshot.connected = connected_;
    snapshot.connectionType = connectionType_;
    if (connected_) {
        snapshot.elapsedSeconds =
            std::max<qint64>(0, (nowMs - startedAtMs_) / 1000);
    }
    render_(snapshot);
}

} // namespace openscpui
