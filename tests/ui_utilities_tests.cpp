#include "TestHarness.hpp"
#include "logic/common/MainWindowSharedUtils.hpp"
#include "logic/common/RowSelection.hpp"
#include "logic/common/UiFormatters.hpp"
#include "logic/navigation/RemotePath.hpp"
#include "logic/remote/RemoteModel.hpp"

#include <QCoreApplication>

#include <algorithm>
#include <initializer_list>
#include <string>
#include <vector>

namespace {

OPENSCP_TEST(testRemotePaths, test) {
    test.check(normalizeRemotePath(QString()) == QStringLiteral("/"),
               "an empty remote path should normalize to root");
    test.check(normalizeRemotePath(QStringLiteral("folder\\child")) ==
                   QStringLiteral("/folder/child"),
               "remote paths should normalize both separator styles");
    test.check(normalizeRemotePath(QStringLiteral("/one/./two/../three")) ==
                   QStringLiteral("/one/three"),
               "dot segments should be resolved");
    test.check(normalizeRemotePath(QStringLiteral("../../outside")) ==
                   QStringLiteral("/outside"),
               "remote paths should remain confined to the logical root");
    test.check(joinRemotePath(QStringLiteral("/base"), QStringLiteral("a/b")) ==
                   QStringLiteral("/base/a/b"),
               "joining should accept a safe relative path");
    test.check(joinRemotePath(QStringLiteral("/base"),
                              QStringLiteral("../b")) == QStringLiteral("/b"),
               "joining should normalize traversal segments");
}

OPENSCP_TEST(testRemoteEntryNameSafety, test) {
    test.check(isSafeRemoteEntryName(QStringLiteral("report.txt")),
               "ordinary remote entry names should be accepted");
    test.check(isSafeRemoteEntryName(QString::fromUtf8("résumé 2026")),
               "Unicode and spaces should be accepted in remote names");
    for (const QString &name :
         {QString(), QStringLiteral("."), QStringLiteral(".."),
          QStringLiteral("folder/file"), QStringLiteral("folder\\file"),
          QStringLiteral("line\nbreak"), QString(QChar(0x7f))}) {
        test.check(!isSafeRemoteEntryName(name),
                   "unsafe remote entry names should be rejected");
    }
}

OPENSCP_TEST(testByteFormatting, test) {
    test.check(formatByteSize(0) == QStringLiteral("0 B"),
               "zero bytes should use the byte unit");
    test.check(formatByteSize(1536) == QStringLiteral("1.5 KiB"),
               "binary byte sizes should use IEC units");
    test.check(formatTransferRate(1.5) == QStringLiteral("1.5 KiB/s"),
               "transfer rates should reuse byte-size formatting");
    test.check(formatTransferRate(0.0) == QString::fromUtf8("—"),
               "unknown transfer rates should use an em dash");
}

OPENSCP_TEST(testPathDepthOrdering, test) {
    const PathDepthComparator shallowestFirst{.deepestFirst = false};
    test.check(
        shallowestFirst(QStringLiteral("/root"), QStringLiteral("/root/child")),
        "shallow ordering should place parent paths first");
    test.check(
        shallowestFirst(QStringLiteral("/root/a"), QStringLiteral("/root/b")),
        "shallow ordering should break equal-depth ties ascending");

    const PathDepthComparator deepestFirst{.deepestFirst = true};
    test.check(
        deepestFirst(QStringLiteral("/root/child"), QStringLiteral("/root")),
        "deep ordering should place child paths first");
    test.check(
        deepestFirst(QStringLiteral("/root/b"), QStringLiteral("/root/a")),
        "deep ordering should break equal-depth ties descending");
}

OPENSCP_TEST(testTerminalTransferStatuses, test) {
    using Status = TransferTask::Status;
    for (const Status status : {Status::Done, Status::Error, Status::Canceled,
                                Status::Skipped, Status::Warning}) {
        test.check(isTerminalTransferStatus(status),
                   "every completed outcome should be terminal");
    }
    for (const Status status :
         {Status::Queued, Status::Running, Status::Paused,
          Status::WaitingForConnection, Status::RetryWaiting}) {
        test.check(!isTerminalTransferStatus(status),
                   "an actionable transfer state should not be terminal");
    }
}

void fillModel(RemoteModel &model, int rows) {
    std::vector<openscp::FileInfo> entries;
    entries.reserve(static_cast<std::size_t>(rows));
    for (int row = 0; row < rows; ++row) {
        openscp::FileInfo info;
        info.name = "entry-" + std::to_string(row);
        info.has_size = true;
        info.mode = 0100644u;
        entries.push_back(info);
    }
    model.setEntries(QStringLiteral("/"), entries);
}

OPENSCP_TEST(testRowSelectionMergesConsecutiveRows, test) {
    RemoteModel model;
    fillModel(model, 10);
    const QItemSelection selection =
        openscpui::rowSelection(model, {}, {4, 1, 2, 3, 7, 1});
    test.check(selection.size() == 2,
               "consecutive rows should become one range each");
    if (selection.size() != 2)
        return;
    test.check(selection[0].top() == 1 && selection[0].bottom() == 4 &&
                   selection[1].top() == 7 && selection[1].bottom() == 7,
               "ranges should cover the rows that were asked for, in order");
    test.check(selection[0].left() == 0 &&
                   selection[0].right() == model.columnCount() - 1,
               "each range should cover the whole row");
    QVector<int> selectedRows;
    for (const QModelIndex &index : selection.indexes()) {
        if (index.column() == 0)
            selectedRows.push_back(index.row());
    }
    std::sort(selectedRows.begin(), selectedRows.end());
    test.check(selectedRows == QVector<int>({1, 2, 3, 4, 7}),
               "the selection should hold every requested row once");
}

OPENSCP_TEST(testRowSelectionIgnoresRowsOutsideTheModel, test) {
    RemoteModel model;
    fillModel(model, 3);
    test.check(openscpui::rowSelection(model, {}, {}).isEmpty(),
               "no rows should give an empty selection");
    const QItemSelection selection =
        openscpui::rowSelection(model, {}, {-1, 0, 1, 3, 99});
    test.check(selection.size() == 1 && selection[0].top() == 0 &&
                   selection[0].bottom() == 1,
               "rows outside the model should be left out");
}

} // namespace

int main(int argc, char **argv) {
    openscp::test::TestHarness harness("UI utility");
    return harness.runWithApplication<QCoreApplication>(argc, argv);
}
