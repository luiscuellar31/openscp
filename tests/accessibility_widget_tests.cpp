#include "QtTestSupport.hpp"
#include "TestHarness.hpp"
#include "logic/common/AppSettings.hpp"
#include "logic/transfers/TransferManager.hpp"
#include "widgets/common/InputModalityTracker.hpp"
#include "widgets/common/ToolbarKeyboardNavigation.hpp"
#include "widgets/dialogs/AboutDialog.hpp"
#include "widgets/dialogs/ConnectionDialog.hpp"
#include "widgets/dialogs/SettingsDialog.hpp"
#include "widgets/dialogs/TransferQueueDialog.hpp"
#include "widgets/files/DragAwareTreeView.hpp"
#include "widgets/navigation/PathNavigationBar.hpp"

#include <QAccessible>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFont>
#include <QFormLayout>
#include <QFrame>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QTableView>
#include <QTextBrowser>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <iostream>
#include <limits>

namespace {

using openscp::testsupport::flushUiEvents;
using openscp::testsupport::waitUntil;

class OrderedFocusWindow final : public QWidget {
    public:
    QList<QWidget *> order;

    protected:
    bool focusNextPrevChild(bool next) override {
        return openscpui::moveKeyboardFocus(order, QApplication::focusWidget(),
                                            next);
    }
};

void sendKey(QWidget *target, Qt::Key key,
             Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QKeyEvent press(QEvent::KeyPress, key, modifiers);
    QApplication::sendEvent(target, &press);
    QKeyEvent release(QEvent::KeyRelease, key, modifiers);
    QApplication::sendEvent(target, &release);
    flushUiEvents();
}

QPoint textPosition(QLineEdit *display, int cursorPosition) {
    const int verticalCenter = display->height() / 2;
    for (int horizontalPosition = 0; horizontalPosition < display->width();
         ++horizontalPosition) {
        const QPoint candidate(horizontalPosition, verticalCenter);
        if (display->cursorPositionAt(candidate) == cursorPosition)
            return candidate;
    }
    return display->rect().center();
}

void sendMouseEvent(QWidget *target, QEvent::Type type, const QPoint &position,
                    Qt::MouseButton button, Qt::MouseButtons buttons) {
    const QPoint globalPosition = target->mapToGlobal(position);
    QMouseEvent event(type, QPointF(position), QPointF(globalPosition), button,
                      buttons, Qt::NoModifier);
    QApplication::sendEvent(target, &event);
    flushUiEvents();
}

class SettingsDialogFixture final {
    public:
    SettingsDialogFixture() {
        dialog.show();
        flushUiEvents();
        sections = dialog.findChild<QListWidget *>();
        pages = dialog.findChild<QStackedWidget *>();
    }

    bool valid() const { return sections && pages; }

    bool selectPage(int index) {
        if (!valid() || index < 0 || index >= sections->count() ||
            index >= pages->count()) {
            return false;
        }
        sections->setCurrentRow(index);
        return pages->currentIndex() == index;
    }

    QWidget *currentPage() const {
        QWidget *current = pages ? pages->currentWidget() : nullptr;
        auto *scroll = qobject_cast<QScrollArea *>(current);
        return scroll ? scroll->widget() : current;
    }

    QList<QFormLayout *> currentForms() const {
        QWidget *page = currentPage();
        return page ? page->findChildren<QFormLayout *>()
                    : QList<QFormLayout *>();
    }

