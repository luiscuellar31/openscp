#include "TestHarness.hpp"
#include "TransferUiController.hpp"

#include <QCoreApplication>

#include <iostream>

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

void testCompletionIsNotRepeated(TestContext &test) {
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

void testRemoteRootBoundaries(TestContext &test) {
    openscpui::TransferUiController controller;
    const QVector<TransferTask> tasks{
        completedUpload(2, QStringLiteral("/teammate/report.txt"))};
    const auto update =
        controller.observe(tasks, {}, true, QStringLiteral("/team"));
    test.check(!update.scheduleRemoteRefresh,
               "similar path prefixes must not cross directory boundaries");
}

void testBatchNotification(TestContext &test) {
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

void testResetAllowsFreshSessionEffects(TestContext &test) {
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

void testInitializationAndRemovalDeltas(TestContext &test) {
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

void testFrequentDeltasStayIncremental(TestContext &test) {
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

} // namespace

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    TestContext test;
    testCompletionIsNotRepeated(test);
    testRemoteRootBoundaries(test);
    testBatchNotification(test);
    testResetAllowsFreshSessionEffects(test);
    testInitializationAndRemovalDeltas(test);
    testFrequentDeltasStayIncremental(test);
    if (test.failures == 0)
        std::cout << "All transfer UI controller tests passed\n";
    return test.failures == 0 ? 0 : 1;
}
