#include "SessionHealthMonitor.hpp"

#include "TimeUtils.hpp"

#include <QCoreApplication>
#include <QGuiApplication>

#include <algorithm>
#include <utility>

namespace openscpui {
namespace {

constexpr int kMinimumIntervalMs = 60 * 1000;
constexpr qint64 kRecentActivityMs = 60 * 1000;
constexpr qint64 kResumeProbeThresholdMs = 60 * 1000;

} // namespace

SessionHealthMonitor::SessionHealthMonitor(QObject *parent) : QObject(parent) {
    timer_.setSingleShot(false);
    timer_.setInterval(10 * 60 * 1000);
    connect(&timer_, &QTimer::timeout, this, [this] {
        const QString reason = callbacks_.periodicReason
                                   ? callbacks_.periodicReason()
                                   : QStringLiteral("periodic");
        (void)requestProbe(reason, false);
    });

    auto *application =
        qobject_cast<QGuiApplication *>(QCoreApplication::instance());
    if (application) {
        connect(application, &QGuiApplication::applicationStateChanged, this,
                [this](Qt::ApplicationState state) {
                    handleApplicationState(state);
                });
    }
}

void SessionHealthMonitor::setCallbacks(Callbacks callbacks) {
    callbacks_ = std::move(callbacks);
}

void SessionHealthMonitor::setInterval(int milliseconds) {
    timer_.setInterval(std::max(milliseconds, kMinimumIntervalMs));
}

int SessionHealthMonitor::interval() const noexcept {
    return timer_.interval();
}

void SessionHealthMonitor::start() {
    if (running_)
        return;
    running_ = true;
    inactiveSinceMs_ = 0;
    if (!timer_.isActive())
        timer_.start();
}

void SessionHealthMonitor::stop() {
    running_ = false;
    timer_.stop();
    inactiveSinceMs_ = 0;
    if (activeProbe_.jobId != 0 && callbacks_.cancelProbe)
        callbacks_.cancelProbe(activeProbe_.jobId);
    clearActiveProbe();
    lastSuccessfulActivityAtMs_ = 0;
}

bool SessionHealthMonitor::isRunning() const noexcept {
    return running_;
}

void SessionHealthMonitor::recordActivity(qint64 nowMs) {
    lastSuccessfulActivityAtMs_ = resolvedNow(nowMs);
}

bool SessionHealthMonitor::requestProbe(const QString &reason, bool force,
                                        qint64 nowMs) {
    if (!running_ || activeProbe_.jobId != 0 || !callbacks_.submitProbe)
        return false;
    if (callbacks_.canProbe && !callbacks_.canProbe())
        return false;

    const qint64 now = resolvedNow(nowMs);
    if (!force && lastSuccessfulActivityAtMs_ > 0 &&
        now - lastSuccessfulActivityAtMs_ < kRecentActivityMs) {
        return false;
    }

    QString path = callbacks_.probePath ? callbacks_.probePath() : QString();
    if (path.isEmpty())
        path = QStringLiteral("/");
    const quint64 jobId = callbacks_.submitProbe(path);
    if (jobId == 0)
        return false;

    activeProbe_ = {jobId, reason, force};
    return true;
}

bool SessionHealthMonitor::completeProbe(quint64 jobId, ProbeOutcome outcome,
                                         const QString &error, qint64 nowMs) {
    if (jobId == 0 || jobId != activeProbe_.jobId)
        return false;

    const ProbeContext completed = activeProbe_;
    clearActiveProbe();
    if (outcome == ProbeOutcome::Succeeded) {
        recordActivity(nowMs);
        if (callbacks_.probeSucceeded)
            callbacks_.probeSucceeded(completed);
    } else if (outcome == ProbeOutcome::Failed && callbacks_.probeFailed) {
        callbacks_.probeFailed(completed, error);
    }
    return true;
}

void SessionHealthMonitor::handleApplicationState(Qt::ApplicationState state,
                                                  qint64 nowMs) {
    if (!running_)
        return;
    const qint64 now = resolvedNow(nowMs);
    if (state != Qt::ApplicationActive) {
        if (inactiveSinceMs_ <= 0)
            inactiveSinceMs_ = now;
        return;
    }
    if (inactiveSinceMs_ <= 0)
        return;

    const qint64 inactiveMs = now - inactiveSinceMs_;
    inactiveSinceMs_ = 0;
    if (inactiveMs < kResumeProbeThresholdMs)
        return;
    const QString reason = callbacks_.resumeReason
                               ? callbacks_.resumeReason(inactiveMs / 1000)
                               : QStringLiteral("resume");
    (void)requestProbe(reason, true, now);
}

bool SessionHealthMonitor::hasProbeInFlight() const noexcept {
    return activeProbe_.jobId != 0;
}

quint64 SessionHealthMonitor::activeJobId() const noexcept {
    return activeProbe_.jobId;
}

void SessionHealthMonitor::clearActiveProbe() {
    activeProbe_ = {};
}

} // namespace openscpui
