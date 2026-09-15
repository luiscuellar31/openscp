#include "QtTestSupport.hpp"
#include "TestHarness.hpp"
#include "app/MainWindow.hpp"
#include "logic/common/AppSettings.hpp"
#include "widgets/common/ToolbarKeyboardNavigation.hpp"
#include "widgets/dialogs/TransferQueueDialog.hpp"
#include "widgets/files/DragAwareTreeView.hpp"
#include "widgets/navigation/PathNavigationBar.hpp"

#include <QAbstractAnimation>
#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPushButton>
#include <QSplitter>
#include <QSplitterHandle>
#include <QStandardPaths>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>

#include <iostream>

namespace {

QString settingsRootPath;
using openscp::testsupport::flushUiEvents;

void configureMainWindowSettings(const QString &runtimeRoot) {
    openscpui::AppSettings settings;
    settings.clear();
    settings.setValue(openscpui::settingskeys::kUiShowConnectionOnStart, false);
    settings.setValue(openscpui::settingskeys::kUiOpenSiteManagerOnDisconnect,
                      false);
    settings.setValue(openscpui::settingskeys::kUiDefaultDownloadDir,
                      QDir(runtimeRoot).filePath(QStringLiteral("downloads")));
    settings.setValue(openscpui::settingskeys::kStagingRoot,
                      QDir(runtimeRoot).filePath(QStringLiteral("staging")));
    settings.sync();
}

void sendTab(QWidget *target, bool forward = true) {
    const Qt::Key key = forward ? Qt::Key_Tab : Qt::Key_Backtab;
    const Qt::KeyboardModifiers modifiers =
        forward ? Qt::NoModifier : Qt::ShiftModifier;
    QKeyEvent press(QEvent::KeyPress, key, modifiers);
    QApplication::sendEvent(target, &press);
    QKeyEvent release(QEvent::KeyRelease, key, modifiers);
    QApplication::sendEvent(target, &release);
    flushUiEvents();
}

void appendAvailable(QList<QWidget *> &target,
                     const QList<QWidget *> &candidates) {
    for (QWidget *candidate : candidates) {
        if (candidate && candidate->isVisible() && candidate->isEnabled() &&
            (candidate->focusPolicy() & Qt::TabFocus)) {
            target.push_back(candidate);
        }
    }
}

struct MainWindowFocusParts {
    QToolBar *mainToolbar = nullptr;
    QToolBar *leftToolbar = nullptr;
    QToolBar *rightToolbar = nullptr;
    QAction *connectAction = nullptr;
    openscpui::PathNavigationBar *leftPath = nullptr;
    openscpui::PathNavigationBar *rightPath = nullptr;
    DragAwareTreeView *leftView = nullptr;
    DragAwareTreeView *rightView = nullptr;
    QPushButton *scpUpload = nullptr;
    QPushButton *scpDownload = nullptr;
};

MainWindowFocusParts focusParts(MainWindow &window) {
    return {
        window.findChild<QToolBar *>(QStringLiteral("mainToolbar")),
        window.findChild<QToolBar *>(QStringLiteral("leftPaneToolbar")),
        window.findChild<QToolBar *>(QStringLiteral("rightPaneToolbar")),
        window.findChild<QAction *>(QStringLiteral("connectAction")),
        window.findChild<openscpui::PathNavigationBar *>(
            QStringLiteral("leftPathNavigationBar")),
        window.findChild<openscpui::PathNavigationBar *>(
            QStringLiteral("rightPathNavigationBar")),
        window.findChild<DragAwareTreeView *>(QStringLiteral("leftFileView")),
        window.findChild<DragAwareTreeView *>(QStringLiteral("rightFileView")),
        window.findChild<QPushButton *>(QStringLiteral("scpQuickUploadButton")),
        window.findChild<QPushButton *>(
            QStringLiteral("scpQuickDownloadButton"))};
}

QList<QWidget *> expectedFocusOrder(const MainWindowFocusParts &parts) {
    QList<QWidget *> order;
    appendAvailable(order,
                    openscpui::toolbarKeyboardFocusWidgets(parts.mainToolbar));
    appendAvailable(order,
                    openscpui::toolbarKeyboardFocusWidgets(parts.leftToolbar));
    appendAvailable(order,
                    {parts.leftPath->keyboardFocusTarget(), parts.leftView});
    appendAvailable(order,
                    openscpui::toolbarKeyboardFocusWidgets(parts.rightToolbar));
    appendAvailable(order,
                    {parts.rightPath->keyboardFocusTarget(), parts.rightView,
                     parts.scpUpload, parts.scpDownload});
    return order;
}

bool hasAllFocusParts(const MainWindowFocusParts &parts) {
    return parts.mainToolbar && parts.leftToolbar && parts.rightToolbar &&
           parts.connectAction && parts.leftPath && parts.rightPath &&
           parts.leftView && parts.rightView && parts.scpUpload &&
           parts.scpDownload;
}

OPENSCP_TEST(testMainWindowSplitterPreservesAnEvenPanelResize, test) {
    configureMainWindowSettings(settingsRootPath);
    MainWindow window;
    window.resize(900, 560);
    window.show();
    flushUiEvents();

    auto *splitter = qobject_cast<QSplitter *>(window.centralWidget());
    test.check(splitter && splitter->count() == 2,
               "the main window should expose its two-panel splitter");
    if (!splitter || splitter->count() != 2)
        return;

    const QList<int> initialSizes = splitter->sizes();
    test.check(qAbs(initialSizes[0] - initialSizes[1]) <= 2,
               "the default file panels should start with an even split");

    const MainWindowFocusParts parts = focusParts(window);
    test.check(parts.leftView && parts.rightView,
               "the splitter regression should use both file panels");
    if (!parts.leftView || !parts.rightView)
        return;

    const bool scrollBarsHidden = openscp::testsupport::waitUntil(
        [&parts] {
            return parts.leftView->verticalScrollBarPolicy() ==
                       Qt::ScrollBarAlwaysOff &&
                   parts.rightView->verticalScrollBarPolicy() ==
                       Qt::ScrollBarAlwaysOff;
        },
        std::chrono::milliseconds(3000));
    test.check(scrollBarsHidden,
               "the file-panel scrollbars should auto-hide in the main window");

    const QList<int> sizesAfterScrollBarsHide = splitter->sizes();
    test.check(
        qAbs(sizesAfterScrollBarsHide[0] - sizesAfterScrollBarsHide[1]) <= 2,
        "auto-hiding scrollbars should not change the panel split");

    QSplitterHandle *handle = splitter->handle(1);
    test.check(handle && handle->width() >= 8 &&
                   handle->cursor().shape() == Qt::SplitHCursor,
               "the panel divider should provide a usable resize-cursor area");

    window.resize(1300, 760);
    flushUiEvents();
    const QList<int> resizedSizes = splitter->sizes();
    test.check(
        qAbs(resizedSizes[0] - resizedSizes[1]) <= 2,
        "an even panel split should remain even when the window resizes");
}

OPENSCP_TEST(testMainWindowPreservesAnExplicitPanelSplit, test) {
    configureMainWindowSettings(settingsRootPath);
    QList<int> expectedSizes;
    {
        MainWindow window;
        window.resize(1000, 600);
        window.show();
        flushUiEvents();

        auto *splitter = qobject_cast<QSplitter *>(window.centralWidget());
        test.check(
            splitter && splitter->count() == 2,
            "the custom-layout fixture should expose the panel splitter");
        if (!splitter || splitter->count() != 2)
            return;

        splitter->setSizes({350, 650});
        flushUiEvents();
        expectedSizes = splitter->sizes();
        window.close();
        flushUiEvents();
    }

    MainWindow restoredWindow;
    restoredWindow.show();
    flushUiEvents();
    auto *restoredSplitter =
        qobject_cast<QSplitter *>(restoredWindow.centralWidget());
    test.check(restoredSplitter && restoredSplitter->count() == 2,
               "the restored window should expose the panel splitter");
    if (!restoredSplitter || restoredSplitter->count() != 2)
        return;

    const QList<int> restoredSizes = restoredSplitter->sizes();
    const double expectedRatio = static_cast<double>(expectedSizes[0]) /
                                 (expectedSizes[0] + expectedSizes[1]);
    const double restoredRatio = static_cast<double>(restoredSizes[0]) /
                                 (restoredSizes[0] + restoredSizes[1]);
    test.check(qAbs(restoredRatio - expectedRatio) < 0.01,
               "an explicitly adjusted panel split should remain persisted");
}

OPENSCP_TEST(testMainWindowTraversesItsCompleteVisibleFocusOrder, test) {
    configureMainWindowSettings(settingsRootPath);
    MainWindow window;
    window.resize(760, 520);
    window.show();
    flushUiEvents();

    const MainWindowFocusParts parts = focusParts(window);
    test.check(hasAllFocusParts(parts),
               "the production MainWindow should expose its focus landmarks");
    if (!hasAllFocusParts(parts))
        return;

    parts.rightToolbar->setMaximumWidth(220);
    flushUiEvents();
    QToolButton *overflow =
        openscpui::toolbarPopupExtensionButton(parts.rightToolbar);
    test.check(overflow && overflow->isVisible(),
               "the real focus order should include a dynamic overflow");

    const QList<QWidget *> expected = expectedFocusOrder(parts);
    QWidget *connectButton =
        parts.mainToolbar->widgetForAction(parts.connectAction);
    test.check(!expected.isEmpty() && expected.first() == connectButton &&
                   expected.contains(parts.leftPath->keyboardFocusTarget()) &&
                   expected.contains(parts.leftView) &&
                   expected.contains(parts.rightPath->keyboardFocusTarget()) &&
                   expected.contains(parts.rightView) &&
                   expected.contains(overflow),
               "the focus sequence should cover Connect, both panels and "
               "overflow controls");
    if (expected.isEmpty())
        return;

    QWidget *receiver = QApplication::focusWidget();
    if (!receiver || receiver->window() != &window)
        receiver = &window;
    for (QWidget *expectedWidget : expected) {
        sendTab(receiver);
        test.check(QApplication::focusWidget() == expectedWidget,
                   "Tab should follow the complete MainWindow focus order");
        receiver = expectedWidget;
    }
    sendTab(receiver);
    test.check(QApplication::focusWidget() == expected.first(),
               "the complete MainWindow focus order should wrap once");
}

OPENSCP_TEST(testPointerInteractionCancelsInitialConnectOverride, test) {
    configureMainWindowSettings(settingsRootPath);
    MainWindow window;
    window.resize(900, 560);
    window.show();
    flushUiEvents();

    const MainWindowFocusParts parts = focusParts(window);
    test.check(hasAllFocusParts(parts),
               "the production MainWindow should expose its focus landmarks");
    if (!hasAllFocusParts(parts))
        return;

    const QPointF localPosition(4.0, 4.0);
    const QPointF globalPosition =
        parts.leftView->viewport()->mapToGlobal(localPosition.toPoint());
    QMouseEvent mousePress(QEvent::MouseButtonPress, localPosition,
                           globalPosition, Qt::LeftButton, Qt::LeftButton,
                           Qt::NoModifier);
    QApplication::sendEvent(parts.leftView->viewport(), &mousePress);
    parts.leftView->setFocus(Qt::MouseFocusReason);
    flushUiEvents();
    test.check(QApplication::focusWidget() == parts.leftView,
               "the simulated pointer interaction should focus the left panel");

    QList<QWidget *> rightToolbarOrder;
    appendAvailable(rightToolbarOrder,
                    openscpui::toolbarKeyboardFocusWidgets(parts.rightToolbar));
    test.check(!rightToolbarOrder.isEmpty(),
               "the right toolbar should have an available focus target");
    if (rightToolbarOrder.isEmpty())
        return;

    sendTab(parts.leftView);
    QWidget *connectButton =
        parts.mainToolbar->widgetForAction(parts.connectAction);
    test.check(QApplication::focusWidget() == rightToolbarOrder.first() &&
                   QApplication::focusWidget() != connectButton,
               "Tab after a pointer interaction should continue from the "
               "panel instead of restarting at Connect");
}

OPENSCP_TEST(testSettingsCloseRestoresPointerFocusWithoutOutline, test) {
    configureMainWindowSettings(settingsRootPath);
    MainWindow window;
    window.resize(900, 560);
    window.show();
    flushUiEvents();

    const MainWindowFocusParts parts = focusParts(window);
    test.check(parts.leftView != nullptr,
               "the settings focus regression needs the left panel");
    if (!parts.leftView)
        return;

    const QPointF localPosition(4.0, 4.0);
    const QPointF globalPosition =
        parts.leftView->viewport()->mapToGlobal(localPosition.toPoint());
    QMouseEvent panelPress(QEvent::MouseButtonPress, localPosition,
                           globalPosition, Qt::LeftButton, Qt::LeftButton,
                           Qt::NoModifier);
    QApplication::sendEvent(parts.leftView->viewport(), &panelPress);
    parts.leftView->setFocus(Qt::MouseFocusReason);

    bool foundSettingsDialog = false;
    QTimer::singleShot(0, &window, [&foundSettingsDialog] {
        auto *dialog =
            qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog)
            return;
        auto *closeButton = dialog->findChild<QPushButton *>(
            QStringLiteral("settingsCloseButton"));
        if (!closeButton)
            return;
        foundSettingsDialog = true;
        const QPointF buttonPosition = closeButton->rect().center();
        const QPointF buttonGlobalPosition =
            closeButton->mapToGlobal(buttonPosition.toPoint());
        QMouseEvent closePress(QEvent::MouseButtonPress, buttonPosition,
                               buttonGlobalPosition, Qt::LeftButton,
                               Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(closeButton, &closePress);
        closeButton->click();
    });

    const bool invoked = QMetaObject::invokeMethod(
        &window, "showSettingsDialog", Qt::DirectConnection);
    flushUiEvents();

    test.check(invoked && foundSettingsDialog,
               "the production Settings dialog should open and close");
    test.check(window.focusWidget() == parts.leftView,
               "closing Settings should restore the previous focus target");
    test.check(!parts.leftView->property("keyboardFocusVisible").toBool(),
               "closing Settings with the pointer should not add a keyboard "
               "focus outline");
}

OPENSCP_TEST(testTransferQueueUsesSupportedModelessWindowLifecycle, test) {
    configureMainWindowSettings(settingsRootPath);
    MainWindow window;
    window.resize(900, 560);
    window.show();
    flushUiEvents();

    QAction *transfersAction = nullptr;
    for (QAction *action : window.findChildren<QAction *>()) {
        if (action->shortcut() == QKeySequence(Qt::Key_F12)) {
            transfersAction = action;
            break;
        }
    }
    test.check(transfersAction != nullptr,
               "the transfer queue action should be available");
    if (!transfersAction)
        return;

    transfersAction->trigger();
    flushUiEvents();
    auto *dialog = window.findChild<TransferQueueDialog *>();
    test.check(dialog && dialog->isVisible() && !dialog->isModal(),
               "the transfer queue should open as a modeless window");
    if (!dialog)
        return;

    const auto hasRunningTransition = [dialog] {
        for (QAbstractAnimation *animation :
             dialog->findChildren<QAbstractAnimation *>()) {
            if (animation->state() == QAbstractAnimation::Running)
                return true;
        }
        return false;
    };
    const QString platformName = QGuiApplication::platformName();
    const bool transitionsSupported =
        platformName != QStringLiteral("offscreen") &&
        platformName != QStringLiteral("minimal");
    test.check(hasRunningTransition() == transitionsSupported,
               "the transfer queue should animate only on supported window "
               "systems");
    const bool openingFinished = openscp::testsupport::waitUntil(
        [dialog, &hasRunningTransition, transitionsSupported] {
            return dialog->isVisible() &&
                   (!transitionsSupported ||
                    qFuzzyCompare(dialog->windowOpacity(), 1.0)) &&
                   !hasRunningTransition();
        },
        std::chrono::milliseconds(1000));
    test.check(openingFinished,
               "the transfer queue opening should reach its resting state");
    const QRect restingGeometry = dialog->geometry();

    dialog->reject();
    // Sampled before pumping events, for the reason spelled out at the close
    // control below: the transition starts synchronously, and once the loop
    // runs a loaded machine can finish it before the check reads the state.
    const bool rejectStartedTransition =
        dialog->isVisible() && hasRunningTransition();
    const bool rejectClosedOutright =
        !dialog->isVisible() && !hasRunningTransition();
    flushUiEvents();
    test.check(transitionsSupported ? rejectStartedTransition
                                    : rejectClosedOutright,
               "rejecting the transfer queue should use the supported close "
               "path");
    const bool closingFinished = openscp::testsupport::waitUntil(
        [dialog] { return !dialog->isVisible(); },
        std::chrono::milliseconds(1000));
    test.check(closingFinished,
               "closing should eventually hide the transfer queue");
    test.check(dialog->geometry() == restingGeometry,
               "closing should restore the transfer queue geometry");
    test.check(!transitionsSupported ||
                   qFuzzyCompare(dialog->windowOpacity(), 1.0),
               "closing should restore the transfer queue opacity");

    transfersAction->trigger();
    const bool reopened = openscp::testsupport::waitUntil(
        [dialog, transitionsSupported] {
            return dialog->isVisible() &&
                   (!transitionsSupported ||
                    qFuzzyCompare(dialog->windowOpacity(), 1.0));
        },
        std::chrono::milliseconds(1000));
    test.check(window.findChild<TransferQueueDialog *>() == dialog && reopened,
               "reopening should reuse the animated queue");

    dialog->close();
    // The transition starts synchronously inside close(), so sample it before
    // pumping events: once the loop runs, a loaded machine can finish the
    // animation before the check and the evidence is gone.
    const bool closeWasAnimated = dialog->isVisible() && hasRunningTransition();
    flushUiEvents();
    if (transitionsSupported) {
        test.check(closeWasAnimated,
                   "the window close control should use the closing animation");
        transfersAction->trigger();
        const bool closeWasReversed = openscp::testsupport::waitUntil(
            [dialog, &hasRunningTransition] {
                return dialog->isVisible() &&
                       qFuzzyCompare(dialog->windowOpacity(), 1.0) &&
                       !hasRunningTransition();
            },
            std::chrono::milliseconds(1000));
        test.check(
            closeWasReversed,
            "reopening during close should safely reverse the transition");
    } else {
        test.check(!dialog->isVisible() && !hasRunningTransition(),
                   "unsupported window systems should close immediately");
        transfersAction->trigger();
        flushUiEvents();
        test.check(dialog->isVisible() && !hasRunningTransition(),
                   "unsupported window systems should reopen immediately");
    }

    dialog->close();
    const bool nativeCloseFinished = openscp::testsupport::waitUntil(
        [dialog] { return !dialog->isVisible(); },
        std::chrono::milliseconds(1000));
    test.check(nativeCloseFinished,
               "the window close control should eventually hide the queue");
}

} // namespace

int main(int argc, char **argv) {
    QApplication application(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("OpenSCP-tests"));
    QApplication::setApplicationName(
        QStringLiteral("main-window-navigation-tests"));
    QStandardPaths::setTestModeEnabled(true);

    openscp::testsupport::IsolatedSettings isolatedSettings;
    if (!isolatedSettings.isValid()) {
        std::cerr << "[FAIL] could not create isolated settings directory\n";
        return 1;
    }
    settingsRootPath = isolatedSettings.path();
    openscp::test::TestHarness harness("MainWindow navigation");
    return harness.run();
}
