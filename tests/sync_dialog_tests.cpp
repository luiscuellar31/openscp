// Sync dialog tests: how the folder comparison reaches the preview.
#include "QtTestSupport.hpp"
#include "TestHarness.hpp"
#include "widgets/dialogs/SyncDialog.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QThreadPool>

#include <algorithm>
#include <chrono>
#include <iostream>

namespace {

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

QCheckBox *mirrorCheck(const SyncDialog &dialog) {
    for (QCheckBox *check : dialog.findChildren<QCheckBox *>()) {
        if (check->text().startsWith(QStringLiteral("Mirror destination")))
            return check;
    }
    return nullptr;
}

QComboBox *directionCombo(const SyncDialog &dialog) {
    for (QComboBox *combo : dialog.findChildren<QComboBox *>()) {
        if (combo->findText(QStringLiteral("Remote → Local")) >= 0)
            return combo;
    }
    return nullptr;
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

OPENSCP_TEST(testPendingMirrorChangeCannotAcceptStalePreview, test) {
    SyncDialog dialog;
    dialog.setSnapshots({}, snapshotWith(QStringLiteral("extra.dat"), 0));
    QCheckBox *mirror = mirrorCheck(dialog);
    QPushButton *synchronize = synchronizeButton(dialog);
    auto *buttons = dialog.findChild<QDialogButtonBox *>();
    test.check(mirror && synchronize && buttons,
               "mirror control and accept button should exist");
    if (!mirror || !synchronize || !buttons)
        return;

    mirror->setChecked(true);
    test.check(waitUntil([&] {
                   return !dialog.executionPlan().deletes.isEmpty() &&
                          synchronize->isEnabled();
               }),
               "mirror deletion should reach the ready preview");

    mirror->setChecked(false);
    test.check(!synchronize->isEnabled(),
               "changing mirror should immediately disable synchronization");
    QMetaObject::invokeMethod(buttons, "accepted", Qt::DirectConnection);
    test.check(dialog.result() != QDialog::Accepted,
               "accepting during the debounce must not execute stale items");
    test.check(waitUntil([&] {
                   return synchronize->isEnabled() &&
                          dialog.executionPlan().deletes.isEmpty();
               }),
               "the refreshed preview should contain no deletions");
}

OPENSCP_TEST(testPendingChangeIgnoresEarlierQueuedComparison, test) {
    SyncDialog dialog;
    dialog.setSnapshots({}, snapshotWith(QStringLiteral("extra.dat"), 0));
    QCheckBox *mirror = mirrorCheck(dialog);
    QPushButton *synchronize = synchronizeButton(dialog);
    test.check(mirror && synchronize, "mirror controls should exist");
    if (!mirror || !synchronize)
        return;

    // The worker has finished, but its result is still queued for the UI.
    test.check(QThreadPool::globalInstance()->waitForDone(5000),
               "the earlier comparison should finish before delivering it");
    mirror->setChecked(true);
    QCoreApplication::sendPostedEvents(qApp, QEvent::MetaCall);
    test.check(!synchronize->isEnabled(),
               "a superseded result must not enable synchronization");
    test.check(waitUntil([&] {
                   return synchronize->isEnabled() &&
                          !dialog.executionPlan().deletes.isEmpty();
               }),
               "only the comparison with the current options should apply");
}

OPENSCP_TEST(testPendingDirectionAndFilterChangesCannotAccept, test) {
    SyncDialog dialog;
    dialog.setSnapshots(snapshotWith(QStringLiteral("local-only.dat"), 0),
                        snapshotWith(QStringLiteral("remote-only.dat"), 0));
    QComboBox *direction = directionCombo(dialog);
    QPushButton *synchronize = synchronizeButton(dialog);
    auto *buttons = dialog.findChild<QDialogButtonBox *>();
    auto *exclude = dialog.findChild<QPlainTextEdit *>();
    test.check(direction && synchronize && buttons && exclude,
               "direction, filter and accept controls should exist");
    if (!direction || !synchronize || !buttons || !exclude)
        return;
    test.check(waitUntil([&] {
                   return synchronize->isEnabled() &&
                          dialog.executionPlan().copies.size() == 1;
               }),
               "initial comparison should be ready");

    direction->setCurrentIndex(
        direction->findData(static_cast<int>(SyncDirection::RemoteToLocal)));
    test.check(!synchronize->isEnabled(),
               "changing direction should immediately disable synchronization");
    QMetaObject::invokeMethod(buttons, "accepted", Qt::DirectConnection);
    test.check(dialog.result() != QDialog::Accepted,
               "pending direction must not accept the old copy operation");
    test.check(waitUntil([&] {
                   const auto plan = dialog.executionPlan();
                   return synchronize->isEnabled() && plan.copies.size() == 1 &&
                          plan.copies.front().relativePath ==
                              QStringLiteral("shared/remote-only.dat");
               }),
               "reverse direction should show the remote source file");

    exclude->setPlainText(QStringLiteral("remote-only.dat"));
    test.check(!synchronize->isEnabled(),
               "changing filters should immediately disable synchronization");
    QMetaObject::invokeMethod(buttons, "accepted", Qt::DirectConnection);
    test.check(dialog.result() != QDialog::Accepted,
               "pending filter must not accept a now excluded copy");
    test.check(waitUntil([&] {
                   return synchronize->isEnabled() &&
                          dialog.executionPlan().copies.isEmpty();
               }),
               "refreshed filter should exclude the remote source file");
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
