#include "ConnectionDialog.hpp"
#include "DragAwareTreeView.hpp"
#include "PathNavigationBar.hpp"
#include "TestHarness.hpp"
#include "ToolbarKeyboardNavigation.hpp"
#include "TransferManager.hpp"
#include "TransferQueueDialog.hpp"

#include <QAccessible>
#include <QAction>
#include <QApplication>
#include <QEventLoop>
#include <QFont>
#include <QFrame>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPushButton>
#include <QSettings>
#include <QTableView>
#include <QTemporaryDir>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

#include <iostream>

namespace {

class OrderedFocusWindow final : public QWidget {
    public:
    QList<QWidget *> order;

    protected:
    bool focusNextPrevChild(bool next) override {
        return openscpui::moveKeyboardFocus(order, QApplication::focusWidget(),
                                            next);
    }
};

void flushUiEvents() {
    for (int pass = 0; pass < 3; ++pass)
        QApplication::processEvents(QEventLoop::AllEvents);
}

void sendKey(QWidget *target, Qt::Key key,
             Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QKeyEvent press(QEvent::KeyPress, key, modifiers);
    QApplication::sendEvent(target, &press);
    QKeyEvent release(QEvent::KeyRelease, key, modifiers);
    QApplication::sendEvent(target, &release);
    flushUiEvents();
}

OPENSCP_TEST(testPathFieldPreservesAppearanceAndKeyboardNavigation, test) {
    openscpui::PathNavigationBar bar(openscpui::PathFlavor::Remote,
                                     QStringLiteral("/projects/reports"));
    bar.resize(480, bar.sizeHint().height());
    bar.show();
    flushUiEvents();

    auto *display = bar.findChild<QLineEdit *>(QStringLiteral("pathDisplay"));
    test.check(display &&
                   display->text() == QStringLiteral("/projects/reports"),
               "the complete path should remain visible in its field");
    if (!display)
        return;

    QAccessibleInterface *interface =
        QAccessible::queryAccessibleInterface(display);
    test.check(display->isReadOnly() &&
                   display->focusPolicy() == Qt::StrongFocus && interface &&
                   interface->role() == QAccessible::EditableText &&
                   interface->state().focusable && interface->state().readOnly,
               "the path field should expose readable and focusable text");
    test.check(!display->accessibleName().trimmed().isEmpty() &&
                   !display->accessibleDescription().trimmed().isEmpty(),
               "the path field should describe its keyboard interaction");
    test.check(!display->property("keyboardFocusVisible").toBool(),
               "the path field should not add an idle focus frame");

    QString requestedPath;
    int openDialogRequests = 0;
    QObject::connect(&bar, &openscpui::PathNavigationBar::pathRequested, &bar,
                     [&](const QString &path) { requestedPath = path; });
    QObject::connect(&bar, &openscpui::PathNavigationBar::openDialogRequested,
                     &bar, [&] { ++openDialogRequests; });

    display->clearFocus();
    flushUiEvents();
    display->setFocus(Qt::TabFocusReason);
    flushUiEvents();
    test.check(display->property("keyboardFocusVisible").toBool(),
               "Tab focus should show the path focus frame");
    sendKey(display, Qt::Key_Left);
    test.check(display->selectedText() == QStringLiteral("projects"),
               "Left should visibly select the parent path segment");
    sendKey(display, Qt::Key_Space);
    test.check(requestedPath == QStringLiteral("/projects"),
               "Space should navigate to a parent path segment");

    sendKey(display, Qt::Key_End);
    sendKey(display, Qt::Key_Return);
    test.check(openDialogRequests == 1,
               "Enter should activate the current-folder action");

    display->clearFocus();
    flushUiEvents();
    test.check(!display->property("keyboardFocusVisible").toBool(),
               "leaving the path field should hide its focus frame");
    display->setFocus(Qt::BacktabFocusReason);
    flushUiEvents();
    test.check(display->property("keyboardFocusVisible").toBool(),
               "Shift+Tab focus should show the path focus frame");

    const int initialFontHeight = display->fontMetrics().height();
    QFont largerFont = bar.font();
    largerFont.setPointSize(qMax(18, largerFont.pointSize() + 6));
    bar.setFont(largerFont);
    flushUiEvents();
    test.check(display->fontMetrics().height() > initialFontHeight,
               "path navigation should inherit larger accessibility fonts");
}

OPENSCP_TEST(testPathFieldKeyboardCanReachTheRoot, test) {
    openscpui::PathNavigationBar bar(openscpui::PathFlavor::Remote,
                                     QStringLiteral("/projects/reports"));
    bar.show();
    flushUiEvents();

    auto *display = bar.findChild<QLineEdit *>(QStringLiteral("pathDisplay"));
    if (!display) {
        test.check(false, "the path field should exist");
        return;
    }

    QString requestedPath;
    QObject::connect(&bar, &openscpui::PathNavigationBar::pathRequested, &bar,
                     [&](const QString &path) { requestedPath = path; });
    display->setFocus(Qt::TabFocusReason);
    sendKey(display, Qt::Key_Home);
    sendKey(display, Qt::Key_Return);
    test.check(requestedPath == QStringLiteral("/"),
               "Home and Enter should navigate to the root segment");
}

OPENSCP_TEST(testToolbarActionsParticipateInKeyboardTraversal, test) {
    OrderedFocusWindow window;
    auto *layout = new QVBoxLayout(&window);
    auto *beforeToolbar = new QLineEdit(&window);
    auto *toolbar = new QToolBar(&window);
    QAction *firstAction = toolbar->addAction(QStringLiteral("First"));
    QAction *disabledAction = toolbar->addAction(QStringLiteral("Disabled"));
    QAction *lastAction = toolbar->addAction(QStringLiteral("Last"));
    disabledAction->setEnabled(false);
    int firstActionTriggers = 0;
    QObject::connect(firstAction, &QAction::triggered, &window,
                     [&] { ++firstActionTriggers; });
    layout->addWidget(beforeToolbar);
    layout->addWidget(toolbar);
    openscpui::configureToolbarKeyboardNavigation(toolbar);
    window.order.push_back(beforeToolbar);
    window.order.append(openscpui::toolbarKeyboardFocusWidgets(toolbar));
    window.show();
    flushUiEvents();

    auto *firstButton =
        qobject_cast<QToolButton *>(toolbar->widgetForAction(firstAction));
    auto *disabledButton =
        qobject_cast<QToolButton *>(toolbar->widgetForAction(disabledAction));
    auto *lastButton =
        qobject_cast<QToolButton *>(toolbar->widgetForAction(lastAction));
    test.check(firstButton && disabledButton && lastButton,
               "toolbar action buttons should be available");
    if (!firstButton || !disabledButton || !lastButton)
        return;

    test.check(firstButton->focusPolicy() == Qt::TabFocus &&
                   disabledButton->focusPolicy() == Qt::TabFocus &&
                   lastButton->focusPolicy() == Qt::TabFocus,
               "toolbar actions should explicitly accept keyboard traversal");
    test.check(!firstButton->testAttribute(Qt::WA_MacShowFocusRect) &&
                   !(firstButton->focusPolicy() & Qt::ClickFocus) &&
                   !firstButton->property("keyboardFocusVisible").toBool(),
               "toolbar focus cues should be explicit and keyboard-only");
    test.check(!firstButton->accessibleName().trimmed().isEmpty() &&
                   !lastButton->accessibleName().trimmed().isEmpty(),
               "icon toolbar actions should retain accessible names");

    beforeToolbar->setFocus(Qt::TabFocusReason);
    flushUiEvents();
    sendKey(beforeToolbar, Qt::Key_Tab);
    test.check(QApplication::focusWidget() == firstButton,
               "Tab should enter a toolbar at its first enabled action");
    auto *focusIndicator = firstButton->findChild<QWidget *>(
        QStringLiteral("keyboardFocusIndicator"), Qt::FindDirectChildrenOnly);
    test.check(firstButton->property("keyboardFocusVisible").toBool() &&
                   focusIndicator && focusIndicator->isVisible(),
               "Tab should show a visible focus outline on a toolbar action");

    sendKey(firstButton, Qt::Key_Space);
    sendKey(firstButton, Qt::Key_Return);
    test.check(firstActionTriggers == 2,
               "Space and Enter should each activate a focused toolbar action");

    sendKey(firstButton, Qt::Key_Tab);
    test.check(QApplication::focusWidget() == lastButton,
               "Tab should move to the next enabled toolbar action");
    test.check(!firstButton->property("keyboardFocusVisible").toBool() &&
                   lastButton->property("keyboardFocusVisible").toBool(),
               "the focus outline should follow keyboard traversal");

    sendKey(lastButton, Qt::Key_Backtab, Qt::ShiftModifier);
    test.check(QApplication::focusWidget() == firstButton,
               "Shift+Tab should move to the previous enabled toolbar action");
    test.check(firstButton->property("keyboardFocusVisible").toBool(),
               "Shift+Tab should also show the toolbar focus outline");

    const QPointF localPosition(2.0, 2.0);
    const QPointF globalPosition =
        beforeToolbar->mapToGlobal(localPosition.toPoint());
    QMouseEvent mousePress(QEvent::MouseButtonPress, localPosition,
                           globalPosition, Qt::LeftButton, Qt::LeftButton,
                           Qt::NoModifier);
    QApplication::sendEvent(beforeToolbar, &mousePress);
    flushUiEvents();
    test.check(QApplication::focusWidget() == firstButton &&
                   !firstButton->property("keyboardFocusVisible").toBool() &&
                   focusIndicator && !focusIndicator->isVisible(),
               "using the mouse should hide the keyboard-only focus outline");

    auto *extension = openscpui::toolbarPopupExtensionButton(toolbar);
    test.check(extension && extension->menu() && !extension->isCheckable() &&
                   !extension->isChecked(),
               "the semantic toolbar extension adapter should find and "
               "normalize the popup button");
    if (extension && extension->menu()) {
        extension->setObjectName(QStringLiteral("renamedQtExtension"));
        extension->setCheckable(true);
        extension->setChecked(true);
        extension->setDown(true);
        extension->menu()->show();
        flushUiEvents();
        extension->menu()->hide();
        flushUiEvents();
        test.check(!extension->isDown() && !extension->isChecked(),
                   "closing the overflow menu should clear its pressed state "
                   "without relying on a private Qt object name");
    }
}

OPENSCP_TEST(testFilePanelShowsKeyboardOnlyFocusOutline, test) {
    OrderedFocusWindow window;
    auto *layout = new QVBoxLayout(&window);
    auto *beforePanel = new QLineEdit(&window);
    auto *panel = new DragAwareTreeView(&window);
    layout->addWidget(beforePanel);
    layout->addWidget(panel);
    window.order = {beforePanel, panel};
    window.resize(420, 280);
    window.show();
    flushUiEvents();

    auto *focusIndicator = panel->findChild<QWidget *>(
        QStringLiteral("keyboardFocusIndicator"), Qt::FindDirectChildrenOnly);
    test.check(focusIndicator && !focusIndicator->isVisible() &&
                   !panel->property("keyboardFocusVisible").toBool(),
               "a file panel should not show an idle focus outline");

    beforePanel->setFocus(Qt::TabFocusReason);
    sendKey(beforePanel, Qt::Key_Tab);
    test.check(QApplication::focusWidget() == panel &&
                   panel->property("keyboardFocusVisible").toBool() &&
                   focusIndicator && focusIndicator->isVisible(),
               "Tab focus should outline the complete file panel");

    const QPointF localPosition(4.0, 4.0);
    const QPointF globalPosition =
        panel->viewport()->mapToGlobal(localPosition.toPoint());
    QMouseEvent mousePress(QEvent::MouseButtonPress, localPosition,
                           globalPosition, Qt::LeftButton, Qt::LeftButton,
                           Qt::NoModifier);
    QApplication::sendEvent(panel->viewport(), &mousePress);
    flushUiEvents();
    test.check(!panel->property("keyboardFocusVisible").toBool() &&
                   focusIndicator && !focusIndicator->isVisible(),
               "using the mouse should hide the panel focus outline");
}

OPENSCP_TEST(testTransferQueueUsesNativeFocusAndAccessibleState, test) {
    TransferManager manager;
    TransferQueueDialog dialog(&manager);
    dialog.show();
    flushUiEvents();

    auto *table =
        dialog.findChild<QTableView *>(QStringLiteral("transferQueueTable"));
    test.check(table != nullptr, "the transfer table should be discoverable");
    if (!table)
        return;

    QAccessibleInterface *tableInterface =
        QAccessible::queryAccessibleInterface(table);
    test.check(table->focusPolicy() != Qt::NoFocus &&
                   table->styleSheet().isEmpty(),
               "the transfer table should retain the platform focus style");
    test.check(tableInterface && tableInterface->role() == QAccessible::Table &&
                   tableInterface->state().focusable &&
                   tableInterface->tableInterface(),
               "the transfer table should expose focus and table semantics");

    auto *summary =
        dialog.findChild<QWidget *>(QStringLiteral("transferQueueSummary"));
    auto *badge =
        dialog.findChild<QLabel *>(QStringLiteral("transferBadgeErrors"));
    test.check(summary && !summary->accessibleName().trimmed().isEmpty(),
               "the queue summary should have an accessible group name");
    test.check(badge && badge->frameShape() == QFrame::NoFrame &&
                   badge->styleSheet().isEmpty() &&
                   !badge->testAttribute(Qt::WA_SetPalette),
               "summary badges should be borderless and use the platform "
               "palette");

    QToolButton *allFilter = nullptr;
    for (QToolButton *button : dialog.findChildren<QToolButton *>()) {
        if (button->text() == TransferQueueDialog::tr("All")) {
            allFilter = button;
            break;
        }
    }
    QAccessibleInterface *filterInterface =
        allFilter ? QAccessible::queryAccessibleInterface(allFilter) : nullptr;
    test.check(allFilter && filterInterface &&
                   filterInterface->state().checkable &&
                   filterInterface->state().checked &&
                   filterInterface->actionInterface() &&
                   !allFilter->accessibleDescription().trimmed().isEmpty(),
               "queue filters should expose their checked state and action");

    QPushButton *closeButton = nullptr;
    for (QPushButton *button : dialog.findChildren<QPushButton *>()) {
        if (button->text() == TransferQueueDialog::tr("Close")) {
            closeButton = button;
            break;
        }
    }
    test.check(closeButton &&
                   !closeButton->accessibleDescription().trimmed().isEmpty(),
               "queue actions should explain their effect to assistive tools");
}

OPENSCP_TEST(testDisclosureSectionsExposeStateAndAction, test) {
    ConnectionDialog dialog;
    auto *section = dialog.findChild<QToolButton *>(
        QStringLiteral("connectionSecuritySectionToggle"));
    QAccessibleInterface *interface =
        section ? QAccessible::queryAccessibleInterface(section) : nullptr;
    test.check(section && interface && interface->state().checkable &&
                   !interface->state().checked &&
                   interface->actionInterface() &&
                   !section->accessibleName().trimmed().isEmpty() &&
                   !section->accessibleDescription().trimmed().isEmpty(),
               "collapsed disclosure sections should expose state and action");
    if (!section || !interface)
        return;

    section->click();
    flushUiEvents();
    interface = QAccessible::queryAccessibleInterface(section);
    test.check(interface && interface->state().checked,
               "expanded disclosure sections should expose their new state");
}

} // namespace

int main(int argc, char **argv) {
    QApplication application(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("OpenSCP-tests"));
    QApplication::setApplicationName(
        QStringLiteral("accessibility-widget-tests"));

    QTemporaryDir settingsRoot;
    if (!settingsRoot.isValid()) {
        std::cerr << "[FAIL] could not create isolated settings directory\n";
        return 1;
    }
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       settingsRoot.path());

    openscp::test::TestHarness harness("Accessible widgets");
    return harness.run();
}