    SettingsDialog dialog;
    QListWidget *sections = nullptr;
    QStackedWidget *pages = nullptr;
};

OPENSCP_TEST(testPathFieldPreservesAppearanceAndKeyboardNavigation, test) {
    openscpui::PathNavigationBar bar(openscpui::PathFlavor::Remote,
                                     QStringLiteral("/projects/reports"));
    bar.resize(480, bar.sizeHint().height());
    bar.show();
    flushUiEvents();

    auto *display = qobject_cast<QLineEdit *>(bar.keyboardFocusTarget());
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

    const QPointF pointerPosition(2.0, 2.0);
    const QPointF globalPointerPosition =
        bar.mapToGlobal(pointerPosition.toPoint());
    QMouseEvent pointerPress(QEvent::MouseButtonPress, pointerPosition,
                             globalPointerPosition, Qt::LeftButton,
                             Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&bar, &pointerPress);
    flushUiEvents();
    test.check(display->hasFocus() &&
                   !display->property("keyboardFocusVisible").toBool(),
               "pointer input elsewhere should hide the path focus frame");

    sendKey(display, Qt::Key_Left);
    test.check(!display->property("keyboardFocusVisible").toBool(),
               "non-traversal keys should not reveal the path focus frame");
    sendKey(display, Qt::Key_Escape, Qt::ShiftModifier);
    test.check(display->property("keyboardFocusVisible").toBool(),
               "Shift+Esc should reveal the current path focus frame");

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

    auto *display = qobject_cast<QLineEdit *>(bar.keyboardFocusTarget());
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

OPENSCP_TEST(testPathFieldClickRequiresMatchingPressAndRelease, test) {
    openscpui::PathNavigationBar bar(openscpui::PathFlavor::Remote,
                                     QStringLiteral("/projects/reports"));
    bar.resize(480, bar.sizeHint().height());
    bar.show();
    flushUiEvents();

    auto *display = qobject_cast<QLineEdit *>(bar.keyboardFocusTarget());
    test.check(display, "the path field should exist");
    if (!display)
        return;

    int navigationRequests = 0;
    QString requestedPath;
    QObject::connect(&bar, &openscpui::PathNavigationBar::pathRequested, &bar,
                     [&](const QString &path) {
                         ++navigationRequests;
                         requestedPath = path;
                     });

    const QPoint projects = textPosition(display, 4);
    sendMouseEvent(display, QEvent::MouseButtonRelease, projects,
                   Qt::LeftButton, Qt::NoButton);
    test.check(navigationRequests == 0,
               "a release without a matching press should do nothing");
    navigationRequests = 0;
    requestedPath.clear();

    sendMouseEvent(display, QEvent::MouseButtonPress, projects, Qt::LeftButton,
                   Qt::LeftButton);
    sendMouseEvent(display, QEvent::MouseButtonRelease, projects,
                   Qt::LeftButton, Qt::NoButton);
    test.check(navigationRequests == 1 &&
                   requestedPath == QStringLiteral("/projects"),
               "a click should activate the segment where it began");
}

OPENSCP_TEST(testPathFieldDragCancelsActivation, test) {
    openscpui::PathNavigationBar bar(openscpui::PathFlavor::Remote,
                                     QStringLiteral("/projects/reports"));
    bar.resize(480, bar.sizeHint().height());
    bar.show();
    flushUiEvents();

    auto *display = qobject_cast<QLineEdit *>(bar.keyboardFocusTarget());
    test.check(display, "the path field should exist");
    if (!display)
        return;

    int navigationRequests = 0;
    int openDialogRequests = 0;
    QObject::connect(&bar, &openscpui::PathNavigationBar::pathRequested, &bar,
                     [&](const QString &) { ++navigationRequests; });
    QObject::connect(&bar, &openscpui::PathNavigationBar::openDialogRequested,
                     &bar, [&] { ++openDialogRequests; });

    const QPoint projects = textPosition(display, 4);
    const QPoint reports = textPosition(display, 12);
    sendMouseEvent(display, QEvent::MouseButtonPress, projects, Qt::LeftButton,
                   Qt::LeftButton);
    sendMouseEvent(display, QEvent::MouseMove, reports, Qt::NoButton,
                   Qt::LeftButton);
    sendMouseEvent(display, QEvent::MouseMove, projects, Qt::NoButton,
                   Qt::LeftButton);
    sendMouseEvent(display, QEvent::MouseButtonRelease, projects,
                   Qt::LeftButton, Qt::NoButton);
    test.check(navigationRequests == 0 && openDialogRequests == 0,
               "dragging to another segment should cancel activation");

    const QPoint outside(-4, display->height() / 2);
    sendMouseEvent(display, QEvent::MouseButtonPress, projects, Qt::LeftButton,
                   Qt::LeftButton);
    sendMouseEvent(display, QEvent::MouseMove, outside, Qt::NoButton,
                   Qt::LeftButton);
    sendMouseEvent(display, QEvent::MouseMove, projects, Qt::NoButton,
                   Qt::LeftButton);
    sendMouseEvent(display, QEvent::MouseButtonRelease, projects,
                   Qt::LeftButton, Qt::NoButton);
    test.check(navigationRequests == 0 && openDialogRequests == 0,
               "dragging outside the field should cancel activation");
}

OPENSCP_TEST(testPathFieldDoubleClickActivatesOnce, test) {
    openscpui::PathNavigationBar bar(openscpui::PathFlavor::Remote,
                                     QStringLiteral("/projects/reports"));
    bar.resize(480, bar.sizeHint().height());
    bar.show();
    flushUiEvents();

    auto *display = qobject_cast<QLineEdit *>(bar.keyboardFocusTarget());
    test.check(display, "the path field should exist");
    if (!display)
        return;

    int navigationRequests = 0;
    QObject::connect(&bar, &openscpui::PathNavigationBar::pathRequested, &bar,
                     [&](const QString &) { ++navigationRequests; });

    const QPoint projects = textPosition(display, 4);
    sendMouseEvent(display, QEvent::MouseButtonPress, projects, Qt::LeftButton,
                   Qt::LeftButton);
    sendMouseEvent(display, QEvent::MouseButtonRelease, projects,
                   Qt::LeftButton, Qt::NoButton);
    sendMouseEvent(display, QEvent::MouseButtonDblClick, projects,
                   Qt::LeftButton, Qt::LeftButton);
    sendMouseEvent(display, QEvent::MouseButtonRelease, projects,
                   Qt::LeftButton, Qt::NoButton);

    test.check(navigationRequests == 1,
               "a double click should activate its segment exactly once");
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

    sendKey(panel, Qt::Key_Down);
    test.check(!panel->property("keyboardFocusVisible").toBool() &&
                   focusIndicator && !focusIndicator->isVisible(),
               "non-traversal keys should not reveal the panel focus outline");

    sendKey(panel, Qt::Key_Escape);
    test.check(!panel->property("keyboardFocusVisible").toBool() &&
                   focusIndicator && !focusIndicator->isVisible(),
               "Esc should not reveal the panel focus outline");

    sendKey(panel, Qt::Key_Escape, Qt::ShiftModifier);
    test.check(panel->property("keyboardFocusVisible").toBool() &&
                   focusIndicator && focusIndicator->isVisible(),
               "Shift+Esc should reveal the panel focus outline");
}

OPENSCP_TEST(testFilePanelScrollBarsFollowScrollActivity, test) {
    QStandardItemModel model(80, 1);
    for (int row = 0; row < model.rowCount(); ++row) {
        model.setData(model.index(row, 0),
                      QStringLiteral("long-file-name-%1.txt").arg(row));
    }

    DragAwareTreeView panel;
    panel.setModel(&model);
    panel.header()->setStretchLastSection(false);
    panel.header()->setSectionResizeMode(QHeaderView::Fixed);
    panel.header()->resizeSection(0, 600);
    panel.resize(240, 160);
    panel.show();
    flushUiEvents();

    test.check(panel.verticalScrollBar()->maximum() > 0 &&
                   panel.horizontalScrollBar()->maximum() > 0,
               "the fixture should require both file-panel scrollbars");
    test.check(panel.verticalScrollBarPolicy() == Qt::ScrollBarAsNeeded &&
                   panel.horizontalScrollBarPolicy() == Qt::ScrollBarAsNeeded,
               "file-panel scrollbars should be available when shown");

    const auto scrollBarsAreHidden = [&panel] {
        return panel.verticalScrollBarPolicy() == Qt::ScrollBarAlwaysOff &&
               panel.horizontalScrollBarPolicy() == Qt::ScrollBarAlwaysOff;
    };
    test.check(waitUntil(scrollBarsAreHidden, std::chrono::milliseconds(3000)),
               "file-panel scrollbars should hide after inactivity");

    const int initialScrollPosition = panel.verticalScrollBar()->value();
    const QPoint localPosition = panel.viewport()->rect().center();
    QWheelEvent wheelEvent(
        QPointF(localPosition),
        QPointF(panel.viewport()->mapToGlobal(localPosition)), QPoint(),
        QPoint(0, -120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase,
        false);
    QApplication::sendEvent(panel.viewport(), &wheelEvent);
    flushUiEvents();

    test.check(panel.verticalScrollBar()->value() > initialScrollPosition,
               "hiding the scrollbars should not disable file-panel scrolling");
    test.check(panel.verticalScrollBarPolicy() == Qt::ScrollBarAsNeeded &&
                   panel.horizontalScrollBarPolicy() == Qt::ScrollBarAsNeeded,
               "scrolling should reveal both file-panel scrollbars");
    test.check(waitUntil(scrollBarsAreHidden, std::chrono::milliseconds(3000)),
               "file-panel scrollbars should hide again after scrolling");
}

OPENSCP_TEST(testPointerDialogCloseDoesNotCreateKeyboardFocus, test) {
    QWidget window;
    auto *layout = new QVBoxLayout(&window);
    auto *beforePanel = new QLineEdit(&window);
    auto *panel = new DragAwareTreeView(&window);
    layout->addWidget(beforePanel);
    layout->addWidget(panel);
    window.show();
    flushUiEvents();

    auto *tracker = openscpui::inputModalityTracker();
    test.check(tracker != nullptr,
               "the input modality tracker should be available");
    if (!tracker)
        return;

    sendKey(beforePanel, Qt::Key_Escape, Qt::ShiftModifier);
    test.check(tracker->isKeyboardActive(),
               "Shift+Esc should activate keyboard focus cues");

    const QPointF titleBarPosition(4.0, 4.0);
    const QPointF titleBarGlobalPosition =
        window.mapToGlobal(titleBarPosition.toPoint());
    QMouseEvent nativeTitleBarPress(
        QEvent::NonClientAreaMouseButtonPress, titleBarPosition,
        titleBarGlobalPosition, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&window, &nativeTitleBarPress);
    beforePanel->setFocus(Qt::OtherFocusReason);
    panel->setFocus(Qt::TabFocusReason);
    flushUiEvents();

    test.check(!tracker->isKeyboardActive(),
               "a native title-bar click should activate pointer modality");
    test.check(!panel->property("keyboardFocusVisible").toBool(),
               "focus restoration after a pointer close should not look like "
               "Tab navigation");
}

OPENSCP_TEST(testAboutDialogOpensCompactStructuredCredits, test) {
    AboutDialog dialog;
    dialog.show();
    flushUiEvents();

    auto *creditsButton =
        dialog.findChild<QPushButton *>(QStringLiteral("aboutCreditsButton"));
    test.check(creditsButton != nullptr,
               "the About dialog should expose its credits on demand");
    if (!creditsButton)
        return;

    const QSize aboutMinimum = dialog.minimumSize();
    test.check(aboutMinimum.width() >= 440 && aboutMinimum.height() >= 260,
               "the About dialog should enforce its compact minimum size");
    test.check(dialog.size() == QSize(460, 270).expandedTo(aboutMinimum),
               "the About dialog should open at its compact preferred size");
    dialog.resize(1, 1);
    flushUiEvents();
    test.check(dialog.size() == aboutMinimum && creditsButton->isVisible() &&
                   dialog.contentsRect().contains(creditsButton->geometry()),
               "the About dialog should keep its actions usable at minimum "
               "size");

    bool structuredCreditsFound = false;
    bool compactCreditsFound = false;
    bool minimumCreditsFound = false;
    bool readableCreditsFound = false;
    QTimer::singleShot(
        0, &dialog,
        [&dialog, &structuredCreditsFound, &compactCreditsFound,
         &minimumCreditsFound, &readableCreditsFound] {
            auto *creditsDialog =
                dialog.findChild<QDialog *>(QStringLiteral("creditsDialog"));
            if (!creditsDialog)
                return;
            auto *componentList = creditsDialog->findChild<QListWidget *>(
                QStringLiteral("creditComponentList"));
            auto *details = creditsDialog->findChild<QTextBrowser *>(
                QStringLiteral("creditDetailsBrowser"));
            structuredCreditsFound = componentList &&
                                     componentList->count() > 0 && details &&
                                     !details->toPlainText().isEmpty();
            readableCreditsFound =
                componentList &&
                componentList
                        ->findItems(QStringLiteral("Qt 6"), Qt::MatchExactly)
                        .size() == 1 &&
                details && !details->toHtml().contains(QStringLiteral("<ul")) &&
                !details->toHtml().contains(QStringLiteral("<li"));
            const QSize creditsMinimum = creditsDialog->minimumSize();
            compactCreditsFound =
                creditsMinimum.width() >= 520 &&
                creditsMinimum.height() >= 340 &&
                creditsDialog->size() ==
                    QSize(560, 360).expandedTo(creditsMinimum);
            creditsDialog->resize(1, 1);
            flushUiEvents();
            minimumCreditsFound =
                creditsDialog->size() == creditsMinimum && componentList &&
                componentList->width() >= componentList->minimumWidth() &&
                details && details->width() > 0;
            creditsDialog->reject();
        });
    creditsButton->click();

    test.check(structuredCreditsFound,
               "credits should use a component list with focused details");
    test.check(compactCreditsFound,
               "the credits browser should open at its compact preferred "
               "size");
    test.check(minimumCreditsFound,
               "the credits browser should preserve both panes at minimum "
               "size");
    test.check(readableCreditsFound,
               "credits should use concise component names and field rows, "
               "not Markdown bullets");
}

OPENSCP_TEST(testTransferQueueUsesNativeFocusAndAccessibleState, test) {
    TransferManager manager;
    TransferQueueDialog dialog(&manager);
    dialog.resize(dialog.minimumSize());
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
                   tableInterface->tableInterface(),
               "the transfer table should expose table semantics");

    auto *summary =
        dialog.findChild<QWidget *>(QStringLiteral("transferQueueSummary"));
    auto *selectionActions = dialog.findChild<QWidget *>(
        QStringLiteral("transferQueueSelectionActions"));
    auto *globalActions = dialog.findChild<QWidget *>(
        QStringLiteral("transferQueueGlobalActions"));
    auto *emptyState =
        dialog.findChild<QLabel *>(QStringLiteral("transferQueueEmptyState"));
    auto *badge =
        dialog.findChild<QLabel *>(QStringLiteral("transferBadgeErrors"));
    test.check(summary && !summary->accessibleName().trimmed().isEmpty(),
               "the queue summary should have an accessible group name");
    test.check(badge && badge->frameShape() == QFrame::NoFrame &&
                   badge->styleSheet().isEmpty() &&
                   !badge->testAttribute(Qt::WA_SetPalette),
               "summary badges should be borderless and use the platform "
               "palette");
    test.check(selectionActions && globalActions &&
                   selectionActions->y() < summary->y() &&
                   summary->y() < globalActions->y(),
               "selection, summary, and queue actions should form a compact "
               "visual hierarchy");
    test.check(emptyState && emptyState->isVisible() &&
                   !emptyState->text().trimmed().isEmpty() &&
                   !emptyState->accessibleName().trimmed().isEmpty(),
               "an empty queue should explain the otherwise blank table");

    QToolButton *allFilter = nullptr;
    QToolButton *completedFilter = nullptr;
    for (QToolButton *button : dialog.findChildren<QToolButton *>()) {
        if (button->text() == TransferQueueDialog::tr("All"))
            allFilter = button;
        else if (button->text() == TransferQueueDialog::tr("Completed"))
            completedFilter = button;
    }
    QAccessibleInterface *filterInterface =
        allFilter ? QAccessible::queryAccessibleInterface(allFilter) : nullptr;
    test.check(allFilter && filterInterface &&
                   filterInterface->state().checkable &&
                   filterInterface->state().checked &&
                   filterInterface->actionInterface() &&
                   !allFilter->accessibleDescription().trimmed().isEmpty(),
               "queue filters should expose their checked state and action");

    manager.enqueueUpload(QStringLiteral("/tmp/source.txt"),
                          QStringLiteral("/remote/source.txt"));
    flushUiEvents();
    test.check(emptyState && table->model()->rowCount() == 1 &&
                   !emptyState->isVisible(),
               "the empty state should disappear when a transfer is added");
    if (completedFilter && allFilter && emptyState) {
        completedFilter->click();
        flushUiEvents();
        test.check(table->model()->rowCount() == 0 && emptyState->isVisible(),
                   "an empty filter should show its own explanatory state");
        allFilter->click();
        flushUiEvents();
    }

    auto *closeButton = dialog.findChild<QPushButton *>(
        QStringLiteral("transferQueueCloseButton"));
    auto *clearButton = dialog.findChild<QPushButton *>(
        QStringLiteral("transferQueueClearButton"));
    auto *optionsButton = dialog.findChild<QPushButton *>(
        QStringLiteral("transferQueueOptionsButton"));
    auto *optionsDialog = dialog.findChild<QDialog *>(
        QStringLiteral("transferQueueOptionsDialog"));
    auto *speedLimit =
        dialog.findChild<QSpinBox *>(QStringLiteral("transferQueueSpeedLimit"));
    auto *applyLimit = dialog.findChild<QPushButton *>(
        QStringLiteral("transferQueueApplyLimit"));
    auto *autoClearMode = dialog.findChild<QComboBox *>(
        QStringLiteral("transferQueueAutoClearMode"));
    auto *autoClearDelay = dialog.findChild<QSpinBox *>(
        QStringLiteral("transferQueueAutoClearDelay"));
    bool allActionLabelsFit = true;
    for (QPushButton *button : dialog.findChildren<QPushButton *>()) {
        if (!button->isVisible())
            continue;
        allActionLabelsFit =
            allActionLabelsFit && button->width() >= button->sizeHint().width();
    }
    test.check(closeButton &&
                   !closeButton->accessibleDescription().trimmed().isEmpty(),
               "queue actions should explain their effect to assistive tools");
    test.check(clearButton && clearButton->menu() &&
                   clearButton->menu()->actions().size() == 2 &&
                   !clearButton->accessibleDescription().trimmed().isEmpty(),
               "infrequent cleanup actions should share an accessible menu");
    test.check(optionsButton && optionsDialog && optionsDialog->layout() &&
                   optionsDialog->layout()->sizeConstraint() ==
                       QLayout::SetFixedSize &&
                   speedLimit && applyLimit && autoClearMode && autoClearDelay,
               "queue settings should live in a compact reusable dialog");
    if (speedLimit && applyLimit) {
        speedLimit->setValue(128);
        applyLimit->click();
        test.check(manager.globalSpeedLimitKBps() == 128,
                   "the relocated global speed control should remain wired");
        manager.setGlobalSpeedLimitKBps(0);
    }
    if (autoClearMode && autoClearDelay) {
        const int completedIndex = autoClearMode->findText(
            TransferQueueDialog::tr("Completed"), Qt::MatchExactly);
        autoClearMode->setCurrentIndex(completedIndex);
        flushUiEvents();
        test.check(completedIndex >= 0 && autoClearDelay->isEnabled(),
                   "automatic cleanup should still control its delay field");
    }
    bool optionsOpenedCompactly = false;
    if (optionsButton && optionsDialog) {
        const QString platformName = QGuiApplication::platformName();
        const bool canPresentModalWindow =
            platformName != QStringLiteral("offscreen") &&
            platformName != QStringLiteral("minimal");
        if (canPresentModalWindow) {
            optionsButton->click();
            flushUiEvents();
            optionsOpenedCompactly =
                optionsDialog->isVisible() && optionsDialog->isModal() &&
                optionsDialog->size().width() < dialog.size().width() &&
                optionsDialog->size().height() < dialog.size().height();
            optionsDialog->reject();
            flushUiEvents();
        } else {
            const QSize optionsSize = optionsDialog->sizeHint();
            optionsOpenedCompactly =
                optionsSize.width() < dialog.size().width() &&
                optionsSize.height() < dialog.size().height();
        }
    }
    test.check(optionsOpenedCompactly,
               "queue options should open as a compact modal dialog");
    test.check(allActionLabelsFit,
               "queue action labels should fit at the minimum window size");
}

OPENSCP_TEST(testSettingsRestoresDefaultStagingFolderOnApply, test) {
    const QString customRoot =
        QDir(QDir::tempPath())
            .filePath(QStringLiteral("openscp-custom-staging-test"));
    const QString defaultRoot = openscpui::defaultStagingRootPath();
    {
        openscpui::AppSettings settings;
        settings.clear();
        settings.setValue(openscpui::settingskeys::kStagingRoot, customRoot);
        settings.sync();
    }

    {
        SettingsDialog dialog;
        auto *path = dialog.findChild<QLineEdit *>(
            QStringLiteral("settingsStagingRoot"));
        auto *choose = dialog.findChild<QPushButton *>(
            QStringLiteral("settingsChooseStagingRoot"));
        auto *restore = dialog.findChild<QPushButton *>(
            QStringLiteral("settingsRestoreDefaultStagingRoot"));
        auto *apply = dialog.findChild<QPushButton *>(
            QStringLiteral("settingsApplyButton"));
        auto *sections = dialog.findChild<QListWidget *>();
        dialog.show();
        if (sections)
            sections->setCurrentRow(sections->count() - 1);
        flushUiEvents();
        test.check(
            path && choose && restore && apply && path->text() == customRoot &&
                path->toolTip() == customRoot && path->cursorPosition() == 0 &&
                restore->isEnabled() && !apply->isEnabled() &&
                !choose->autoDefault() && !choose->isDefault() &&
                !restore->autoDefault() && !restore->isDefault() &&
                !restore->text().trimmed().isEmpty() &&
                restore->icon().isNull() && !restore->isFlat() &&
                restore->style() == choose->style() &&
                !restore->accessibleName().trimmed().isEmpty() &&
                !restore->accessibleDescription().trimmed().isEmpty(),
            "staging actions should use matching native push buttons "
            "without taking the dialog default action");
        if (path && choose && restore && apply) {
            restore->click();
            test.check(path->text() == defaultRoot &&
                           path->toolTip() == defaultRoot &&
                           path->cursorPosition() == 0 &&
                           !restore->isEnabled() && apply->isEnabled() &&
                           apply->isDefault() && !choose->isDefault(),
                       "restoring should update only the field and mark "
                       "settings as modified");
        }
        dialog.reject();
    }

    {
        openscpui::AppSettings settings;
        test.check(
            settings.value(openscpui::settingskeys::kStagingRoot).toString() ==
                customRoot,
            "closing without Apply should preserve the custom path");
    }

    {
        SettingsDialog dialog;
        auto *restore = dialog.findChild<QPushButton *>(
            QStringLiteral("settingsRestoreDefaultStagingRoot"));
        auto *apply = dialog.findChild<QPushButton *>(
            QStringLiteral("settingsApplyButton"));
        if (restore && apply) {
            restore->click();
            apply->click();
        }
    }

    {
        openscpui::AppSettings settings;
        test.check(!settings.contains(openscpui::settingskeys::kStagingRoot) &&
                       openscpui::effectiveStagingRootPath(settings) ==
                           defaultRoot,
                   "Apply should remove the override and restore the central "
                   "default without touching the filesystem");
        settings.clear();
        settings.sync();
    }
}

OPENSCP_TEST(testSettingsDownloadDurabilityFailsSafeAndPersists, test) {
    {
        openscpui::AppSettings settings;
        settings.clear();
        settings.setValue(openscpui::settingskeys::kTransferLocalFileDurability,
                          999);
        settings.sync();
    }

    SettingsDialog dialog;
    auto *durability = dialog.findChild<QComboBox *>(
        QStringLiteral("settingsLocalFileDurability"));
    auto *apply =
        dialog.findChild<QPushButton *>(QStringLiteral("settingsApplyButton"));
    const int maximum =
        static_cast<int>(openscp::LocalFileDurability::FileAndDirectory);
    const int buffered =
        static_cast<int>(openscp::LocalFileDurability::Buffered);
    test.check(durability && apply &&
                   durability->currentData().toInt() == maximum,
               "invalid persisted durability should load as the safest mode");
    if (durability && apply) {
        durability->setCurrentIndex(durability->findData(buffered));
        test.check(apply->isEnabled(),
                   "changing download durability should enable Apply");
        apply->click();
    }

    openscpui::AppSettings settings;
    test.check(
        settings.value(openscpui::settingskeys::kTransferLocalFileDurability)
                .toInt() == buffered,
        "Apply should persist the selected download durability");
    settings.clear();
    settings.sync();
}

OPENSCP_TEST(testSettingsPagesUseSharedFormStructure, test) {
    SettingsDialogFixture fixture;
    test.check(fixture.valid(),
               "the Settings dialog should expose sections and pages");
    if (!fixture.valid())
        return;
    SettingsDialog &dialog = fixture.dialog;
    auto *sections = fixture.sections;
    auto *pages = fixture.pages;
    const QList<int> expectedSectionCounts = {2, 1, 2, 1, 3, 1, 2};
    int sectionCount = 0;
    bool pageStructureMatches = true;
    bool sectionStructureMatches = true;
    bool contentIsIndented = true;
    bool notesShareTheHeadingAxis = true;
    bool checksRideIndentedSpanningRows = true;
    int noteCount = 0;
    int checkCount = 0;
    QMargins sharedPageMargins;
    QMargins sharedFormMargins;
    int sharedPageSpacing = -1;
    int sharedSectionSpacing = -1;
    int sharedHorizontalSpacing = -1;
    int sharedVerticalSpacing = -1;

    if (sections && pages) {
        for (int pageIndex = 0; pageIndex < pages->count(); ++pageIndex) {
            fixture.selectPage(pageIndex);
            flushUiEvents();
            auto *scroll = qobject_cast<QScrollArea *>(pages->currentWidget());
            QWidget *page = fixture.currentPage();
            auto *pageLayout =
                page ? qobject_cast<QVBoxLayout *>(page->layout()) : nullptr;
            if (!scroll || !page || !pageLayout) {
                pageStructureMatches = false;
                continue;
            }

            QList<QWidget *> pageSections;
            for (QWidget *child : page->findChildren<QWidget *>(
                     QString(), Qt::FindDirectChildrenOnly)) {
                if (child->property("settingsSection").toBool())
                    pageSections.push_back(child);
            }
            if (pageIndex == 0) {
                sharedPageMargins = pageLayout->contentsMargins();
                sharedPageSpacing = pageLayout->spacing();
            }
            pageStructureMatches =
                pageStructureMatches &&
                scroll->property("settingsPageScroll").toBool() &&
                scroll->horizontalScrollBarPolicy() == Qt::ScrollBarAlwaysOff &&
                page->property("settingsFormPage").toBool() &&
                pageLayout->contentsMargins() == sharedPageMargins &&
                pageLayout->spacing() == sharedPageSpacing &&
                pageIndex < expectedSectionCounts.size() &&
                pageSections.size() == expectedSectionCounts.at(pageIndex);

            for (QWidget *section : pageSections) {
                ++sectionCount;
                auto *sectionLayout =
                    qobject_cast<QVBoxLayout *>(section->layout());
                auto *form = section->findChild<QFormLayout *>();
                QLabel *heading = nullptr;
                for (QLabel *candidate : section->findChildren<QLabel *>(
                         QString(), Qt::FindDirectChildrenOnly)) {
                    if (candidate->property("settingsSectionHeading")
                            .toBool()) {
                        heading = candidate;
                        break;
                    }
                }

                if (!sectionLayout || !form || !heading) {
                    sectionStructureMatches = false;
                    continue;
                }
                if (sharedHorizontalSpacing < 0) {
                    sharedFormMargins = form->contentsMargins();
                    sharedSectionSpacing = sectionLayout->spacing();
                    sharedHorizontalSpacing = form->horizontalSpacing();
                    sharedVerticalSpacing = form->verticalSpacing();
                }
                const int expectedIndent =
                    qMax(0, page->style()->pixelMetric(
                                QStyle::PM_LayoutLeftMargin, nullptr, page));
                sectionStructureMatches =
                    sectionStructureMatches && heading->font().bold() &&
                    sectionLayout->contentsMargins() == QMargins() &&
                    sectionLayout->spacing() == sharedSectionSpacing &&
                    sectionLayout->count() >= 2 &&
                    sectionLayout->itemAt(0)->widget() == heading &&
                    sectionLayout->itemAt(sectionLayout->count() - 1)
                            ->layout() == form &&
                    form->property("settingsFormLayout").toBool() &&
                    form->contentsMargins() == sharedFormMargins &&
                    form->contentsMargins().left() == expectedIndent &&
                    form->contentsMargins().top() == 0 &&
                    form->contentsMargins().right() == 0 &&
                    form->contentsMargins().bottom() == 0 &&
                    form->property("settingsSectionIndent").toInt() ==
                        expectedIndent &&
                    form->horizontalSpacing() == sharedHorizontalSpacing &&
                    form->verticalSpacing() == sharedVerticalSpacing &&
                    form->fieldGrowthPolicy() ==
                        QFormLayout::AllNonFixedFieldsGrow &&
                    form->rowWrapPolicy() == QFormLayout::WrapLongRows &&
                    form->labelAlignment().testFlag(Qt::AlignRight) &&
                    form->labelAlignment().testFlag(Qt::AlignVCenter) &&
                    form->formAlignment().testFlag(Qt::AlignLeft) &&
                    form->formAlignment().testFlag(Qt::AlignTop);

                int contentLeft = std::numeric_limits<int>::max();
                for (int row = 0; row < form->rowCount(); ++row) {
                    for (QFormLayout::ItemRole role :
                         {QFormLayout::LabelRole, QFormLayout::FieldRole,
                          QFormLayout::SpanningRole}) {
                        QLayoutItem *item = form->itemAt(row, role);
                        QWidget *widget = item ? item->widget() : nullptr;
                        if (widget) {
                            contentLeft =
                                qMin(contentLeft,
                                     widget->mapTo(&dialog, QPoint()).x());
                        }
                    }
                }
                for (int index = 1; index + 1 < sectionLayout->count();
                     ++index) {
                    auto *note = qobject_cast<QLabel *>(
                        sectionLayout->itemAt(index)->widget());
                    ++noteCount;
                    notesShareTheHeadingAxis =
                        notesShareTheHeadingAxis && note &&
                        note->property("settingsFormNote").toBool() &&
                        note->wordWrap() &&
                        note->mapTo(&dialog, QPoint()).x() ==
                            heading->mapTo(&dialog, QPoint()).x();
                }
                for (QCheckBox *check : section->findChildren<QCheckBox *>()) {
                    QWidget *checkRow = check->parentWidget();
                    int formRow = -1;
                    QFormLayout::ItemRole checkRole = QFormLayout::LabelRole;
                    if (checkRow &&
                        checkRow->property("settingsCheckRow").toBool()) {
                        form->getWidgetPosition(checkRow, &formRow, &checkRole);
                    }
                    ++checkCount;
                    checksRideIndentedSpanningRows =
                        checksRideIndentedSpanningRows && formRow >= 0 &&
                        checkRole == QFormLayout::SpanningRole &&
                        check->mapTo(&dialog, QPoint()).x() >
                            checkRow->mapTo(&dialog, QPoint()).x();
                }
                contentIsIndented =
                    contentIsIndented &&
                    contentLeft != std::numeric_limits<int>::max() &&
                    heading->mapTo(&dialog, QPoint()).x() < contentLeft;
            }
        }
    } else {
        pageStructureMatches = false;
    }

    test.check(pages && pages->count() == 7 && pageStructureMatches,
               "every settings page should use the same responsive form base");
    test.check(sectionCount == 12 && sectionStructureMatches &&
                   contentIsIndented,
               "every section should lead one consistently indented form");
    test.check(noteCount == 1 && notesShareTheHeadingAxis,
               "settings guidance should align with its section heading");
    test.check(checkCount > 0 && checksRideIndentedSpanningRows,
               "settings checks should span their forms from the field axis");
}

OPENSCP_TEST(testSettingsInlineActionPreservesNativeBounds, test) {
    SettingsDialogFixture fixture;
    test.check(fixture.valid(),
               "the Settings dialog should expose sections and pages");
    if (!fixture.valid())
        return;
    SettingsDialog &dialog = fixture.dialog;

    auto *button = dialog.findChild<QPushButton *>(
        QStringLiteral("settingsRestoreDefaultLayout"));
    QWidget *section = button ? button->parentWidget() : nullptr;
    while (section && !section->property("settingsSection").toBool())
        section = section->parentWidget();
    QWidget *row = button ? button->parentWidget() : nullptr;
    auto *rowLayout =
        row ? qobject_cast<QHBoxLayout *>(row->layout()) : nullptr;
    const QRect buttonInSection =
        button && section
            ? QRect(button->mapTo(section, QPoint()), button->size())
            : QRect();

    test.check(
        button && section && row && row != section && rowLayout &&
            rowLayout->contentsMargins() == QMargins() &&
            row->rect().contains(button->geometry()) &&
            section->rect().contains(buttonInSection) &&
            button->visibleRegion().boundingRect().contains(button->rect()),
        "native inline actions should remain fully inside their row "
        "and section bounds");
}

OPENSCP_TEST(testSettingsPageSwitchKeepsWrappedRowsStable, test) {
    SettingsDialogFixture fixture;
    test.check(fixture.valid(),
               "the Settings dialog should expose sections and pages");
    if (!fixture.valid())
        return;
    SettingsDialog &dialog = fixture.dialog;
    auto *sections = fixture.sections;
    auto *pages = fixture.pages;
    QList<int> changes;
    if (pages) {
        QObject::connect(pages, &QStackedWidget::currentChanged, &dialog,
                         [&changes](int index) { changes.push_back(index); });
    }

    bool switchesAreStable = sections && pages;
    int inspectedSwitches = 0;
    const auto switchToPage = [&](int index) {
        changes.clear();
        const bool selected = fixture.selectPage(index);

        // The wrap a page carries when it is switched in has to be its final
        // one: a correction landing afterwards is the reflow users see.
        QList<QPair<QCheckBox *, QPair<int, int>>> rowsOnArrival;
        if (pages && pages->currentWidget()) {
            for (QCheckBox *check :
                 pages->currentWidget()->findChildren<QCheckBox *>()) {
                if (check->isVisible()) {
                    rowsOnArrival.push_back(
                        {check, {check->minimumHeight(), check->height()}});
                }
            }
        }
        switchesAreStable = switchesAreStable && selected &&
                            pages->currentIndex() == index &&
                            changes == QList<int>{index};

        flushUiEvents();
        for (const auto &[check, onArrival] : rowsOnArrival) {
            ++inspectedSwitches;
            switchesAreStable =
                switchesAreStable && check && check->isVisible() &&
                onArrival.first == check->minimumHeight() &&
                onArrival.second <=
                    check->height() + check->fontMetrics().lineSpacing();
        }
    };

    for (int index : {1, 5, 3, 6, 0, 4, 2})
        switchToPage(index);

    // A resize leaves the pages that are off screen holding the wrap they had
    // at the previous width, so switching after one is the harder case.
    for (const QSize &size :
         {QSize(1180, 760), QSize(760, 560), QSize(860, 620)}) {
        dialog.resize(size);
        flushUiEvents();
        for (int index : {4, 6, 0, 3})
            switchToPage(index);
    }

    test.check(switchesAreStable && inspectedSwitches > 0,
               "switching settings pages should select only the requested "
               "page and paint it at its settled size");
}

OPENSCP_TEST(testSettingsOpensWithoutResettlingWrappedRows, test) {
    SettingsDialog dialog;
    dialog.resize(860, 620);
    dialog.show();

    QList<QPair<QCheckBox *, int>> paintedHeights;
    for (QCheckBox *check : dialog.findChildren<QCheckBox *>()) {
        if (check->isVisible())
            paintedHeights.push_back({check, check->height()});
    }
    flushUiEvents();

    bool opensSettled = !paintedHeights.isEmpty();
    for (const auto &[check, paintedHeight] : paintedHeights)
        opensSettled = opensSettled && check->height() == paintedHeight;

    test.check(opensSettled,
               "opening Settings should paint its wrapped rows at the size "
               "they keep");
}

OPENSCP_TEST(testSettingsLabelsAndFieldsShareAxes, test) {
    SettingsDialogFixture fixture;
    test.check(fixture.valid(),
               "the Settings dialog should expose sections and pages");
    if (!fixture.valid())
        return;
    SettingsDialog &dialog = fixture.dialog;
    auto *sections = fixture.sections;
    auto *pages = fixture.pages;
    int sharedLabelWidth = -1;
    int sharedFieldAxis = -1;
    int formLabelCount = 0;
    bool labelsShareAxis = true;
    bool fieldsShareAxis = true;
    bool labelBuddiesMatch = true;
    bool networkUsesSharedRoles = false;
    bool checksShareTheFieldAxis = true;
    int checkAxisCount = 0;

    for (int pageIndex = 0; sections && pages && pageIndex < pages->count();
         ++pageIndex) {
        fixture.selectPage(pageIndex);
        flushUiEvents();
        const QList<QFormLayout *> forms = fixture.currentForms();
        if (forms.isEmpty()) {
            fieldsShareAxis = false;
            continue;
        }
        for (QFormLayout *form : forms) {
            for (int row = 0; row < form->rowCount(); ++row) {
                QLayoutItem *labelItem =
                    form->itemAt(row, QFormLayout::LabelRole);
                QLayoutItem *fieldItem =
                    form->itemAt(row, QFormLayout::FieldRole);
                auto *label = labelItem
                                  ? qobject_cast<QLabel *>(labelItem->widget())
                                  : nullptr;
                QWidget *field = fieldItem ? fieldItem->widget() : nullptr;
                if (!label || !field ||
                    !label->property("settingsFormLabel").toBool()) {
                    continue;
                }

                ++formLabelCount;
                if (sharedLabelWidth < 0)
                    sharedLabelWidth = label->width();
                labelsShareAxis =
                    labelsShareAxis &&
                    qAbs(label->width() - sharedLabelWidth) <= 1 &&
                    label->minimumWidth() == label->maximumWidth() &&
                    label->alignment().testFlag(Qt::AlignRight) &&
                    label->alignment().testFlag(Qt::AlignVCenter) &&
                    form->labelAlignment().testFlag(Qt::AlignVCenter);
                QWidget *buddy = label->buddy();
                labelBuddiesMatch =
                    labelBuddiesMatch && buddy &&
                    (buddy == field || field->isAncestorOf(buddy));

                const int fieldAxis = field->mapTo(&dialog, QPoint()).x();
                if (sharedFieldAxis < 0)
                    sharedFieldAxis = fieldAxis;
                fieldsShareAxis =
                    fieldsShareAxis && qAbs(fieldAxis - sharedFieldAxis) <= 4;
                if (label->text() ==
                    SettingsDialog::tr("Session health check interval:")) {
                    int labelRow = -1;
                    QFormLayout::ItemRole labelRole = QFormLayout::SpanningRole;
                    form->getWidgetPosition(label, &labelRow, &labelRole);
                    networkUsesSharedRoles =
                        labelRole == QFormLayout::LabelRole &&
                        fieldItem ==
                            form->itemAt(labelRow, QFormLayout::FieldRole) &&
                        field->property("settingsFieldRole").toString() ==
                            QStringLiteral("compact") &&
                        label->alignment().testFlag(Qt::AlignRight) &&
                        qAbs(fieldAxis - sharedFieldAxis) <= 4;
                }
            }
        }

        QWidget *page = fixture.currentPage();
        for (QCheckBox *check :
             page ? page->findChildren<QCheckBox *>() : QList<QCheckBox *>()) {
            if (sharedFieldAxis < 0)
                continue;
            ++checkAxisCount;
            checksShareTheFieldAxis = checksShareTheFieldAxis &&
                                      qAbs(check->mapTo(&dialog, QPoint()).x() -
                                           sharedFieldAxis) <= 4;
        }
    }

    test.check(formLabelCount >= 15 && sharedLabelWidth > 0 &&
                   labelsShareAxis && fieldsShareAxis && labelBuddiesMatch,
               "settings labels and fields should share common visual axes");
    test.check(checkAxisCount > 0 && checksShareTheFieldAxis,
               "label-less checks should start on the shared field axis");
    test.check(networkUsesSharedRoles,
               "Network should use the shared label and field columns");
}

OPENSCP_TEST(testSettingsPathRowsUseNativeVerticalGeometry, test) {
    SettingsDialogFixture fixture;
    test.check(fixture.valid(),
               "the Settings dialog should expose sections and pages");
    if (!fixture.valid())
        return;
    SettingsDialog &dialog = fixture.dialog;
    for (int pageIndex = 0; pageIndex < fixture.pages->count(); ++pageIndex) {
        fixture.selectPage(pageIndex);
        flushUiEvents();
    }

    int pathCount = 0;
    bool rowsAreAligned = true;
    for (QWidget *widget : dialog.findChildren<QWidget *>()) {
        if (widget->property("settingsFieldRole").toString() !=
            QStringLiteral("path")) {
            continue;
        }
        auto *edit = qobject_cast<QLineEdit *>(widget);
        QWidget *rowWidget = edit ? edit->parentWidget() : nullptr;
        auto *row = rowWidget ? qobject_cast<QHBoxLayout *>(rowWidget->layout())
                              : nullptr;
        QLabel *label = nullptr;
        for (QLabel *candidate : dialog.findChildren<QLabel *>()) {
            if (candidate->buddy() == edit) {
                label = candidate;
                break;
            }
        }
        if (!edit || !rowWidget || !row || !label) {
            rowsAreAligned = false;
            continue;
        }

        ++pathCount;
        QLineEdit reference;
        reference.setFont(edit->font());
        const auto centerTwice = [&dialog](const QWidget *control) {
            return control->mapTo(&dialog, QPoint()).y() * 2 +
                   control->height();
        };
        rowsAreAligned =
            rowsAreAligned && row->contentsMargins() == QMargins() &&
            label->alignment().testFlag(Qt::AlignVCenter) &&
            edit->sizeHint().height() == reference.sizeHint().height() &&
            edit->minimumSizeHint().height() ==
                reference.minimumSizeHint().height() &&
            qAbs(centerTwice(edit) - centerTwice(label)) <= 4;
        for (QPushButton *button : rowWidget->findChildren<QPushButton *>()) {
            rowsAreAligned =
                rowsAreAligned &&
                qAbs(centerTwice(edit) - centerTwice(button)) <= 4 &&
                button->height() >= button->sizeHint().height();
        }
    }

    test.check(pathCount == 3 && rowsAreAligned,
               "path rows should preserve native heights and vertical centers");
}

OPENSCP_TEST(testSettingsControlsUseSharedWidthRoles, test) {
    SettingsDialogFixture fixture;
    test.check(fixture.valid(),
               "the Settings dialog should expose sections and pages");
    if (!fixture.valid())
        return;
    SettingsDialog &dialog = fixture.dialog;
    for (int pageIndex = 0; pageIndex < fixture.pages->count(); ++pageIndex) {
        fixture.selectPage(pageIndex);
        flushUiEvents();
    }

    int standardFieldCount = 0;
    int compactFieldCount = 0;
    int pathFieldCount = 0;
    int sharedCompactWidth = -1;
    bool fieldRolesMatch = true;
    for (QWidget *widget : dialog.findChildren<QWidget *>()) {
        const QString role = widget->property("settingsFieldRole").toString();
        if (role == QStringLiteral("standard")) {
            ++standardFieldCount;
            fieldRolesMatch = fieldRolesMatch &&
                              widget->sizePolicy().horizontalPolicy() ==
                                  QSizePolicy::Expanding &&
                              widget->maximumWidth() < QWIDGETSIZE_MAX;
        } else if (role == QStringLiteral("compact")) {
            ++compactFieldCount;
            if (sharedCompactWidth < 0)
                sharedCompactWidth = widget->width();
            fieldRolesMatch = fieldRolesMatch &&
                              widget->sizePolicy().horizontalPolicy() ==
                                  QSizePolicy::Preferred &&
                              widget->maximumWidth() < QWIDGETSIZE_MAX &&
                              widget->width() == sharedCompactWidth;
        } else if (role == QStringLiteral("path")) {
            auto *edit = qobject_cast<QLineEdit *>(widget);
            ++pathFieldCount;
            fieldRolesMatch =
                fieldRolesMatch && edit && edit->toolTip() == edit->text() &&
                !edit->accessibleName().trimmed().isEmpty() &&
                !edit->accessibleDescription().trimmed().isEmpty() &&
                widget->sizePolicy().horizontalPolicy() ==
                    QSizePolicy::Expanding &&
                widget->maximumWidth() == QWIDGETSIZE_MAX &&
                widget->property("settingsElidesPath").toBool();
        }
    }

    int pathRowCount = 0;
    bool pathRowsYieldWidth = true;
    for (QWidget *widget : dialog.findChildren<QWidget *>()) {
        if (!widget->property("settingsPathRow").toBool())
            continue;
        ++pathRowCount;
        pathRowsYieldWidth =
            pathRowsYieldWidth &&
            widget->sizePolicy().horizontalPolicy() == QSizePolicy::Ignored;
    }

    int inlineActionCount = 0;
    bool inlineActionsAreSecondary = true;
    for (const QPushButton *button : dialog.findChildren<QPushButton *>()) {
        if (!button->property("settingsInlineAction").toBool())
            continue;
        ++inlineActionCount;
        inlineActionsAreSecondary =
            inlineActionsAreSecondary && !button->autoDefault() &&
            !button->isDefault() &&
            button->sizePolicy().horizontalPolicy() == QSizePolicy::Maximum &&
            button->width() >= button->sizeHint().width();
    }

    test.check(
        standardFieldCount > 0 && compactFieldCount > 0 &&
            pathFieldCount == 3 && pathRowCount == 3 && fieldRolesMatch &&
            pathRowsYieldWidth,
        "settings controls should use standard, compact, and path roles");
    test.check(inlineActionCount >= 5 && inlineActionsAreSecondary,
               "inline settings actions should fit and remain secondary");
}

OPENSCP_TEST(testSettingsPathsStayResponsiveAndPreserveValues, test) {
    SettingsDialogFixture fixture;
    test.check(fixture.valid(),
               "the Settings dialog should expose sections and pages");
    if (!fixture.valid())
        return;
    SettingsDialog &dialog = fixture.dialog;
    auto *pages = fixture.pages;
    bool responsiveWithoutHorizontalScroll = true;
    QLabel *longTranslatedLabel = nullptr;
    for (QLabel *label : dialog.findChildren<QLabel *>()) {
        if (label->text() ==
            SettingsDialog::tr("Session health check interval:")) {
            longTranslatedLabel = label;
            label->setText(QStringLiteral(
                "Long translated session health check interval setting:"));
            break;
        }
    }
    const QList<QSize> dialogSizes = {dialog.minimumSize(), QSize(860, 620),
                                      QSize(1180, 760)};
    for (const QSize &size : dialogSizes) {
        dialog.resize(size);
        for (int pageIndex = 0; pageIndex < pages->count(); ++pageIndex) {
            fixture.selectPage(pageIndex);
            flushUiEvents();
            auto *scroll = qobject_cast<QScrollArea *>(pages->currentWidget());
            responsiveWithoutHorizontalScroll =
                responsiveWithoutHorizontalScroll && scroll &&
                scroll->horizontalScrollBar()->maximum() == 0;
        }
    }
    test.check(responsiveWithoutHorizontalScroll && longTranslatedLabel &&
                   longTranslatedLabel->height() >
                       longTranslatedLabel->fontMetrics().height(),
               "settings pages should not need horizontal scrolling");

    auto *path =
        dialog.findChild<QLineEdit *>(QStringLiteral("settingsStagingRoot"));
    auto *apply =
        dialog.findChild<QPushButton *>(QStringLiteral("settingsApplyButton"));
    if (path) {
        const QString longPath = QStringLiteral("/very/long/settings/path/") +
                                 QString(240, QLatin1Char('x')) +
                                 QStringLiteral("/folder");
        if (auto *close = dialog.findChild<QPushButton *>(
                QStringLiteral("settingsCloseButton"))) {
            close->setFocus();
        }
        path->setText(longPath);
        path->repaint();
        flushUiEvents();
        test.check(
            path->fontMetrics().horizontalAdvance(longPath) >
                    path->contentsRect().width() &&
                path->property("settingsElidesPath").toBool() &&
                path->text() == longPath && path->toolTip() == longPath &&
                path->cursorPosition() == 0 && path->selectedText().isEmpty() &&
                !path->accessibleName().trimmed().isEmpty() &&
                !path->accessibleDescription().trimmed().isEmpty(),
            "an elided path should preserve its full editable value and "
            "accessible context");
    }

    if (path && apply) {
        test.check(apply->isEnabled() && apply->isDefault(),
                   "Apply should become the only default action when dirty");
    }
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

    openscp::testsupport::IsolatedSettings settingsRoot;
    if (!settingsRoot.isValid()) {
        std::cerr << "[FAIL] could not create isolated settings directory\n";
        return 1;
    }
    openscp::test::TestHarness harness("Accessible widgets");
    const int result = harness.run();
    openscp::testsupport::drainThreadPool();
    return result;
}
