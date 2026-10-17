#include "QtTestSupport.hpp"
#include "TestHarness.hpp"
#include "logic/persistence/SavedSitesPersistence.hpp"
#include "widgets/dialogs/SiteManagerDialog.hpp"

#include <QApplication>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QStackedWidget>
#include <QTableView>
#include <QTemporaryDir>
#include <QTimer>

#include <iostream>
#include <string>

namespace {

void clearSavedSites() {
    QSettings().clear();
}

SiteEntry savedSiteFixture() {
    SiteEntry site;
    site.siteId = QStringLiteral("site-manager-test-id");
    site.name = QStringLiteral("Test server");
    site.opt.protocol = openscp::Protocol::Sftp;
    site.opt.host = "sftp.example";
    site.opt.port = 22;
    site.opt.username = "alice";
    return site;
}

OPENSCP_TEST(testEmptyStateIsTheInitialCallToAction, test) {
    clearSavedSites();
    SiteManagerDialog dialog;

    auto *content = dialog.findChild<QStackedWidget *>(
        QStringLiteral("siteManagerContent"));
    auto *emptyState =
        dialog.findChild<QWidget *>(QStringLiteral("siteManagerEmptyState"));
    auto *search =
        dialog.findChild<QLineEdit *>(QStringLiteral("siteManagerSearch"));
    auto *title =
        dialog.findChild<QLabel *>(QStringLiteral("siteManagerEmptyTitle"));
    auto *description = dialog.findChild<QLabel *>(
        QStringLiteral("siteManagerEmptyDescription"));
    auto *addButton = dialog.findChild<QPushButton *>(
        QStringLiteral("siteManagerEmptyAddButton"));
    auto *footerAddButton =
        dialog.findChild<QPushButton *>(QStringLiteral("siteManagerAddButton"));
    auto *footerEditButton = dialog.findChild<QPushButton *>(
        QStringLiteral("siteManagerEditButton"));
    auto *footerDuplicateButton = dialog.findChild<QPushButton *>(
        QStringLiteral("siteManagerDuplicateButton"));
    auto *footerDeleteButton = dialog.findChild<QPushButton *>(
        QStringLiteral("siteManagerDeleteButton"));
    auto *footerConnectButton = dialog.findChild<QPushButton *>(
        QStringLiteral("siteManagerConnectButton"));
    auto *closeButton = dialog.findChild<QPushButton *>(
        QStringLiteral("siteManagerCloseButton"));

    test.check(content && emptyState && search && title && description &&
                   addButton && footerAddButton && footerEditButton &&
                   footerDuplicateButton && footerDeleteButton &&
                   footerConnectButton && closeButton,
               "the Site Manager should expose a complete empty state");
    if (!content || !emptyState || !search || !title || !description ||
        !addButton || !footerAddButton || !footerEditButton ||
        !footerDuplicateButton || !footerDeleteButton || !footerConnectButton ||
        !closeButton) {
        return;
    }

    test.check(content->currentWidget() == emptyState,
               "the empty state should replace the table when no sites exist");
    test.check(search->isHidden(),
               "search should be hidden when there are no sites to search");
    test.check(title->text() == QStringLiteral("No saved sites"),
               "the empty state should explain why the list is blank");
    test.check(!description->text().trimmed().isEmpty(),
               "the empty state should explain what adding a site does");
    test.check(addButton->isEnabled() && addButton->isDefault(),
               "adding a site should be the primary empty-state action");
    test.check(description->sizePolicy().hasHeightForWidth(),
               "wrapped empty-state text should reserve its required height");
    test.check(footerAddButton->isHidden() && footerEditButton->isHidden() &&
                   footerDuplicateButton->isHidden() &&
                   footerDeleteButton->isHidden() &&
                   footerConnectButton->isHidden(),
               "site actions should not duplicate or clutter the empty state");
    test.check(!closeButton->isHidden(),
               "Close should remain available in the empty state");
}

OPENSCP_TEST(testReloadSwitchesBetweenEmptyStateAndTable, test) {
    clearSavedSites();
    SiteManagerDialog dialog;

    const auto saveResult =
        SavedSitesPersistence::saveSites({savedSiteFixture()});
    test.check(saveResult.ok,
               std::string("the saved-site fixture should persist: ") +
                   saveResult.error.toStdString());
    if (!saveResult.ok)
        return;

    dialog.reloadFromSettings();
    auto *content = dialog.findChild<QStackedWidget *>(
        QStringLiteral("siteManagerContent"));
    auto *table =
        dialog.findChild<QTableView *>(QStringLiteral("savedSitesTable"));
    auto *search =
        dialog.findChild<QLineEdit *>(QStringLiteral("siteManagerSearch"));
    auto *footerAddButton =
        dialog.findChild<QPushButton *>(QStringLiteral("siteManagerAddButton"));
    auto *footerEditButton = dialog.findChild<QPushButton *>(
        QStringLiteral("siteManagerEditButton"));
    auto *footerDuplicateButton = dialog.findChild<QPushButton *>(
        QStringLiteral("siteManagerDuplicateButton"));
    auto *footerDeleteButton = dialog.findChild<QPushButton *>(
        QStringLiteral("siteManagerDeleteButton"));
    auto *footerConnectButton = dialog.findChild<QPushButton *>(
        QStringLiteral("siteManagerConnectButton"));

    test.check(content && table && search && footerAddButton &&
                   footerEditButton && footerDuplicateButton &&
                   footerDeleteButton && footerConnectButton,
               "the populated Site Manager widgets should be available");
    if (!content || !table || !search || !footerAddButton ||
        !footerEditButton || !footerDuplicateButton || !footerDeleteButton ||
        !footerConnectButton) {
        return;
    }

    test.check(content->currentWidget() == table,
               "the table should replace the empty state after a site appears");
    test.check(!search->isHidden(),
               "search should be restored when saved sites exist");
    test.check(table->model() && table->model()->rowCount() == 1,
               "the restored table should contain the saved site");
    test.check(!footerAddButton->isHidden() && !footerEditButton->isHidden() &&
                   !footerDuplicateButton->isHidden() &&
                   !footerDeleteButton->isHidden() &&
                   !footerConnectButton->isHidden(),
               "site actions should return with the populated table");

    const auto clearResult = SavedSitesPersistence::saveSites({});
    test.check(clearResult.ok, "the saved-site fixture should be removable");
    if (!clearResult.ok)
        return;

    dialog.reloadFromSettings();
    auto *emptyState =
        dialog.findChild<QWidget *>(QStringLiteral("siteManagerEmptyState"));
    test.check(emptyState && content->currentWidget() == emptyState,
               "removing the final site should restore the empty state");
    test.check(search->isHidden(),
               "search should hide again after the final site is removed");
}

OPENSCP_TEST(testChangesKeepSitesSavedElsewhere, test) {
    clearSavedSites();
    const SiteEntry kept = savedSiteFixture();
    SiteEntry removed = savedSiteFixture();
    removed.siteId = QStringLiteral("site-manager-removed-id");
    removed.name = QStringLiteral("Old server");
    test.check(SavedSitesPersistence::saveSites({removed}).ok,
               "the first saved site should persist");
    SiteManagerDialog dialog;
    // Saved while the dialog is open, as a quick connection does.
    test.check(SavedSitesPersistence::saveSites({removed, kept}).ok,
               "a site saved elsewhere should persist");

    auto *table =
        dialog.findChild<QTableView *>(QStringLiteral("savedSitesTable"));
    auto *deleteButton = dialog.findChild<QPushButton *>(
        QStringLiteral("siteManagerDeleteButton"));
    test.check(table && table->model() && deleteButton &&
                   table->model()->rowCount() == 1,
               "the dialog should list the site it loaded");
    if (!table || !table->model() || !deleteButton)
        return;
    table->selectRow(0);
    deleteButton->click();

    const auto stored = SavedSitesPersistence::loadSites().sites;
    test.check(stored.size() == 1 && stored.front().siteId == kept.siteId,
               "removing a site must keep the sites saved after the dialog "
               "loaded them");
    test.check(table->model()->rowCount() == 1,
               "the dialog should list the stored sites after a change");
}

OPENSCP_TEST(testRemovingASiteOnlyRemovesItsHostKeyWhenAsked, test) {
    clearSavedSites();
    QTemporaryDir directory;
    test.check(directory.isValid(), "the host key test needs a folder");
    if (!directory.isValid())
        return;
    const QString knownHosts =
        directory.filePath(QStringLiteral("known_hosts"));
    const auto writeKnownHosts = [&knownHosts] {
        QFile file(knownHosts);
        return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
               file.write("sftp.example ssh-ed25519 "
                          "AAAAC3NzaC1lZDI1NTE5AAAAILZlz+tnMZZGpyX4/"
                          "qwU9iIfMHkUqPnwGwGZRuQQ3v1d\n") > 0;
    };
    const auto keyKept = [&knownHosts] {
        QFile file(knownHosts);
        return file.open(QIODevice::ReadOnly) &&
               file.readAll().contains("sftp.example");
    };
    const auto site = [&knownHosts](const char *id,
                                    openscp::Protocol protocol) {
        SiteEntry entry = savedSiteFixture();
        entry.siteId = QString::fromLatin1(id);
        entry.name = QString::fromLatin1(id);
        entry.opt.protocol = protocol;
        entry.opt.known_hosts_path = knownHosts.toStdString();
        return entry;
    };

    // Removes the only listed site, answering any question with `answer`,
    // and tells whether a question was asked.
    const auto removeOnlySite = [&test](const QVector<SiteEntry> &sites,
                                        QMessageBox::StandardButton answer) {
        test.check(SavedSitesPersistence::saveSites(sites).ok,
                   "the host key sites should be saved");
        SiteManagerDialog dialog;
        auto *table =
            dialog.findChild<QTableView *>(QStringLiteral("savedSitesTable"));
        auto *deleteButton = dialog.findChild<QPushButton *>(
            QStringLiteral("siteManagerDeleteButton"));
        if (!table || !deleteButton)
            return false;
        table->selectRow(0);
        bool asked = false;
        QTimer::singleShot(0, &dialog, [&asked, answer] {
            if (auto *box = qobject_cast<QMessageBox *>(
                    QApplication::activeModalWidget())) {
                asked = true;
                box->done(answer);
            }
        });
        deleteButton->click();
        return asked;
    };

    test.check(writeKnownHosts(), "the known_hosts fixture should be written");
    const SiteEntry first = site("a-first", openscp::Protocol::Sftp);
    const SiteEntry second = site("b-second", openscp::Protocol::Scp);
    test.check(!removeOnlySite({first, second}, QMessageBox::Yes) && keyKept(),
               "a key another saved site uses should stay without asking");
    test.check(!removeOnlySite({site("a-ftp", openscp::Protocol::Ftp)},
                               QMessageBox::Yes) &&
                   keyKept(),
               "removing an FTP site should not touch known_hosts");
    test.check(removeOnlySite({second}, QMessageBox::No) && keyKept(),
               "the key should stay when the user keeps it");
    test.check(removeOnlySite({second}, QMessageBox::Yes) && !keyKept(),
               "the key should go when the user asks for it");
    test.check(!removeOnlySite({second}, QMessageBox::Yes),
               "nothing should be asked once known_hosts lacks the key");
}

} // namespace

int main(int argc, char **argv) {
    QApplication application(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("OpenSCP-tests"));
    QApplication::setApplicationName(
        QStringLiteral("site-manager-dialog-tests"));

    openscp::testsupport::IsolatedSettings settingsRoot;
    if (!settingsRoot.isValid()) {
        std::cerr << "[FAIL] could not create isolated settings directory\n";
        return 1;
    }

    openscp::test::TestHarness harness("Site Manager dialog");
    const int result = harness.run();
    clearSavedSites();
    return result;
}
