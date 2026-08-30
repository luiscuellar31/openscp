#include "HostKeyPromptCoordinator.hpp"
#include "SessionHealthMonitor.hpp"
#include "TestHarness.hpp"

#include <QCoreApplication>
#include <QVector>

#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using HostPrompt = openscpui::HostKeyPromptCoordinator::Prompt;

HostPrompt prompt(QString host, quint16 port = 22) {
    return {std::move(host), port, QStringLiteral("ssh-ed25519"),
            QStringLiteral("SHA256:test"), true};
}

bool waitForPresentations(std::condition_variable &condition, std::mutex &mutex,
                          const std::vector<HostPrompt> &presented,
                          std::size_t expected) {
    std::unique_lock<std::mutex> lock(mutex);
    return condition.wait_for(lock, 2s,
                              [&] { return presented.size() >= expected; });
}

void testHostKeyAcceptanceAndRejection(TestContext &test) {
    openscpui::HostKeyPromptCoordinator coordinator;
    std::mutex mutex;
    std::condition_variable presentedChanged;
    std::vector<HostPrompt> presented;
    coordinator.setPresentPrompt([&](const HostPrompt &value) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            presented.push_back(value);
        }
        presentedChanged.notify_all();
    });

    auto accepted = std::async(std::launch::async, [&] {
        return coordinator.requestDecision(prompt(QStringLiteral("alpha")));
    });
    const bool firstPresented =
        waitForPresentations(presentedChanged, mutex, presented, 1);
    test.check(firstPresented && coordinator.hasPendingPrompt(),
               "host-key requests should publish their pending prompt");
    test.check(coordinator.resolve(true),
               "an active host-key prompt should accept one decision");
    test.check(accepted.get(), "accepted host keys should unblock as trusted");

    auto rejected = std::async(std::launch::async, [&] {
        return coordinator.requestDecision(prompt(QStringLiteral("beta")));
    });
    const bool secondPresented =
        waitForPresentations(presentedChanged, mutex, presented, 2);
    test.check(secondPresented && coordinator.resolve(false),
               "a later host-key prompt should accept rejection");
    test.check(!rejected.get() && !coordinator.hasPendingPrompt(),
               "rejected host keys should clear pending state");
    test.check(!coordinator.resolve(true),
               "decisions without an active prompt should be ignored");
}

void testHostKeyCancellation(TestContext &test) {
    openscpui::HostKeyPromptCoordinator coordinator;
    std::promise<void> presented;
    coordinator.setPresentPrompt(
        [&](const HostPrompt &) { presented.set_value(); });

    auto decision = std::async(std::launch::async, [&] {
        return coordinator.requestDecision(prompt(QStringLiteral("cancel")));
    });
    test.check(presented.get_future().wait_for(2s) == std::future_status::ready,
               "cancel fixture should reach presentation");
    coordinator.cancel();
    test.check(!decision.get() && !coordinator.hasPendingPrompt(),
               "cancellation should reject and wake a waiting connection");

    std::promise<void> presentedAgain;
    coordinator.setPresentPrompt(
        [&](const HostPrompt &) { presentedAgain.set_value(); });
    auto later = std::async(std::launch::async, [&] {
        return coordinator.requestDecision(prompt(QStringLiteral("later")));
    });
    test.check(presentedAgain.get_future().wait_for(2s) ==
                   std::future_status::ready,
               "cancellation should not poison later connection attempts");
    test.check(coordinator.resolve(true) && later.get(),
               "later host-key requests should remain usable");
}

void testConcurrentHostKeyPromptsAreSerialized(TestContext &test) {
    openscpui::HostKeyPromptCoordinator coordinator;
    std::mutex mutex;
    std::condition_variable presentedChanged;
    std::vector<HostPrompt> presented;
    coordinator.setPresentPrompt([&](const HostPrompt &value) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            presented.push_back(value);
        }
        presentedChanged.notify_all();
    });

    auto first = std::async(std::launch::async, [&] {
        return coordinator.requestDecision(prompt(QStringLiteral("first")));
    });
    test.check(waitForPresentations(presentedChanged, mutex, presented, 1),
               "the first concurrent prompt should be presented");
    auto second = std::async(std::launch::async, [&] {
        return coordinator.requestDecision(prompt(QStringLiteral("second")));
    });
    std::this_thread::sleep_for(20ms);
    {
        std::lock_guard<std::mutex> lock(mutex);
        test.check(presented.size() == 1,
                   "a second prompt must not overwrite the active prompt");
    }

    test.check(coordinator.resolve(true) && first.get(),
               "the first serialized prompt should resolve independently");
    test.check(waitForPresentations(presentedChanged, mutex, presented, 2),
               "the queued prompt should be presented after the first");
    const auto pending = coordinator.pendingPrompt();
    test.check(pending && pending->host == QStringLiteral("second"),
               "queued prompts should retain their own connection data");
    test.check(coordinator.resolve(false) && !second.get(),
               "the second serialized prompt should retain its decision");
}

