#include "ConnectionDialog.hpp"
#include "QtTestSupport.hpp"
#include "TestHarness.hpp"

#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPalette>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QTemporaryDir>
#include <QToolButton>

#include <iostream>

namespace {

using openscp::testsupport::flushUiEvents;

QToolButton *sectionToggle(ConnectionDialog &dialog, const char *name) {
    return dialog.findChild<QToolButton *>(QString::fromLatin1(name));
}

void clearSettings() {
    QSettings(QStringLiteral("OpenSCP"), QStringLiteral("OpenSCP")).clear();
}

OPENSCP_TEST(testSavedSiteFormStartsCompact, test) {
    clearSettings();
    ConnectionDialog dialog;
    dialog.setSiteNameVisible(true);
    dialog.ensurePolished();
    dialog.show();
    flushUiEvents();

    auto *pathsToggle = sectionToggle(dialog, "connectionPathsSectionToggle");
    auto *sshToggle = sectionToggle(dialog, "connectionSshKeySectionToggle");
    auto *networkToggle =
        sectionToggle(dialog, "connectionNetworkSectionToggle");
    auto *securityToggle =
        sectionToggle(dialog, "connectionSecuritySectionToggle");
    auto *host = dialog.findChild<QWidget *>(QStringLiteral("connectionHost"));
    auto *pathRow = dialog.findChild<QWidget *>(
        QStringLiteral("connectionInitialLocalPathRow"));
    auto *keyRow =
        dialog.findChild<QWidget *>(QStringLiteral("connectionSshKeyPathRow"));
    auto *proxyType =
        dialog.findChild<QComboBox *>(QStringLiteral("connectionProxyType"));
    auto *knownHostsRow = dialog.findChild<QWidget *>(
        QStringLiteral("connectionKnownHostsPathRow"));
    auto *pathsSection =
        dialog.findChild<QWidget *>(QStringLiteral("connectionPathsSection"));
    auto *pathsSummary = dialog.findChild<QLabel *>(
        QStringLiteral("connectionPathsSectionSummary"));
    auto *securitySummary = dialog.findChild<QLabel *>(
        QStringLiteral("connectionSecuritySectionSummary"));
    auto *scrollArea = dialog.findChild<QScrollArea *>(
        QStringLiteral("connectionOptionsScrollArea"));
    auto *dialogButtons = dialog.findChild<QDialogButtonBox *>(
        QStringLiteral("connectionDialogButtons"));

    test.check(pathsToggle && sshToggle && networkToggle && securityToggle &&
                   host && pathRow && keyRow && proxyType && knownHostsRow &&
                   pathsSection && pathsSummary && scrollArea && dialogButtons,
               "the compact connection form should expose every section");
    if (!pathsToggle || !sshToggle || !networkToggle || !securityToggle ||
        !host || !pathRow || !keyRow || !proxyType || !knownHostsRow ||
        !pathsSection || !pathsSummary || !scrollArea || !dialogButtons) {
        return;
    }

    test.check(!host->isHidden(),
               "essential connection fields should remain visible");
    test.check(!pathsToggle->isChecked() && !sshToggle->isChecked() &&
                   !networkToggle->isChecked() && !securityToggle->isChecked(),
               "optional sections should start collapsed");
    test.check(pathRow->isHidden() && keyRow->isHidden() &&
                   proxyType->isHidden() && knownHostsRow->isHidden(),
               "collapsed sections should hide their detailed fields");
    test.check(securitySummary &&
                   securitySummary->text() == securitySummary->toolTip(),
               "default section summaries should fit without truncation");

    const QSize compactSize = dialog.size();
    const QPoint compactPosition = dialog.pos();
    const int compactHeaderWidth = pathsSection->width();
    const int compactToggleX = pathsToggle->mapTo(&dialog, QPoint()).x();
    pathsToggle->click();
    flushUiEvents();

    test.check(dialog.width() == compactSize.width(),
               "expanding a section should preserve the dialog width");
    test.check(pathsSection->width() == compactHeaderWidth &&
                   pathsSummary->isHidden(),
               "expanded headers should preserve their width and hide their "
               "redundant summaries");
    test.check(pathsToggle->mapTo(&dialog, QPoint()).x() == compactToggleX,
               "the disclosure arrow should stay left-aligned when opened");
    test.check(dialog.pos() == compactPosition,
               "expanding a section should not move the dialog");

    sshToggle->click();
    const int socksIndex =
        proxyType->findData(static_cast<int>(openscp::ProxyType::Socks5));
    if (socksIndex >= 0)
        proxyType->setCurrentIndex(socksIndex);
    networkToggle->click();
    securityToggle->click();
    flushUiEvents();

    test.check(!pathRow->isHidden() && !keyRow->isHidden() &&
                   !proxyType->isHidden() && !knownHostsRow->isHidden(),
               "expanding sections should reveal their detailed fields");
    test.check(dialog.height() > compactSize.height(),
               "progressive disclosure should reduce the initial height");
    test.check(dialog.width() == compactSize.width(),
               "opening every section should not widen the dialog");
    if (dialog.screen()) {
        test.check(dialog.frameGeometry().height() <=
                       dialog.screen()->availableGeometry().height() * 4 / 5,
                   "expanded content should stay within the screen height "
                   "limit");
    }
    test.check(scrollArea->verticalScrollBarPolicy() == Qt::ScrollBarAsNeeded,
               "overflowing optional fields should use internal scrolling");
    test.check(!dialogButtons->isHidden() &&
                   dialogButtons->parentWidget() == &dialog,
               "dialog actions should remain fixed outside the scroll area");
}

OPENSCP_TEST(testSavedSiteFormRejectsHorizontalClipping, test) {
    clearSettings();
    ConnectionDialog dialog;
    // A caller or window manager may provide an undersized initial geometry.
    // The dialog must recover because horizontal scrolling is intentionally
    // disabled for this form.
    dialog.resize(1, dialog.height());
    dialog.setSiteNameVisible(true);
    dialog.ensurePolished();
    dialog.show();
    flushUiEvents();

    auto *siteName =
        dialog.findChild<QWidget *>(QStringLiteral("connectionSiteName"));
    auto *scrollArea = dialog.findChild<QScrollArea *>(
        QStringLiteral("connectionOptionsScrollArea"));
    test.check(siteName && scrollArea,
               "the connection form and its viewport should exist");
    if (!siteName || !scrollArea)
        return;

    const QPoint siteNameRight = siteName->mapTo(
        scrollArea->viewport(), QPoint(siteName->width() - 1, 0));
    const QPoint scrollAreaRight =
        scrollArea->mapTo(&dialog, QPoint(scrollArea->width() - 1, 0));
    test.check(scrollArea->viewport()->width() >=
                       scrollArea->widget()->minimumSizeHint().width() &&
                   scrollAreaRight.x() <= dialog.contentsRect().right() &&
                   siteNameRight.x() <=
                       scrollArea->viewport()->rect().right() &&
                   scrollArea->horizontalScrollBar()->maximum() == 0,
               "the initial form should fit without horizontal clipping");
}

OPENSCP_TEST(testProtocolHidesUnsupportedSections, test) {
    clearSettings();
    ConnectionDialog dialog;
    auto *protocol =
        dialog.findChild<QComboBox *>(QStringLiteral("connectionProtocol"));
    auto *sshSection =
        dialog.findChild<QWidget *>(QStringLiteral("connectionSshKeySection"));

    test.check(protocol && sshSection,
               "protocol and SSH disclosure controls should exist");
    if (!protocol || !sshSection)
        return;

    const int ftpIndex =
        protocol->findData(static_cast<int>(openscp::Protocol::Ftp));
    test.check(ftpIndex >= 0, "FTP should be available in the protocol list");
    if (ftpIndex < 0)
        return;

    protocol->setCurrentIndex(ftpIndex);
    test.check(sshSection->isHidden(),
               "FTP should not expose SSH key configuration");
}

OPENSCP_TEST(testConfiguredSectionsExpandWhenEditing, test) {
    clearSettings();
    ConnectionDialog dialog;
    openscp::SessionOptions options;
    options.protocol = openscp::Protocol::Sftp;
    options.host = "sftp.example";
    options.port = 22;
    options.username = "alice";
    options.private_key_path = "/tmp/id_ed25519";
    options.proxy_type = openscp::ProxyType::Socks5;
    options.proxy_host = "proxy.example";
    options.proxy_port = 1080;
    options.known_hosts_policy = openscp::KnownHostsPolicy::AcceptNew;

    dialog.setOptions(options);
    auto *sshToggle = sectionToggle(dialog, "connectionSshKeySectionToggle");
    auto *networkToggle =
        sectionToggle(dialog, "connectionNetworkSectionToggle");
    auto *securityToggle =
        sectionToggle(dialog, "connectionSecuritySectionToggle");
    auto *networkSummary = dialog.findChild<QLabel *>(
        QStringLiteral("connectionNetworkSectionSummary"));

    test.check(sshToggle && networkToggle && securityToggle && networkSummary,
               "configured disclosure controls should be discoverable");
    if (!sshToggle || !networkToggle || !securityToggle || !networkSummary)
        return;

    test.check(sshToggle->isChecked() && networkToggle->isChecked() &&
                   securityToggle->isChecked(),
               "non-default edited settings should expand their sections");
    test.check(!networkSummary->text().trimmed().isEmpty(),
               "collapsed sections should retain a meaningful summary");

    const openscp::SessionOptions restored = dialog.options();
    test.check(restored.private_key_path == options.private_key_path &&
                   restored.proxy_type == options.proxy_type &&
                   restored.proxy_host == options.proxy_host &&
                   restored.proxy_port == options.proxy_port &&
                   restored.known_hosts_policy == options.known_hosts_policy,
               "disclosure state must not change connection semantics");
}

OPENSCP_TEST(testAcceptButtonCanDescribeTheAction, test) {
    clearSettings();
    ConnectionDialog dialog;
    dialog.setAcceptButtonText(QStringLiteral("Add site"));
    auto *acceptButton = dialog.findChild<QPushButton *>(
        QStringLiteral("connectionAcceptButton"));
    test.check(acceptButton &&
                   acceptButton->text() == QStringLiteral("Add site"),
               "callers should be able to name the resulting action");
}

OPENSCP_TEST(testDisclosureHeadersFollowPaletteAndInputModality, test) {
    clearSettings();
    ConnectionDialog dialog;
    dialog.setSiteNameVisible(true);

    const QColor expectedTextColor(255, 255, 255);
    QPalette highContrastPalette = dialog.palette();
    highContrastPalette.setColor(QPalette::Active, QPalette::WindowText,
                                 expectedTextColor);
    highContrastPalette.setColor(QPalette::Inactive, QPalette::WindowText,
                                 expectedTextColor);
    dialog.setPalette(highContrastPalette);
    dialog.show();
    flushUiEvents();

    auto *toggle = sectionToggle(dialog, "connectionPathsSectionToggle");
    auto *summary = dialog.findChild<QLabel *>(
        QStringLiteral("connectionPathsSectionSummary"));
    QWidget *focusIndicator =
        toggle ? toggle->findChild<QWidget *>(
                     QStringLiteral("keyboardFocusIndicator"),
                     Qt::FindDirectChildrenOnly)
               : nullptr;
    test.check(toggle && summary,
               "a disclosure header should expose its toggle and summary");
    if (!toggle || !summary)
        return;

    const QColor summaryTextColor =
        summary->palette().color(QPalette::Active, QPalette::WindowText);
    test.check(summaryTextColor == expectedTextColor &&
                   summaryTextColor.alpha() == 255,
               "summary text should inherit the full-opacity system palette");
    test.check(focusIndicator &&
                   !toggle->styleSheet().contains(QStringLiteral(":focus")) &&
                   !toggle->testAttribute(Qt::WA_MacShowFocusRect),
               "disclosure focus styling should use the shared indicator");

    QKeyEvent keyboardPress(QEvent::KeyPress, Qt::Key_Left, Qt::NoModifier);
    QApplication::sendEvent(toggle, &keyboardPress);
    toggle->setFocus(Qt::TabFocusReason);
    flushUiEvents();
    test.check(toggle->hasFocus(),
               "the disclosure toggle should accept keyboard focus");
    test.check(toggle->property("keyboardFocusVisible").toBool() &&
                   focusIndicator && focusIndicator->isVisible(),
               "keyboard focus should show the disclosure outline");

    const QPointF pointerPosition(2.0, 2.0);
    const QPointF globalPointerPosition =
        summary->mapToGlobal(pointerPosition.toPoint());
    QMouseEvent pointerPress(QEvent::MouseButtonPress, pointerPosition,
                             globalPointerPosition, Qt::LeftButton,
                             Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(summary, &pointerPress);
    flushUiEvents();
    test.check(toggle->hasFocus() &&
                   !toggle->property("keyboardFocusVisible").toBool() &&
                   focusIndicator && !focusIndicator->isVisible(),
               "pointer input should hide a retained keyboard-focus outline");
}

} // namespace

int main(int argc, char **argv) {
    QApplication application(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("OpenSCP-tests"));
    QApplication::setApplicationName(QStringLiteral("connection-dialog-tests"));

    QTemporaryDir settingsRoot;
    if (!settingsRoot.isValid()) {
        std::cerr << "[FAIL] could not create isolated settings directory\n";
        return 1;
    }
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       settingsRoot.path());

    openscp::test::TestHarness harness("Connection dialog");
    const int result = harness.run();
    clearSettings();
    return result;
}
