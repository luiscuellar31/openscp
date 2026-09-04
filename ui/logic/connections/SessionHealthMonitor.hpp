#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

#include <functional>

namespace openscpui {

class SessionHealthMonitor final : public QObject {
    public:
    enum class ProbeOutcome { Succeeded, Failed, Canceled, Superseded };

    struct ProbeContext {
        quint64 jobId = 0;
        QString reason;
        bool forced = false;
    };

    struct Callbacks {
        std::function<bool()> canProbe;
        std::function<QString()> probePath;
        std::function<quint64(const QString &)> submitProbe;
        std::function<void(quint64)> cancelProbe;
        std::function<QString()> periodicReason;
        std::function<QString(qint64)> resumeReason;
        std::function<void(const ProbeContext &)> probeSucceeded;
        std::function<void(const ProbeContext &, const QString &)> probeFailed;
    };

    explicit SessionHealthMonitor(QObject *parent = nullptr);

    void setCallbacks(Callbacks callbacks);
    void setInterval(int milliseconds);
    [[nodiscard]] int interval() const noexcept;

    void start();
    void stop();
    [[nodiscard]] bool isRunning() const noexcept;

    void recordActivity(qint64 nowMs = -1);
    [[nodiscard]] bool requestProbe(const QString &reason, bool force,
                                    qint64 nowMs = -1);
    [[nodiscard]] bool completeProbe(quint64 jobId, ProbeOutcome outcome,
                                     const QString &error = {},
                                     qint64 nowMs = -1);
    void handleApplicationState(Qt::ApplicationState state, qint64 nowMs = -1);

    [[nodiscard]] bool hasProbeInFlight() const noexcept;
    [[nodiscard]] quint64 activeJobId() const noexcept;

    private:
    void clearActiveProbe();

    QTimer timer_;
    Callbacks callbacks_;
    ProbeContext activeProbe_;
    qint64 lastSuccessfulActivityAtMs_ = 0;
    qint64 inactiveSinceMs_ = 0;
    bool running_ = false;
};

} // namespace openscpui
