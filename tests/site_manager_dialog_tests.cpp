#include "SavedSitesPersistence.hpp"
#include "SiteManagerDialog.hpp"
#include "TestHarness.hpp"

#include <QApplication>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QStackedWidget>
#include <QTableView>
#include <QTemporaryDir>

#include <iostream>
#include <string>

namespace {

void clearSavedSites() {
    QSettings(QStringLiteral("OpenSCP"), QStringLiteral("OpenSCP")).clear();
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
        SavedSitesPersistence::saveSites({savedSiteFixture()}, true);
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

    const auto clearResult = SavedSitesPersistence::saveSites({}, true);
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

} // namespace

int main(int argc, char **argv) {
    QApplication application(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("OpenSCP-tests"));
    QApplication::setApplicationName(
        QStringLiteral("site-manager-dialog-tests"));

    QTemporaryDir settingsRoot;
    if (!settingsRoot.isValid()) {
        std::cerr << "[FAIL] could not create isolated settings directory\n";
        return 1;
    }
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       settingsRoot.path());

    openscp::test::TestHarness harness("Site Manager dialog");
    const int result = harness.run();
    clearSavedSites();
    return result;
}