void testSessionHealthLifecycleAndOverlappingProbes(TestContext &test) {
    openscpui::SessionHealthMonitor monitor;
    quint64 nextJobId = 0;
    QVector<QString> submittedPaths;
    QVector<quint64> canceledJobs;
    QVector<openscpui::SessionHealthMonitor::ProbeContext> successes;
    QVector<QString> failures;

    openscpui::SessionHealthMonitor::Callbacks callbacks;
    callbacks.canProbe = [] { return true; };
    callbacks.probePath = [] { return QStringLiteral("/team"); };
    callbacks.submitProbe = [&](const QString &path) {
        submittedPaths.push_back(path);
        return ++nextJobId;
    };
    callbacks.cancelProbe = [&](quint64 jobId) {
        canceledJobs.push_back(jobId);
    };
    callbacks.periodicReason = [] { return QStringLiteral("periodic-test"); };
    callbacks.resumeReason = [](qint64 seconds) {
        return QStringLiteral("resume-%1").arg(seconds);
    };
    callbacks.probeSucceeded = [&](const auto &context) {
        successes.push_back(context);
    };
    callbacks.probeFailed = [&](const auto &, const QString &error) {
        failures.push_back(error);
    };
    monitor.setCallbacks(std::move(callbacks));
    monitor.setInterval(1);
    test.check(monitor.interval() == 60'000,
               "health intervals should enforce a safe minimum");

    monitor.start();
    monitor.recordActivity(1'000);
    test.check(!monitor.requestProbe(QStringLiteral("recent"), false, 30'000),
               "recent successful activity should suppress periodic probes");
    test.check(monitor.requestProbe(QStringLiteral("forced"), true, 30'000) &&
                   monitor.hasProbeInFlight() && monitor.activeJobId() == 1,
               "forced probes should start with a stable job ID");
    test.check(!monitor.requestProbe(QStringLiteral("overlap"), true, 30'001),
               "overlapping health probes should be rejected");
    test.check(!monitor.completeProbe(
                   999, openscpui::SessionHealthMonitor::ProbeOutcome::Failed,
                   QStringLiteral("stale"), 30'002),
               "stale probe completions should be ignored");
    test.check(monitor.completeProbe(
                   1, openscpui::SessionHealthMonitor::ProbeOutcome::Succeeded,
                   {}, 30'002) &&
                   successes.size() == 1 && !monitor.hasProbeInFlight(),
               "successful probes should record activity and clear state");

    test.check(monitor.requestProbe(QStringLiteral("failure"), false, 100'003),
               "idle sessions should permit a later periodic probe");
    test.check(monitor.completeProbe(
                   2, openscpui::SessionHealthMonitor::ProbeOutcome::Failed,
                   QStringLiteral("connection reset"), 100'004) &&
                   failures ==
                       QVector<QString>{QStringLiteral("connection reset")},
               "failed probes should report their transport error once");

    test.check(
        monitor.requestProbe(QStringLiteral("disconnect"), true, 100'005),
        "disconnect fixture should start an active probe");
    monitor.stop();
    test.check(!monitor.isRunning() && !monitor.hasProbeInFlight() &&
                   canceledJobs == QVector<quint64>{3},
               "stopping health monitoring should cancel its active probe");
    test.check(submittedPaths.size() == 3 &&
                   submittedPaths.front() == QStringLiteral("/team"),
               "health probes should use the current remote path callback");
}

void testSessionHealthResumeProbe(TestContext &test) {
    openscpui::SessionHealthMonitor monitor;
    QString submittedReason;
    quint64 nextJobId = 40;
    openscpui::SessionHealthMonitor::Callbacks callbacks;
    callbacks.canProbe = [] { return true; };
    callbacks.submitProbe = [&](const QString &) { return ++nextJobId; };
    callbacks.resumeReason = [&](qint64 seconds) {
        submittedReason = QStringLiteral("resume-%1").arg(seconds);
        return submittedReason;
    };
    monitor.setCallbacks(std::move(callbacks));
    monitor.start();

    monitor.handleApplicationState(Qt::ApplicationInactive, 1'000);
    monitor.handleApplicationState(Qt::ApplicationActive, 50'000);
    test.check(!monitor.hasProbeInFlight(),
               "short application inactivity should not probe the session");
    monitor.handleApplicationState(Qt::ApplicationSuspended, 100'000);
    monitor.handleApplicationState(Qt::ApplicationActive, 161'000);
    test.check(monitor.hasProbeInFlight() && monitor.activeJobId() == 41 &&
                   submittedReason == QStringLiteral("resume-61"),
               "long resume intervals should trigger one forced probe");
    test.check(monitor.completeProbe(
                   41, openscpui::SessionHealthMonitor::ProbeOutcome::Canceled),
               "canceled resume probes should clear overlap protection");
    monitor.stop();
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    TestContext test;
    testHostKeyAcceptanceAndRejection(test);
    testHostKeyCancellation(test);
    testConcurrentHostKeyPromptsAreSerialized(test);
    testSessionHealthLifecycleAndOverlappingProbes(test);
    testSessionHealthResumeProbe(test);
    if (test.failures == 0)
        std::cout << "All connection coordinator tests passed\n";
    return test.failures == 0 ? 0 : 1;
}
