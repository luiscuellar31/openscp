// Transfer queue dialog tests: what the table shows as the queue changes.
#include "QtTestSupport.hpp"
#include "TestHarness.hpp"
#include "logic/transfers/TransferManager.hpp"
#include "widgets/dialogs/TransferQueueDialog.hpp"

#include <QAbstractItemModel>
#include <QApplication>
#include <QClipboard>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QPersistentModelIndex>
#include <QSortFilterProxyModel>
#include <QTableView>

#include <algorithm>
#include <iostream>

namespace {

using openscp::testsupport::flushUiEvents;
using openscp::testsupport::waitUntil;

// The destination column; its logical id is fixed by the dialog.
constexpr int kDestinationColumn = 3;

QStringList destinations(const QAbstractItemModel *model) {
    QStringList result;
    for (int row = 0; row < model->rowCount(); ++row)
        result.push_back(
            model->index(row, kDestinationColumn).data().toString());
    return result;
}

QString badgeText(const QWidget *dialog, const QString &objectName) {
    const auto *label = dialog->findChild<QLabel *>(objectName);
    return label ? label->text() : QString();
}

OPENSCP_TEST(testQueueSummaryFollowsTaskStates, test) {
    TransferManager manager;
    manager.setSessionIdentity(QStringLiteral("test-session"));
    TransferBatchOptions batch;
    batch.sessionKey = QStringLiteral("test-session");

    TransferQueueDialog dialog(&manager);
    dialog.resize(dialog.minimumSize());
    dialog.show();
    flushUiEvents();

    QVector<quint64> ids;
    for (int index = 0; index < 4; ++index) {
        ids.push_back(manager.enqueueRemoteDelete(
            QStringLiteral("/summary-%1").arg(index), false, batch));
    }
    test.check(waitUntil([&] {
                   return badgeText(&dialog,
                                    QStringLiteral("transferBadgeTotal")) ==
                          TransferQueueDialog::tr("Total: %1").arg(4);
               }),
               "the summary should count the tasks that were queued");

    manager.cancelTask(ids[0]);
    manager.cancelTask(ids[1]);
    test.check(waitUntil([&] {
                   return badgeText(&dialog,
                                    QStringLiteral("transferBadgeCanceled")) ==
                              TransferQueueDialog::tr("Canceled: %1").arg(2) &&
                          badgeText(&dialog,
                                    QStringLiteral("transferBadgeTotal")) ==
                              TransferQueueDialog::tr("Total: %1").arg(4);
               }),
               "the summary should follow the state of each task");

    manager.removeTasks({ids[0], ids[1]});
    test.check(waitUntil([&] {
                   return badgeText(&dialog,
                                    QStringLiteral("transferBadgeTotal")) ==
                              TransferQueueDialog::tr("Total: %1").arg(2) &&
                          badgeText(&dialog,
                                    QStringLiteral("transferBadgeCanceled")) ==
                              TransferQueueDialog::tr("Canceled: %1").arg(0);
               }),
               "removed tasks should leave the summary");
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

    // Rows added after a removal have to reach the table's index too.
    const quint64 late = manager.enqueueRemoteDelete(
        QStringLiteral("/queued-late"), false, batch);
    const quint64 later = manager.enqueueRemoteDelete(
        QStringLiteral("/queued-later"), false, batch);
    flushUiEvents();
    manager.removeTasks({late});
    flushUiEvents();
    test.check(destinations(table->model()) ==
                   QStringList{QStringLiteral("/queued-later")},
               "the table should still find the rows it added later");
    test.check(manager.taskSnapshot(later).has_value(),
               "removing by id should not touch the other task");
}

OPENSCP_TEST(testSelectedActionsUseTaskSnapshots, test) {
    TransferManager manager;
    manager.setSessionIdentity(QStringLiteral("test-session"));
    TransferBatchOptions batch;
    batch.sessionKey = QStringLiteral("test-session");
    const quint64 first = manager.enqueueRemoteDelete(
        QStringLiteral("/selected-first"), false, batch);
    const quint64 unselected = manager.enqueueRemoteDelete(
        QStringLiteral("/unselected"), false, batch);
    const quint64 last = manager.enqueueRemoteDelete(
        QStringLiteral("/selected-last"), false, batch);

    TransferQueueDialog dialog(&manager);
    dialog.show();
    flushUiEvents();
    auto *table =
        dialog.findChild<QTableView *>(QStringLiteral("transferQueueTable"));
    test.check(table && table->model() && table->model()->rowCount() == 3,
               "all queued tasks should appear for selection");
    if (!table || !table->model() || table->model()->rowCount() != 3)
        return;

    auto *selection = table->selectionModel();
    selection->select(table->model()->index(0, 0),
                      QItemSelectionModel::Select | QItemSelectionModel::Rows);
    selection->select(table->model()->index(2, 0),
                      QItemSelectionModel::Select | QItemSelectionModel::Rows);

    test.check(QMetaObject::invokeMethod(&dialog, "onCopyDestinationPath"),
               "copy selected paths should be callable");
    test.check(QGuiApplication::clipboard()->text() ==
                   QStringLiteral("/selected-first\n/selected-last"),
               "copy should use only selected task destinations in row order");

    test.check(QMetaObject::invokeMethod(&dialog, "onPauseSelected"),
               "pause selected should be callable");
    const auto pausedFirst = manager.taskSnapshot(first);
    const auto pausedLast = manager.taskSnapshot(last);
    const auto untouched = manager.taskSnapshot(unselected);
    test.check(pausedFirst && pausedLast && untouched &&
                   pausedFirst->status == TransferTask::Status::Paused &&
                   pausedLast->status == TransferTask::Status::Paused &&
                   untouched->status != TransferTask::Status::Paused,
               "pause should affect only selected tasks");

    test.check(QMetaObject::invokeMethod(&dialog, "onStopSelected"),
               "cancel selected should be callable");
    const auto canceledFirst = manager.taskSnapshot(first);
    const auto canceledLast = manager.taskSnapshot(last);
    const auto stillUntouched = manager.taskSnapshot(unselected);
    test.check(canceledFirst && canceledLast && stillUntouched &&
                   canceledFirst->status == TransferTask::Status::Canceled &&
                   canceledLast->status == TransferTask::Status::Canceled &&
                   stillUntouched->status != TransferTask::Status::Canceled,
               "cancel should affect only selected tasks");
}

OPENSCP_TEST(testDiscontinuousRemovalPreservesIndexesAndSelection, test) {
    TransferManager manager;
    TransferBatchOptions batch;
    QVector<quint64> ids;
    for (int row = 0; row < 48; ++row)
        ids.push_back(manager.enqueueRemoteDelete(
            QStringLiteral("/bulk-%1").arg(row), false, batch));

    TransferQueueDialog dialog(&manager);
    auto *table =
        dialog.findChild<QTableView *>(QStringLiteral("transferQueueTable"));
    auto *proxy =
        table ? qobject_cast<QSortFilterProxyModel *>(table->model()) : nullptr;
    test.check(proxy && proxy->rowCount() == 48,
               "bulk-removal table should expose all tasks");
    if (!proxy || proxy->rowCount() != 48)
        return;
    auto *source = proxy->sourceModel();
    auto *selection = table->selectionModel();
    selection->select(QItemSelection(proxy->index(10, 0), proxy->index(20, 10)),
                      QItemSelectionModel::Select);
    selection->setCurrentIndex(proxy->index(19, kDestinationColumn),
                               QItemSelectionModel::NoUpdate);
    QPersistentModelIndex sourceSurvivor(source->index(19, kDestinationColumn));
    QPersistentModelIndex proxySurvivor(proxy->index(19, kDestinationColumn));
    QPersistentModelIndex sourceRemoved(source->index(10, kDestinationColumn));
    QPersistentModelIndex proxyRemoved(proxy->index(10, kDestinationColumn));
    QPersistentModelIndex createdDuringNotification;
    int layouts = 0;
    int resets = 0;
    int removals = 0;
    QObject::connect(
        source, &QAbstractItemModel::layoutAboutToBeChanged, &dialog, [&] {
            test.check(source->rowCount() == 48,
                       "layout notification must precede compaction");
            createdDuringNotification = source->index(45, kDestinationColumn);
        });
    QObject::connect(source, &QAbstractItemModel::layoutChanged, &dialog,
                     [&] { ++layouts; });
    QObject::connect(source, &QAbstractItemModel::modelReset, &dialog,
                     [&] { ++resets; });
    QObject::connect(source, &QAbstractItemModel::rowsRemoved, &dialog,
                     [&] { ++removals; });

    QVector<quint64> removed{ids[0], ids[47]};
    QStringList expected;
    for (int row = 1; row < 47; ++row) {
        if (row % 2 == 0)
            removed.push_back(ids[row]);
        else
            expected.push_back(QStringLiteral("/bulk-%1").arg(row));
    }
    manager.removeTasks(removed);
    flushUiEvents();
    test.check(
        layouts == 1 && resets == 0 && removals == 0,
        "discontinuous removal should use one layout change without reset");
    test.check(destinations(source) == expected &&
                   destinations(proxy) == expected,
               "source and proxy must retain surviving tasks in order");
    test.check(sourceSurvivor.isValid() && proxySurvivor.isValid() &&
                   sourceSurvivor.row() == 9 && proxySurvivor.row() == 9 &&
                   sourceSurvivor.data().toString() ==
                       QStringLiteral("/bulk-19") &&
                   proxySurvivor.data() == sourceSurvivor.data(),
               "persistent source and proxy indexes must retain task identity");
    test.check(!sourceRemoved.isValid() && !proxyRemoved.isValid(),
               "removed tasks must invalidate persistent indexes");
    test.check(createdDuringNotification.isValid() &&
                   createdDuringNotification.data().toString() ==
                       QStringLiteral("/bulk-45"),
               "indexes created by layout observers must also be remapped");
    QStringList selected;
    for (const auto &index : selection->selectedRows(kDestinationColumn))
        selected.push_back(index.data().toString());
    std::sort(selected.begin(), selected.end());
    test.check(selected == QStringList{"/bulk-11", "/bulk-13", "/bulk-15",
                                       "/bulk-17", "/bulk-19"},
               "selection ranges must retain only their surviving tasks");
    test.check(selection->currentIndex().data().toString() ==
                   QStringLiteral("/bulk-19"),
               "current surviving task must remain current");

    test.check(QMetaObject::invokeMethod(&dialog, "onTasksRemoved",
                                         Q_ARG(QVector<quint64>, removed)),
               "stale removal notification should be accepted");
    test.check(layouts == 1 && destinations(source) == expected,
               "already absent ids must produce no structural notification");
    manager.pauseTask(ids[19]);
    flushUiEvents();
    test.check(source->index(9, 4).data().toString() ==
                   TransferQueueDialog::tr("Paused"),
               "later updates must find the compacted row by task id");
    test.check(waitUntil([&] {
                   return badgeText(&dialog, "transferBadgeTotal") ==
                          TransferQueueDialog::tr("Total: %1").arg(23);
               }),
               "summary counts must match the compacted table");
}

OPENSCP_TEST(benchmarkDiscontinuousRemoval, test) {
    if (!qEnvironmentVariableIsSet("OPENSCP_BENCH_QUEUE_MODEL"))
        return;
    for (int size : {2000, 4000, 8000}) {
        TransferManager manager;
        TransferBatchOptions batch;
        QVector<quint64> removed;
        for (int row = 0; row < size; ++row) {
            const auto id = manager.enqueueRemoteDelete(
                QStringLiteral("/benchmark-%1").arg(row), false, batch);
            if (row % 2 == 0)
                removed.push_back(id);
        }
        TransferQueueDialog dialog(&manager);
        QElapsedTimer timer;
        timer.start();
        // Isolate model/proxy work from TransferManager's queue compaction.
        test.check(QMetaObject::invokeMethod(&dialog, "onTasksRemoved",
                                             Q_ARG(QVector<quint64>, removed)),
                   "benchmark removal should invoke the dialog update");
        std::cout << "BENCH discontinuous_model_" << size
                  << "_us=" << timer.nsecsElapsed() / 1000 << '\n';
    }
}

OPENSCP_TEST(testBulkRemovalThroughSortedFilterAndFullClear, test) {
    TransferManager manager;
    TransferBatchOptions batch;
    QVector<quint64> ids;
    for (int row = 0; row < 24; ++row) {
        ids.push_back(manager.enqueueRemoteDelete(
            QStringLiteral("/filtered-%1").arg(row, 2, 10, QChar('0')), false,
            batch));
        if (row % 2 != 0)
            manager.cancelTask(ids.back());
    }
    TransferQueueDialog dialog(&manager);
    auto *table =
        dialog.findChild<QTableView *>(QStringLiteral("transferQueueTable"));
    auto *proxy =
        table ? qobject_cast<QSortFilterProxyModel *>(table->model()) : nullptr;
    test.check(proxy != nullptr, "filtered queue should expose its proxy");
    if (!proxy)
        return;
    test.check(
        QMetaObject::invokeMethod(&dialog, "onFilterChanged", Q_ARG(int, 4)),
        "canceled filter should be callable");
    proxy->sort(kDestinationColumn, Qt::DescendingOrder);
    test.check(proxy->rowCount() == 12 &&
                   proxy->index(0, kDestinationColumn).data().toString() ==
                       QStringLiteral("/filtered-23"),
               "proxy should filter statuses and reverse task order");
    if (proxy->rowCount() != 12)
        return;
    auto *source = proxy->sourceModel();
    auto *selection = table->selectionModel();
    selection->select(QItemSelection(proxy->index(0, 0), proxy->index(2, 10)),
                      QItemSelectionModel::Select);
    selection->setCurrentIndex(proxy->index(0, kDestinationColumn),
                               QItemSelectionModel::NoUpdate);
    QPersistentModelIndex survivor(proxy->index(1, kDestinationColumn));
    QPersistentModelIndex removedIndex(proxy->index(0, kDestinationColumn));
    int layouts = 0;
    int rowRemovals = 0;
    QObject::connect(source, &QAbstractItemModel::layoutChanged, &dialog,
                     [&] { ++layouts; });
    QObject::connect(source, &QAbstractItemModel::rowsRemoved, &dialog,
                     [&](const QModelIndex &, int first, int last) {
                         ++rowRemovals;
                         test.check(
                             first == 0 && last == 9 && source->rowCount() == 0,
                             "full clear should announce one valid range");
                     });
    QVector<quint64> removed{ids[19], ids[23]};
    for (int row = 0; row < 24; row += 2)
        removed.push_back(ids[row]);
    QVector<quint64> notification = removed;
    notification += QVector<quint64>{ids[19], ids[0], 0};
    // Exercise duplicate/unknown IDs in a notification, then bring the manager
    // into the same state; its subsequent notification must be a no-op.
    test.check(QMetaObject::invokeMethod(&dialog, "onTasksRemoved",
                                         Q_ARG(QVector<quint64>, notification)),
               "duplicate removal ids should be accepted");
    manager.removeTasks(removed);
    flushUiEvents();
    QStringList expected;
    QVector<quint64> remaining;
    for (int row = 21; row >= 1; row -= 2) {
        if (row == 19)
            continue;
        expected.push_back(
            QStringLiteral("/filtered-%1").arg(row, 2, 10, QChar('0')));
        remaining.push_back(ids[row]);
    }
    test.check(layouts == 1 && rowRemovals == 0 &&
                   destinations(proxy) == expected,
               "bulk removal must update a sorted filter with one layout");
    test.check(
        survivor.isValid() && survivor.row() == 0 &&
            survivor.data().toString() == QStringLiteral("/filtered-21") &&
            !removedIndex.isValid() && !selection->currentIndex().isValid(),
        "surviving identity must persist and removed current task must clear");
    const auto selected = selection->selectedRows(kDestinationColumn);
    test.check(selected.size() == 1 && selected[0].data().toString() ==
                                           QStringLiteral("/filtered-21"),
               "filtered selection must drop removed tasks without selecting "
               "neighbors");

    manager.removeTasks(remaining);
    flushUiEvents();
    test.check(source->rowCount() == 0 && proxy->rowCount() == 0 &&
                   table->verticalHeader()->count() == 0 &&
                   !survivor.isValid() && selection->selectedRows().isEmpty() &&
                   layouts == 1 && rowRemovals == 1,
               "full contiguous clear must empty view, selection and indexes");
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
    const int result = harness.run();
    openscp::testsupport::drainThreadPool();
    return result;
}
