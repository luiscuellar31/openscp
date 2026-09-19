#pragma once

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <QThreadPool>

#include <chrono>
#include <functional>
#include <thread>

namespace openscp::testsupport {

class IsolatedSettings final {
    public:
    IsolatedSettings() {
        if (!directory_.isValid())
            return;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                           directory_.path());
    }

    bool isValid() const { return directory_.isValid(); }
    QString path() const { return directory_.path(); }

    private:
    QTemporaryDir directory_;
};

inline bool
waitUntil(const std::function<bool()> &predicate,
          std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents();
        if (predicate())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    QCoreApplication::processEvents();
    return predicate();
}

inline bool spinUntil(const std::function<bool()> &predicate, int timeoutMs) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return predicate();
}

inline void flushUiEvents(int passes = 5) {
    for (int pass = 0; pass < passes; ++pass)
        QCoreApplication::processEvents(QEventLoop::AllEvents);
}

inline void drainThreadPool(int timeoutMs = 2000) {
    if (auto *pool = QThreadPool::globalInstance()) {
        pool->waitForDone(timeoutMs);
    }
}

} // namespace openscp::testsupport
