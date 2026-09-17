// Transfer queue dialog tests: what the table shows as the queue changes.
#include "QtTestSupport.hpp"
#include "TestHarness.hpp"
#include "logic/transfers/TransferManager.hpp"
#include "widgets/dialogs/TransferQueueDialog.hpp"

#include <QAbstractItemModel>
#include <QApplication>
#include <QTableView>

#include <iostream>

namespace {

using openscp::testsupport::flushUiEvents;

// The destination column; its logical id is fixed by the dialog.
constexpr int kDestinationColumn = 3;

QStringList destinations(const QAbstractItemModel *model) {
    QStringList result;
    for (int row = 0; row < model->rowCount(); ++row)
        result.push_back(
            model->index(row, kDestinationColumn).data().toString());
    return result;
}

OPENSCP_TEST(testQueueTableFollowsRemovedTasks, test) {
    TransferManager manager;
    manager.setSessionIdentity(QStringLiteral("test-session"));
    TransferBatchOptions batch;
    batch.sessionKey = QStringLiteral("test-session");
    QVector<quint64> ids;
    for (int index = 0; index < 6; ++index) {
        ids.push_back(manager.enqueueRemoteDelete(
            QStringLiteral("/queued-%1").arg(index), false, batch));
    }

    TransferQueueDialog dialog(&manager);
    dialog.resize(dialog.minimumSize());
    dialog.show();
    flushUiEvents();

    auto *table =
        dialog.findChild<QTableView *>(QStringLiteral("transferQueueTable"));
    test.check(table != nullptr && table->model() != nullptr,
               "the transfer table should be discoverable");
    if (!table || !table->model())
        return;
    test.check(table->model()->rowCount() == 6,
               "the table should show the queued tasks");

    // A contiguous run and a separate row, as a selection usually removes.
    manager.removeTasks({ids[1], ids[2], ids[3], ids[5]});
    flushUiEvents();
    test.check(destinations(table->model()) ==
                   QStringList{QStringLiteral("/queued-0"),
                               QStringLiteral("/queued-4")},
               "the table should keep the rows the queue kept, in order");

    manager.removeTasks({ids[0], ids[4]});
    flushUiEvents();
    test.check(table->model()->rowCount() == 0,
               "removing the rest should empty the table");
}

} // namespace

int main(int argc, char **argv) {
    QApplication application(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("OpenSCP-tests"));
    QApplication::setApplicationName(
        QStringLiteral("transfer-queue-dialog-tests"));

    openscp::testsupport::IsolatedSettings settingsRoot;
    if (!settingsRoot.isValid()) {
        std::cerr << "[FAIL] could not create isolated settings directory\n";
        return 1;
    }
    openscp::test::TestHarness harness("Transfer queue dialog");
    return harness.run();
}
