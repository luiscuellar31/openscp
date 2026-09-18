// Sync dialog tests: how the folder comparison reaches the preview.
#include "QtTestSupport.hpp"
#include "TestHarness.hpp"
#include "widgets/dialogs/SyncDialog.hpp"

#include <QApplication>
#include <QDialogButtonBox>
#include <QPushButton>

#include <algorithm>
#include <chrono>
#include <iostream>

namespace {

using openscp::testsupport::flushUiEvents;
using openscp::testsupport::waitUntil;

QVector<SyncSnapshotEntry> snapshotWith(const QString &uniqueName, int files) {
    QVector<SyncSnapshotEntry> entries;
    entries.reserve(files + 2);
    SyncSnapshotEntry folder;
    folder.relativePath = QStringLiteral("shared");
    folder.type = SyncEntryType::Directory;
    folder.modifiedMs = 1;
    entries.push_back(folder);
    for (int file = 0; file < files; ++file) {
        SyncSnapshotEntry item;
        item.relativePath = QStringLiteral("shared/file-%1.dat").arg(file);
        item.size = 10;
        item.modifiedMs = 1;
        entries.push_back(item);
    }
    SyncSnapshotEntry unique;
    unique.relativePath = QStringLiteral("shared/") + uniqueName;
    unique.size = 10;
    unique.modifiedMs = 1;
    entries.push_back(unique);
    return entries;
}

bool hasItemFor(const SyncDialog &dialog, const QString &relativePath) {
    const auto items = dialog.comparisonItems();
    return std::any_of(items.cbegin(), items.cend(),
                       [&](const SyncComparisonItem &item) {
                           return item.relativePath == relativePath;
                       });
}

QPushButton *synchronizeButton(const SyncDialog &dialog) {
    auto *buttons = dialog.findChild<QDialogButtonBox *>();
    return buttons ? buttons->button(QDialogButtonBox::Ok) : nullptr;
}

OPENSCP_TEST(testComparisonWithoutSnapshotsIsReadyAtOnce, test) {
    SyncDialog dialog;
    test.check(dialog.comparisonItems().isEmpty(),
               "a dialog without snapshots should have an empty preview");
    QPushButton *synchronize = synchronizeButton(dialog);
    test.check(synchronize && synchronize->isEnabled(),
               "an empty comparison should leave the dialog usable");
}

OPENSCP_TEST(testComparisonRunsWithoutBlockingTheDialog, test) {
    SyncDialog dialog;
    dialog.setSnapshots(snapshotWith(QStringLiteral("only-source.dat"), 400),
                        {});
    QPushButton *synchronize = synchronizeButton(dialog);
    test.check(synchronize && !synchronize->isEnabled(),
               "the dialog should not synchronize a comparison in progress");
    test.check(waitUntil([&] {
                   return hasItemFor(dialog,
                                     QStringLiteral("shared/only-source.dat"));
               }),
               "the comparison should reach the preview when it finishes");
    test.check(synchronize && synchronize->isEnabled(),
               "the dialog should be usable once the comparison lands");
}

OPENSCP_TEST(testOnlyTheNewestComparisonReachesThePreview, test) {
    SyncDialog dialog;
    // The superseded comparison is far larger, so it finishes after the one
    // that replaced it.
    dialog.setSnapshots(snapshotWith(QStringLiteral("stale.dat"), 100'000), {});
    dialog.setSnapshots(snapshotWith(QStringLiteral("newest.dat"), 50), {});
    test.check(waitUntil([&] {
                   return hasItemFor(dialog,
                                     QStringLiteral("shared/newest.dat"));
               }),
               "the newest comparison should reach the preview");
    const bool staleArrived = waitUntil(
        [&] { return hasItemFor(dialog, QStringLiteral("shared/stale.dat")); },
        std::chrono::milliseconds(2000));
    test.check(!staleArrived,
               "a superseded comparison should not overwrite the newest one");
}

} // namespace

int main(int argc, char **argv) {
    QApplication application(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("OpenSCP-tests"));
    QApplication::setApplicationName(QStringLiteral("sync-dialog-tests"));

    openscp::testsupport::IsolatedSettings settingsRoot;
    if (!settingsRoot.isValid()) {
        std::cerr << "[FAIL] could not create isolated settings directory\n";
        return 1;
    }
    openscp::test::TestHarness harness("Sync dialog");
    return harness.run();
}
