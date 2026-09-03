// Owns connection-duration state independently from MainWindow presentation.
#pragma once

#include <QString>
#include <QTimer>

#include <functional>

namespace openscpui {

class ConnectionStatusCoordinator final {
    public:
    struct Snapshot {
        bool connected = false;
        QString connectionType;
        qint64 elapsedSeconds = 0;
    };

    using RenderCallback = std::function<void(const Snapshot &)>;

    ConnectionStatusCoordinator();

    void setRenderCallback(RenderCallback callback);
    void start(QString connectionType, qint64 nowMs = -1);
    void reset();
    void refresh(qint64 nowMs = -1);

    [[nodiscard]] bool isConnected() const { return connected_; }

    private:
    void publish(qint64 nowMs);

    QTimer timer_;
    RenderCallback render_;
    QString connectionType_;
    qint64 startedAtMs_ = 0;
    bool connected_ = false;
};

} // namespace openscpui
