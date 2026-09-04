#include "TestHarness.hpp"
#include "logic/transfers/TransferUiController.hpp"

#include <QCoreApplication>

namespace {

TransferTask completedUpload(quint64 id, const QString &destination) {
    TransferTask task;
    task.taskId = id;
    task.type = TransferTask::Type::Upload;
    task.src = QStringLiteral("/local/report.txt");
    task.dst = destination;
    task.status = TransferTask::Status::Done;
    return task;
}

TransferTask queuedUpload(quint64 id, const QString &destination) {
    TransferTask task = completedUpload(id, destination);
    task.status = TransferTask::Status::Queued;
    return task;
}

TransferTask downloadWithStatus(quint64 id, const QString &destination,
                                TransferTask::Status status) {
    TransferTask task;
    task.taskId = id;
    task.type = TransferTask::Type::Download;
    task.src = QStringLiteral("/remote/report.txt");
    task.dst = destination;
    task.status = status;
    return task;
}

OPENSCP_TEST(testCompletionIsNotRepeated, test) {
    openscpui::TransferUiController controller;
    controller.initialize(
        {queuedUpload(1, QStringLiteral("/team/report.txt"))});
    const QVector<TransferTask> completion{
        completedUpload(1, QStringLiteral("/team/report.txt"))};
    const auto first =
        controller.observe(completion, {}, true, QStringLiteral("/team"));
    test.check(first.scheduleRemoteRefresh,
               "a completed upload in the visible folder should refresh it");
    test.check(!first.completionMessage.isEmpty(),
               "a newly completed transfer should produce one notification");

    controller.completeScheduledRefresh();
    const auto repeated =
        controller.observe(completion, {}, true, QStringLiteral("/team"));
    test.check(!repeated.scheduleRemoteRefresh &&
                   repeated.completionMessage.isEmpty(),
               "unchanged terminal tasks should not repeat UI effects");
}

OPENSCP_TEST(testRemoteRootBoundaries, test) {
    openscpui::TransferUiController controller;
    const QVector<TransferTask> tasks{
        completedUpload(2, QStringLiteral("/teammate/report.txt"))};
    const auto update =
        controller.observe(tasks, {}, true, QStringLiteral("/team"));
    test.check(!update.scheduleRemoteRefresh,
               "similar path prefixes must not cross directory boundaries");
}

OPENSCP_TEST(testBatchNotification, test) {
    openscpui::TransferUiController controller;
    TransferTask download;
    download.taskId = 4;
    download.type = TransferTask::Type::Download;
    download.dst = QStringLiteral("/local/a.txt");
    download.status = TransferTask::Status::Done;
    const QVector<TransferTask> tasks{
        completedUpload(3, QStringLiteral("/remote/a.txt")), download};
    const auto update = controller.observe(tasks, {}, false, {});
    test.check(update.completionMessage.contains(QStringLiteral("2")),
               "simultaneous completions should be summarized once");
    test.check(!update.scheduleRemoteRefresh,
               "local mode should not schedule a remote refresh");
}

OPENSCP_TEST(testResetAllowsFreshSessionEffects, test) {
    openscpui::TransferUiController controller;
    const QVector<TransferTask> tasks{
        completedUpload(5, QStringLiteral("/a.txt"))};
    (void)controller.observe(tasks, {}, true, QStringLiteral("/"));
    controller.reset();
    const auto update =
        controller.observe(tasks, {}, true, QStringLiteral("/"));
    test.check(update.scheduleRemoteRefresh &&
                   !update.completionMessage.isEmpty(),
               "session reset should discard prior UI observation state");
}

OPENSCP_TEST(testInitializationAndRemovalDeltas, test) {
    openscpui::TransferUiController controller;
    const TransferTask completed =
        completedUpload(6, QStringLiteral("/existing.txt"));
    controller.initialize({completed});
    const auto unchanged =
        controller.observe({completed}, {}, true, QStringLiteral("/"));
    test.check(!unchanged.scheduleRemoteRefresh &&
                   unchanged.completionMessage.isEmpty(),
               "initial terminal tasks should not replay UI effects");

    (void)controller.observe({}, {completed.taskId}, true, QStringLiteral("/"));
    const auto reinserted =
        controller.observe({completed}, {}, true, QStringLiteral("/"));
    test.check(reinserted.scheduleRemoteRefresh &&
                   !reinserted.completionMessage.isEmpty(),
               "removal deltas should release per-task observation state");
}

OPENSCP_TEST(testFrequentDeltasStayIncremental, test) {
    openscpui::TransferUiController controller;
    QVector<TransferTask> initial;
    initial.reserve(10'000);
    for (quint64 id = 1; id <= 10'000; ++id)
        initial.push_back(queuedUpload(id, QStringLiteral("/bulk/%1").arg(id)));
    controller.initialize(initial);

    TransferTask changing = initial.back();
    changing.status = TransferTask::Status::Running;
    for (int updateIndex = 0; updateIndex < 1'000; ++updateIndex) {
        changing.progress = updateIndex % 100;
        const auto update =
            controller.observe({changing}, {}, true, QStringLiteral("/bulk"));
        test.check(update.completionMessage.isEmpty() &&
                       !update.scheduleRemoteRefresh,
                   "non-terminal deltas should not produce UI effects");
    }
    changing.status = TransferTask::Status::Done;
    const auto completed =
        controller.observe({changing}, {}, true, QStringLiteral("/bulk"));
    test.check(
        completed.scheduleRemoteRefresh &&
            !completed.completionMessage.isEmpty(),
        "one terminal delta should complete independently of queue size");
}

OPENSCP_TEST(testDownloadOpenIntentFollowsTaskLifecycle, test) {
    openscpui::TransferUiController controller;
    const QString localPath = QStringLiteral("/local/report.txt");
    controller.openDownloadWhenCompleted(20, localPath);

    const auto running = controller.observe(
        {downloadWithStatus(20, localPath, TransferTask::Status::Running)}, {},
        false, {});
    test.check(running.completedDownloadPathsToOpen.isEmpty(),
               "an active download must not open prematurely");

    const auto completed = controller.observe(
        {downloadWithStatus(20, localPath, TransferTask::Status::Done)}, {},
        false, {});
    test.check(completed.completedDownloadPathsToOpen == QStringList{localPath},
               "a successful tracked task should request opening once");

    const auto repeated = controller.observe(
        {downloadWithStatus(20, localPath, TransferTask::Status::Done)}, {},
        false, {});
    test.check(repeated.completedDownloadPathsToOpen.isEmpty(),
               "a completed task must not request opening twice");
}

OPENSCP_TEST(testDownloadOpenIntentClearsOnFailureRemovalAndReset, test) {
    openscpui::TransferUiController controller;
    const QString localPath = QStringLiteral("/local/report.txt");

    controller.openDownloadWhenCompleted(21, localPath);
    const auto failed = controller.observe(
        {downloadWithStatus(21, localPath, TransferTask::Status::Error)}, {},
        false, {});
    test.check(failed.completedDownloadPathsToOpen.isEmpty(),
               "failed downloads must clear intent without opening");

    controller.openDownloadWhenCompleted(22, localPath);
    (void)controller.observe({}, {22}, false, {});
    const auto removedThenCompleted = controller.observe(
        {downloadWithStatus(22, localPath, TransferTask::Status::Done)}, {},
        false, {});
    test.check(removedThenCompleted.completedDownloadPathsToOpen.isEmpty(),
               "removed tasks must release their pending open intent");

    controller.openDownloadWhenCompleted(23, localPath);
    controller.reset();
    const auto resetThenCompleted = controller.observe(
        {downloadWithStatus(23, localPath, TransferTask::Status::Done)}, {},
        false, {});
    test.check(resetThenCompleted.completedDownloadPathsToOpen.isEmpty(),
               "reset must not retain open intent across sessions");
}

} // namespace

int main(int argc, char **argv) {
    openscp::test::TestHarness harness("transfer UI controller");
    return harness.runWithApplication<QCoreApplication>(argc, argv);
}
