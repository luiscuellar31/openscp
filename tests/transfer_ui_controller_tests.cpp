#include "TransferUiController.hpp"

#include <QCoreApplication>

#include <iostream>

namespace {

struct TestContext {
    int failures = 0;

    void check(bool condition, const char *message) {
        if (condition)
            return;
        ++failures;
        std::cerr << "[FAIL] " << message << '\n';
    }
};

TransferTask completedUpload(quint64 id, const QString &destination) {
    TransferTask task;
    task.taskId = id;
    task.type = TransferTask::Type::Upload;
    task.src = QStringLiteral("/local/report.txt");
    task.dst = destination;
    task.status = TransferTask::Status::Done;
    return task;
}

void testCompletionIsNotRepeated(TestContext &test) {
    openscpui::TransferUiController controller;
    const QVector<TransferTask> tasks{
        completedUpload(1, QStringLiteral("/team/report.txt"))};
    const auto first = controller.observe(tasks, true, QStringLiteral("/team"));
    test.check(first.scheduleRemoteRefresh,
               "a completed upload in the visible folder should refresh it");
    test.check(!first.completionMessage.isEmpty(),
               "a newly completed transfer should produce one notification");

    controller.completeScheduledRefresh();
    const auto repeated =
        controller.observe(tasks, true, QStringLiteral("/team"));
    test.check(!repeated.scheduleRemoteRefresh &&
                   repeated.completionMessage.isEmpty(),
               "unchanged terminal tasks should not repeat UI effects");
}

void testRemoteRootBoundaries(TestContext &test) {
    openscpui::TransferUiController controller;
    const QVector<TransferTask> tasks{
        completedUpload(2, QStringLiteral("/teammate/report.txt"))};
    const auto update =
        controller.observe(tasks, true, QStringLiteral("/team"));
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
    const auto update = controller.observe(tasks, false, {});
    test.check(update.completionMessage.contains(QStringLiteral("2")),
               "simultaneous completions should be summarized once");
    test.check(!update.scheduleRemoteRefresh,
               "local mode should not schedule a remote refresh");
}

void testResetAllowsFreshSessionEffects(TestContext &test) {
    openscpui::TransferUiController controller;
    const QVector<TransferTask> tasks{
        completedUpload(5, QStringLiteral("/a.txt"))};
    (void)controller.observe(tasks, true, QStringLiteral("/"));
    controller.reset();
    const auto update = controller.observe(tasks, true, QStringLiteral("/"));
    test.check(update.scheduleRemoteRefresh &&
                   !update.completionMessage.isEmpty(),
               "session reset should discard prior UI observation state");
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    TestContext test;
    testCompletionIsNotRepeated(test);
    testRemoteRootBoundaries(test);
    testBatchNotification(test);
    testResetAllowsFreshSessionEffects(test);
    if (test.failures == 0)
        std::cout << "All transfer UI controller tests passed\n";
    return test.failures == 0 ? 0 : 1;
}
