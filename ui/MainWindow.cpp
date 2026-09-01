// MainWindow orchestrator: builds UI shell, wires actions, and applies
// preferences. Detailed local/remote/transfer logic lives in dedicated units.
#include "MainWindow.hpp"

#include "AboutDialog.hpp"
#include "AppSettings.hpp"
#include "ConnectionDialog.hpp"
#include "DragAwareTreeView.hpp"
#include "MainWindowSharedUtils.hpp"
#include "NavigationScope.hpp"
#include "OpenPathDialog.hpp"
#include "PaneController.hpp"
#include "PathNavigationBar.hpp"
#include "PermissionsDialog.hpp"
#include "RemoteActionController.hpp"
#include "RemoteModel.hpp"
#include "RemoteOperationController.hpp"
#include "SecretStore.hpp"
#include "SessionController.hpp"
#include "SettingsDialog.hpp"
#include "SiteManagerDialog.hpp"
#include "TransferManager.hpp"
#include "TransferQueueDialog.hpp"
#include "UiAlerts.hpp"

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEasingCurve>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGuiApplication>
#include <QHeaderView>
#include <QIcon>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QLabel>
#include <QListView>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QParallelAnimationGroup>
#include <QProcess>
#include <QProgressDialog>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QScrollBar>
#include <QSet>
#include <QShortcut>
#include <QShowEvent>
#include <QSize>
#include <QSizePolicy>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QTabWidget>
#include <QTemporaryFile>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <thread>

namespace {

QIcon mainWindowActionIcon(const char *name) {
    return QIcon(QStringLiteral(":/assets/icons/") + QLatin1String(name));
}

void setActionIconAndTooltip(QAction *action, const QIcon &icon) {
    if (!action)
        return;
    action->setIcon(icon);
    action->setToolTip(action->text());
}

void bindActionToPanelShortcut(QAction *action, QWidget *panel,
                               const QKeySequence &shortcut) {
    if (!action)
        return;
    action->setShortcut(shortcut);
    action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    if (panel)
        panel->addAction(action);
}

QListWidget *createNavigationTabList(QTabWidget *tabs, const QString &title) {
    if (!tabs)
        return nullptr;
    auto *list = new QListWidget(tabs);
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    list->setAlternatingRowColors(true);
    list->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    tabs->addTab(list, title);
    return list;
}

void addDisabledListPlaceholder(QListWidget *list, const QString &text) {
    if (!list)
        return;
    auto *item = new QListWidgetItem(text, list);
    item->setFlags(Qt::NoItemFlags);
}

bool listHasUserData(QListWidget *list) {
    if (!list)
        return false;
    for (int row = 0; row < list->count(); ++row) {
        if (!list->item(row)->data(Qt::UserRole).toString().isEmpty())
            return true;
    }
    return false;
}

void configurePanelTreeView(QTreeView *view, QAbstractItemModel *model,
                            const QModelIndex &rootIndex) {
    if (!view || !model)
        return;
    view->setModel(model);
    view->setExpandsOnDoubleClick(false);
    view->setRootIndex(rootIndex);
    view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    view->setSelectionBehavior(QAbstractItemView::SelectRows);
    view->setSortingEnabled(true);
    view->sortByColumn(0, Qt::AscendingOrder);
    view->header()->setStretchLastSection(true);
    view->setColumnWidth(0, 280);
    view->setDragEnabled(true);
}

void configurePanelDropTarget(QTreeView *view, QObject *eventFilterOwner) {
    if (!view)
        return;
    view->setAcceptDrops(true);
    view->setDragDropMode(QAbstractItemView::DragDrop);
    view->viewport()->setAcceptDrops(true);
    view->setDefaultDropAction(Qt::CopyAction);
    if (eventFilterOwner)
        view->viewport()->installEventFilter(eventFilterOwner);
}

QToolBar *createPaneIconToolbar(const QString &title, QWidget *parent) {
    auto *bar = new QToolBar(title, parent);
    bar->setIconSize(QSize(18, 18));
    bar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    return bar;
}

bool focusWithinWidget(QWidget *focus, QWidget *root) {
    if (!focus || !root)
        return false;
    for (QWidget *cur = focus; cur != nullptr; cur = cur->parentWidget()) {
        if (cur == root)
            return true;
    }
    return false;
}

QString formatConnectionElapsed(qint64 totalSeconds) {
    if (totalSeconds < 0)
        totalSeconds = 0;
    const qint64 hours = totalSeconds / 3600;
    const qint64 minutes = (totalSeconds % 3600) / 60;
    const qint64 seconds = totalSeconds % 60;
    return QStringLiteral("%1:%2:%3")
        .arg(hours, 2, 10, QLatin1Char('0'))
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'));
}

QString trimNavigationLabel(const QString &raw, int maxLen = 96) {
    QString out = raw.simplified();
    if (out.size() <= maxLen)
        return out;
    if (maxLen <= 3)
        return out.left(maxLen);
    return out.left(maxLen - 3) + QStringLiteral("...");
}

} // namespace

MainWindow::~MainWindow() {
    hostKeyPromptCoordinator_.cancel();
    sessionHealthMonitor_.stop();
}

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    QPointer<MainWindow> self(this);
    hostKeyPromptCoordinator_.setPresentPrompt(
        [self](const openscpui::HostKeyPromptCoordinator::Prompt &prompt) {
            if (!self)
                return;
            QMetaObject::invokeMethod(
                self,
                [self, prompt] {
                    if (!self)
                        return;
                    const auto pending =
                        self->hostKeyPromptCoordinator_.pendingPrompt();
                    if (pending && pending->requestId == prompt.requestId)
                        self->showHostKeyPrompt(prompt);
                },
                Qt::QueuedConnection);
        });
    sessionController_ = new openscpui::SessionController(this);
    paneController_ = new openscpui::PaneController(this);
    const QString home = preferredLocalHomePath();
    initializePanels(home);
    initializeMainToolbar();
    initializeMenuBarActions();
    initializePanelInteractions();
    initializeRuntimeState();
}

void MainWindow::initializePanels(const QString &home) {
    // Models
    leftModel_ = new QFileSystemModel(this);
    rightLocalModel_ = new QFileSystemModel(this);

    leftModel_->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot |
                          QDir::AllDirs);
    rightLocalModel_->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot |
                                QDir::AllDirs);

    // Initial paths: local home (fallback to root if HOME is unavailable)
    leftModel_->setRootPath(home);
    rightLocalModel_->setRootPath(home);

    // Views
    leftView_ = new DragAwareTreeView(this);
    rightView_ = new DragAwareTreeView(this);

    configurePanelTreeView(leftView_, leftModel_, leftModel_->index(home));
    configurePanelTreeView(rightView_, rightLocalModel_,
                           rightLocalModel_->index(home));
    configurePanelDropTarget(rightView_, this);
    configurePanelDropTarget(leftView_, this);

    // Central splitter with two panes
    mainSplitter_ = new QSplitter(this);
    auto *leftPane = new QWidget(this);
    auto *rightPane = new QWidget(this);

    // Keep the path visually flat while allowing direct navigation through
    // each parent segment. Manual entry lives in a dedicated dialog.
    leftPath_ = new openscpui::PathNavigationBar(openscpui::PathFlavor::Local,
                                                 home, leftPane);
    rightPath_ = new openscpui::PathNavigationBar(openscpui::PathFlavor::Local,
                                                  home, rightPane);
    connect(leftPath_, &openscpui::PathNavigationBar::pathRequested, this,
            [this](const QString &path) { setLeftRoot(path); });
    connect(rightPath_, &openscpui::PathNavigationBar::pathRequested, this,
            [this](const QString &path) {
                if (rightIsRemote_)
                    setRightRemoteRoot(path);
                else
                    setRightRoot(path);
            });
    connect(leftPath_, &openscpui::PathNavigationBar::openDialogRequested, this,
            [this] { showOpenPathDialog(false); });
    connect(rightPath_, &openscpui::PathNavigationBar::openDialogRequested,
            this, [this] { showOpenPathDialog(true); });

    auto *leftLayout = new QVBoxLayout(leftPane);
    auto *rightLayout = new QVBoxLayout(rightPane);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setContentsMargins(0, 0, 0, 0);

    // Right content stack:
    // - index 0: standard file tree view (local/SFTP)
    // - index 1: SCP transfer-only panel (no remote listing)
    rightContentStack_ = new QStackedWidget(rightPane);
    rightContentStack_->addWidget(rightView_);
    scpTransferPanel_ = new QWidget(rightPane);
    auto *scpLay = new QVBoxLayout(scpTransferPanel_);
    scpLay->setContentsMargins(12, 12, 12, 12);
    scpLay->setSpacing(8);
    scpModeHintLabel_ = new QLabel(
        tr("This protocol works in transfer-only mode.\nUse the remote path "
           "above as the target folder for uploads.\nFor downloads, choose a "
           "remote file path explicitly."),
        scpTransferPanel_);
    scpModeHintLabel_->setWordWrap(true);
    scpQuickUploadBtn_ =
        new QPushButton(tr("Upload local files…"), scpTransferPanel_);
    scpQuickDownloadBtn_ =
        new QPushButton(tr("Download remote file…"), scpTransferPanel_);
    scpLay->addWidget(scpModeHintLabel_);
    scpLay->addWidget(scpQuickUploadBtn_);
    scpLay->addWidget(scpQuickDownloadBtn_);
    scpLay->addStretch(1);
    connect(scpQuickUploadBtn_, &QPushButton::clicked, this,
            &MainWindow::uploadViaDialog);
    connect(scpQuickDownloadBtn_, &QPushButton::clicked, this,
            &MainWindow::downloadRightToLeft);
    rightContentStack_->addWidget(scpTransferPanel_);
    rightContentStack_->setCurrentWidget(rightView_);

    // Left pane sub‑toolbar
    leftPaneBar_ = createPaneIconToolbar(QStringLiteral("LeftBar"), leftPane);
    // Helper for icons from local resources
    auto resIcon = [](const char *fname) -> QIcon {
        return QIcon(QStringLiteral(":/assets/icons/") + QLatin1String(fname));
    };
    auto leftSearchLabel = [this]() {
        return rightIsRemote_ ? tr("Local panel") : tr("Local panel - left");
    };
    auto rightSearchLabel = [this]() {
        return rightIsRemote_ ? tr("Remote panel") : tr("Local panel - right");
    };
    // Left sub-toolbar: navigation/favorites, then file operations.
    actUpLeft_ = leftPaneBar_->addAction(tr("Up"), this, &MainWindow::goUpLeft);
    setActionIconAndTooltip(actUpLeft_, resIcon("action-go-up.svg"));
    // Button "Open left folder" next to Up
    actChooseLeft_ = leftPaneBar_->addAction(tr("Open left folder"), this,
                                             &MainWindow::chooseLeftDir);
    setActionIconAndTooltip(actChooseLeft_, resIcon("action-open-folder.svg"));
    actHomeLeft_ =
        leftPaneBar_->addAction(tr("Home"), this, &MainWindow::goHomeLeft);
    setActionIconAndTooltip(actHomeLeft_, resIcon("action-go-home.svg"));
    actFavoriteToggleLeft_ =
        leftPaneBar_->addAction(tr("Add current path to favorites"), this,
                                [this] { toggleCurrentFavorite(false); });
    actFavoriteToggleLeft_->setObjectName(
        QStringLiteral("leftFavoriteToggleAction"));
    actFavoriteToggleLeft_->setCheckable(true);
    setActionIconAndTooltip(actFavoriteToggleLeft_,
                            resIcon("action-favorite-inactive.svg"));
    actSearchLeft_ = leftPaneBar_->addAction(
        tr("Search items"), this, [this, leftSearchLabel] {
            searchItemsInCurrentFolder(leftView_, leftSearchLabel());
        });
    setActionIconAndTooltip(actSearchLeft_, resIcon("action-search-item.svg"));
    leftPaneBar_->addSeparator();
    actCopyF5_ =
        leftPaneBar_->addAction(tr("Copy"), this, &MainWindow::copyLeftToRight);
    setActionIconAndTooltip(actCopyF5_, resIcon("action-copy.svg"));
    // Shortcut F5 on left panel (scope: left view and its children)
    bindActionToPanelShortcut(actCopyF5_, leftView_, QKeySequence(Qt::Key_F5));
    actMoveF6_ =
        leftPaneBar_->addAction(tr("Move"), this, &MainWindow::moveLeftToRight);
    setActionIconAndTooltip(actMoveF6_, resIcon("action-move-to-right.svg"));
    // Shortcut F6 on left panel
    bindActionToPanelShortcut(actMoveF6_, leftView_, QKeySequence(Qt::Key_F6));
    actDelete_ = leftPaneBar_->addAction(tr("Delete"), this,
                                         &MainWindow::deleteFromLeft);
    setActionIconAndTooltip(actDelete_, resIcon("action-delete.svg"));
    bindActionToPanelShortcut(actDelete_, leftView_,
                              QKeySequence(Qt::Key_Delete));
    // Action: copy from right panel to left (remote/local -> left)
    actCopyRight_ = new QAction(tr("Copy to left panel"), this);
    connect(actCopyRight_, &QAction::triggered, this,
            &MainWindow::copyRightToLeft);
    setActionIconAndTooltip(
        actCopyRight_, QIcon(QLatin1String(":/assets/icons/action-copy.svg")));
    // Action: move from right panel to left
    actMoveRight_ = new QAction(tr("Move to left panel"), this);
    connect(actMoveRight_, &QAction::triggered, this,
            &MainWindow::moveRightToLeft);
    setActionIconAndTooltip(
        actMoveRight_,
        QIcon(QLatin1String(":/assets/icons/action-move-to-left.svg")));
    // Additional local actions (also in toolbar)
    actNewDirLeft_ = new QAction(tr("New folder"), this);
    connect(actNewDirLeft_, &QAction::triggered, this, &MainWindow::newDirLeft);
    actRenameLeft_ = new QAction(tr("Rename"), this);
    connect(actRenameLeft_, &QAction::triggered, this,
            &MainWindow::renameLeftSelected);
    actNewFileLeft_ = new QAction(tr("New file"), this);
    connect(actNewFileLeft_, &QAction::triggered, this,
            &MainWindow::newFileLeft);
    setActionIconAndTooltip(actRenameLeft_, resIcon("action-rename.svg"));
    setActionIconAndTooltip(actNewDirLeft_, resIcon("action-new-folder.svg"));
    setActionIconAndTooltip(actNewFileLeft_, resIcon("action-new-file.svg"));
    // Shortcuts (left panel scope)
    bindActionToPanelShortcut(actRenameLeft_, leftView_,
                              QKeySequence(Qt::Key_F2));
    bindActionToPanelShortcut(actNewDirLeft_, leftView_,
                              QKeySequence(Qt::Key_F9));
    bindActionToPanelShortcut(actNewFileLeft_, leftView_,
                              QKeySequence(Qt::Key_F10));
    leftPaneBar_->addAction(actRenameLeft_);
    leftPaneBar_->addAction(actNewFileLeft_);
    leftPaneBar_->addAction(actNewDirLeft_);
    leftLayout->addWidget(leftPaneBar_);

    // Left panel: toolbar -> flat path bar -> view
    leftLayout->addWidget(leftPath_);
    leftLayout->addWidget(leftView_);

    // Right pane sub‑toolbar
    rightPaneBar_ =
        createPaneIconToolbar(QStringLiteral("RightBar"), rightPane);
    actUpRight_ =
        rightPaneBar_->addAction(tr("Up"), this, &MainWindow::goUpRight);
    setActionIconAndTooltip(actUpRight_, resIcon("action-go-up.svg"));
    // Button "Open right folder" next to Up
    actChooseRight_ = rightPaneBar_->addAction(tr("Open right folder"), this,
                                               &MainWindow::chooseRightDir);
    setActionIconAndTooltip(actChooseRight_, resIcon("action-open-folder.svg"));
    actHomeRight_ =
        rightPaneBar_->addAction(tr("Home"), this, &MainWindow::goHomeRight);
    setActionIconAndTooltip(actHomeRight_, resIcon("action-go-home.svg"));
    actFavoriteToggleRight_ =
        rightPaneBar_->addAction(tr("Add current path to favorites"), this,
                                 [this] { toggleCurrentFavorite(true); });
    actFavoriteToggleRight_->setObjectName(
        QStringLiteral("rightFavoriteToggleAction"));
    actFavoriteToggleRight_->setCheckable(true);
    setActionIconAndTooltip(actFavoriteToggleRight_,
                            resIcon("action-favorite-inactive.svg"));
    actSearchRight_ = rightPaneBar_->addAction(
        tr("Search items"), this, [this, rightSearchLabel] {
            searchItemsInCurrentFolder(rightView_, rightSearchLabel());
        });
    setActionIconAndTooltip(actSearchRight_, resIcon("action-search-item.svg"));

    // Right navigation also exposes global local favorites until a remote
    // session switches this panel to its connection-scoped favorites.
    // Right panel actions (create first, then add in requested order)
    actDownloadF7_ = new QAction(tr("Download"), this);
    connect(actDownloadF7_, &QAction::triggered, this,
            &MainWindow::downloadRightToLeft);
    actDownloadF7_->setEnabled(false); // starts disabled on local
    setActionIconAndTooltip(actDownloadF7_, resIcon("action-download.svg"));

    actUploadRight_ = new QAction(tr("Upload…"), this);
    connect(actUploadRight_, &QAction::triggered, this,
            &MainWindow::uploadViaDialog);
    setActionIconAndTooltip(actUploadRight_, resIcon("action-upload.svg"));
    // Shortcut F8 on right panel to upload via dialog (remote only)
    bindActionToPanelShortcut(actUploadRight_, rightView_,
                              QKeySequence(Qt::Key_F8));

    actRefreshRight_ = new QAction(tr("Refresh"), this);
    connect(actRefreshRight_, &QAction::triggered, this,
            &MainWindow::refreshRightRemotePanel);
    setActionIconAndTooltip(actRefreshRight_, resIcon("action-refresh.svg"));

    actOpenTerminalRight_ = new QAction(tr("Open in terminal"), this);
    connect(actOpenTerminalRight_, &QAction::triggered, this,
            &MainWindow::openRightRemoteTerminal);
    setActionIconAndTooltip(actOpenTerminalRight_,
                            resIcon("action-open-terminal.svg"));

    actNewDirRight_ = new QAction(tr("New folder"), this);
    connect(actNewDirRight_, &QAction::triggered, this,
            &MainWindow::newDirRight);
    actRenameRight_ = new QAction(tr("Rename"), this);
    connect(actRenameRight_, &QAction::triggered, this,
            &MainWindow::renameRightSelected);
    actDeleteRight_ = new QAction(tr("Delete"), this);
    connect(actDeleteRight_, &QAction::triggered, this,
            &MainWindow::deleteRightSelected);
    actNewFileRight_ = new QAction(tr("New file"), this);
    connect(actNewFileRight_, &QAction::triggered, this,
            &MainWindow::newFileRight);
    setActionIconAndTooltip(actNewDirRight_, resIcon("action-new-folder.svg"));
    setActionIconAndTooltip(actRenameRight_, resIcon("action-rename.svg"));
    setActionIconAndTooltip(actDeleteRight_, resIcon("action-delete.svg"));
    setActionIconAndTooltip(actNewFileRight_, resIcon("action-new-file.svg"));
    // Shortcuts (right panel scope)
    bindActionToPanelShortcut(actRenameRight_, rightView_,
                              QKeySequence(Qt::Key_F2));
    bindActionToPanelShortcut(actNewDirRight_, rightView_,
                              QKeySequence(Qt::Key_F9));
    bindActionToPanelShortcut(actNewFileRight_, rightView_,
                              QKeySequence(Qt::Key_F10));

    // Order: Copy, Move, Delete, Rename, New folder, then
    // Download/Upload/Refresh
    rightPaneBar_->addSeparator();
    // Toolbar buttons with generic texts (Copy/Move)
    actCopyRightTb_ = new QAction(tr("Copy"), this);
    connect(actCopyRightTb_, &QAction::triggered, this,
            &MainWindow::copyRightToLeft);
    actMoveRightTb_ = new QAction(tr("Move"), this);
    connect(actMoveRightTb_, &QAction::triggered, this,
            &MainWindow::moveRightToLeft);
    setActionIconAndTooltip(actCopyRightTb_, resIcon("action-copy.svg"));
    setActionIconAndTooltip(actMoveRightTb_,
                            resIcon("action-move-to-left.svg"));
    // Shortcuts F5/F6 on right panel (scope: right view)
    bindActionToPanelShortcut(actCopyRightTb_, rightView_,
                              QKeySequence(Qt::Key_F5));
    bindActionToPanelShortcut(actMoveRightTb_, rightView_,
                              QKeySequence(Qt::Key_F6));
    rightPaneBar_->addAction(actCopyRightTb_);
    rightPaneBar_->addAction(actMoveRightTb_);
    rightPaneBar_->addAction(actDeleteRight_);
    rightPaneBar_->addAction(actRenameRight_);
    rightPaneBar_->addAction(actNewFileRight_);
    rightPaneBar_->addAction(actNewDirRight_);
    rightPaneBar_->addSeparator();
    rightPaneBar_->addAction(actDownloadF7_);
    rightPaneBar_->addAction(actUploadRight_);
    rightPaneBar_->addSeparator();
    rightPaneBar_->addAction(actRefreshRight_);
    rightPaneBar_->addAction(actOpenTerminalRight_);
    // Delete shortcut also on right panel (limited to right panel widget)
    if (actDeleteRight_) {
        bindActionToPanelShortcut(actDeleteRight_, rightView_,
                                  QKeySequence(Qt::Key_Delete));
    }
    // Keyboard shortcut F7 on right panel: only acts when remote and with
    // selection
    if (rightView_) {
        auto *scF7 = new QShortcut(QKeySequence(Qt::Key_F7), rightView_);
        scF7->setContext(Qt::WidgetWithChildrenShortcut);
        connect(scF7, &QShortcut::activated, this, [this] {
            if (!rightIsRemote_)
                return; // only when remote
            auto sel = rightView_->selectionModel();
            if (!sel || sel->selectedRows(kNameColumn).isEmpty()) {
                statusBar()->showMessage(tr("Select items to download"), 2000);
                return;
            }
            downloadRightToLeft();
        });
    }
    // Ctrl+F / Cmd+F: open search dialog for the focused panel.
    auto *scFind = new QShortcut(QKeySequence::Find, this);
    scFind->setContext(Qt::WindowShortcut);
    connect(scFind, &QShortcut::activated, this,
            [this, leftSearchLabel, rightSearchLabel] {
                QWidget *focus = QApplication::focusWidget();
                const bool inRightPanel =
                    focusWithinWidget(focus, rightView_) ||
                    focusWithinWidget(focus, rightPath_) ||
                    focusWithinWidget(focus, rightPaneBar_);
                if (inRightPanel) {
                    searchItemsInCurrentFolder(rightView_, rightSearchLabel());
                    return;
                }

                const bool inLeftPanel = focusWithinWidget(focus, leftView_) ||
                                         focusWithinWidget(focus, leftPath_) ||
                                         focusWithinWidget(focus, leftPaneBar_);
                if (inLeftPanel) {
                    searchItemsInCurrentFolder(leftView_, leftSearchLabel());
                    return;
                }

                if (rightIsRemote_)
                    searchItemsInCurrentFolder(rightView_, rightSearchLabel());
                else
                    searchItemsInCurrentFolder(leftView_, leftSearchLabel());
            });

    const auto openFocusedPath = [this] {
        QWidget *focus = QApplication::focusWidget();
        const bool inRightPanel = focusWithinWidget(focus, rightView_) ||
                                  focusWithinWidget(focus, rightPath_) ||
                                  focusWithinWidget(focus, rightPaneBar_);
        if (inRightPanel) {
            rightPath_->requestOpenDialog();
            return;
        }
        leftPath_->requestOpenDialog();
    };
    auto *scEditPath =
        new QShortcut(QKeySequence(QStringLiteral("Ctrl+L")), this);
    scEditPath->setContext(Qt::WindowShortcut);
    connect(scEditPath, &QShortcut::activated, this, openFocusedPath);
#ifdef Q_OS_MACOS
    auto *scEditPathMac =
        new QShortcut(QKeySequence(QStringLiteral("Meta+L")), this);
    scEditPathMac->setContext(Qt::WindowShortcut);
    connect(scEditPathMac, &QShortcut::activated, this, openFocusedPath);
#endif
    auto *scOpenPath = new QShortcut(QKeySequence(QKeySequence::Open), this);
    scOpenPath->setContext(Qt::WindowShortcut);
    connect(scOpenPath, &QShortcut::activated, this, openFocusedPath);
    // Disable strictly-remote actions at startup
    if (actDownloadF7_)
        actDownloadF7_->setEnabled(false);
    actUploadRight_->setEnabled(false);
    if (actRefreshRight_)
        actRefreshRight_->setEnabled(false);
    if (actOpenTerminalRight_)
        actOpenTerminalRight_->setEnabled(false);
    if (actNewFileRight_)
        actNewFileRight_->setEnabled(false);

    // Right panel: toolbar -> flat path bar -> view
    rightLayout->addWidget(rightPaneBar_);
    rightLayout->addWidget(rightPath_);
    rightLayout->addWidget(rightContentStack_);

    // Mount panes into the splitter
    mainSplitter_->addWidget(leftPane);
    mainSplitter_->addWidget(rightPane);
    setCentralWidget(mainSplitter_);
}

void MainWindow::initializeMainToolbar() {
    auto *mainToolbar = addToolBar("Main");
    mainToolbar->setObjectName("mainToolbar");
    mainToolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    mainToolbar->setMovable(false);
    // Keep the system default size for the main toolbar and make sub‑toolbars
    // slightly smaller
    const int mainIconPx = mainToolbar->style()->pixelMetric(
        QStyle::PM_ToolBarIconSize, nullptr, mainToolbar);
    const int subIconPx =
        qMax(16, mainIconPx - 4); // sub‑toolbars slightly smaller
    leftPaneBar_->setIconSize(QSize(subIconPx, subIconPx));
    rightPaneBar_->setIconSize(QSize(subIconPx, subIconPx));
    // Copy/move/delete actions now live in the left sub‑toolbar
    actConnect_ =
        mainToolbar->addAction(tr("Connect"), this, &MainWindow::connectRemote);
    actConnect_->setIcon(mainWindowActionIcon("action-connect.svg"));
    actConnect_->setToolTip(actConnect_->text());
    mainToolbar->addSeparator();
    actDisconnect_ = mainToolbar->addAction(tr("Disconnect"), this,
                                            &MainWindow::disconnectRemote);
    actDisconnect_->setIcon(mainWindowActionIcon("action-disconnect.svg"));
    actDisconnect_->setToolTip(actDisconnect_->text());
    actDisconnect_->setEnabled(false);

    auto setTextBesideIcon = [mainToolbar](QAction *action,
                                           const QString &text) {
        if (!mainToolbar || !action)
            return;
        if (QWidget *widget = mainToolbar->widgetForAction(action)) {
            if (auto *button = qobject_cast<QToolButton *>(widget)) {
                button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
                button->setText(text);
            }
        }
    };
    // Show text to the LEFT of the icon for Connect/Disconnect buttons only
    setTextBesideIcon(actConnect_, tr("Connect"));
    setTextBesideIcon(actDisconnect_, tr("Disconnect"));
    mainToolbar->addSeparator();
    actSites_ = mainToolbar->addAction(tr("Saved sites"), [this] {
        SiteManagerDialog dlg(this);
        if (dlg.exec() == QDialog::Accepted) {
            SiteEntry site;
            if (dlg.selectedSite(site))
                startSavedSiteConnect(site);
        }
    });
    actSites_->setIcon(mainWindowActionIcon("action-open-saved-sites.svg"));
    actSites_->setToolTip(actSites_->text());
    mainToolbar->addSeparator();
    actShowQueue_ = mainToolbar->addAction(tr("Transfers"),
                                           [this] { showTransferQueue(); });
    actShowQueue_->setIcon(
        mainWindowActionIcon("action-open-transfer-queue.svg"));
    actShowQueue_->setToolTip(actShowQueue_->text());
    mainToolbar->addSeparator();
    actSync_ =
        mainToolbar->addAction(tr("Sync"), this, &MainWindow::showSyncDialog);
    actSync_->setIcon(mainWindowActionIcon("action-refresh.svg"));
    actSync_->setToolTip(tr("Compare and synchronize the current folders"));
    actSync_->setEnabled(false);
    mainToolbar->addSeparator();
    actShowHistory_ = mainToolbar->addAction(tr("History"), this,
                                             &MainWindow::showHistoryMenu);
    actShowHistory_->setIcon(mainWindowActionIcon("action-open-history.svg"));
    actShowHistory_->setToolTip(actShowHistory_->text());
    mainToolbar->addSeparator();
    actShowFavorites_ = mainToolbar->addAction(
        tr("Favorites"), this, &MainWindow::showFavoritesDialog);
    actShowFavorites_->setObjectName(QStringLiteral("showFavoritesAction"));
    actShowFavorites_->setIcon(mainWindowActionIcon("bookmark.svg"));
    actShowFavorites_->setToolTip(actShowFavorites_->text());

    // Show text beside icon for Sites and Queue too
    setTextBesideIcon(actSites_, tr("Saved sites"));
    setTextBesideIcon(actShowQueue_, tr("Transfers"));
    setTextBesideIcon(actSync_, tr("Sync"));
    setTextBesideIcon(actShowHistory_, tr("History"));
    setTextBesideIcon(actShowFavorites_, tr("Favorites"));

    // Global shortcut to open the transfer queue
    actShowQueue_->setShortcut(QKeySequence(Qt::Key_F12));
    actShowQueue_->setShortcutContext(Qt::ApplicationShortcut);
    this->addAction(actShowQueue_);
    // Global shortcut to open recent history
    actShowHistory_->setShortcut(QKeySequence::fromString(
        QStringLiteral("Ctrl+Shift+H"), QKeySequence::PortableText));
    actShowHistory_->setShortcutContext(Qt::ApplicationShortcut);
    this->addAction(actShowHistory_);

    // Global fullscreen toggle (standard platform shortcut)
    // macOS: Ctrl+Cmd+F, Linux: F11
    {
        QAction *actToggleFs = new QAction(tr("Full screen"), this);
        actToggleFs->setShortcut(QKeySequence::FullScreen);
        actToggleFs->setShortcutContext(Qt::ApplicationShortcut);
        connect(actToggleFs, &QAction::triggered, this, [this] {
            const bool isFullScreen = (windowState() & Qt::WindowFullScreen);
            if (isFullScreen)
                setWindowState(windowState() & ~Qt::WindowFullScreen);
            else
                setWindowState(windowState() | Qt::WindowFullScreen);
        });
        this->addAction(actToggleFs);
    }

    // Separator to the right of the history/favorites navigation group
    mainToolbar->addSeparator();

    // Spacer to push next action to the far right
    {
        QWidget *spacer = new QWidget(this);
        spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        mainToolbar->addWidget(spacer);
    }

    // Visual separator before the right-side buttons
    mainToolbar->addSeparator();
    // About button (to the left of Settings)
    actAboutToolbar_ = mainToolbar->addAction(
        mainWindowActionIcon("action-open-about-us.svg"), tr("About OpenSCP"),
        this, &MainWindow::showAboutDialog);
    if (actAboutToolbar_)
        actAboutToolbar_->setToolTip(actAboutToolbar_->text());
    // Settings button (far right)
    actPrefsToolbar_ = mainToolbar->addAction(
        mainWindowActionIcon("action-open-settings.svg"), tr("Settings"), this,
        &MainWindow::showSettingsDialog);
    actPrefsToolbar_->setToolTip(actPrefsToolbar_->text());
}

void MainWindow::initializeMenuBarActions() {
    // Menu bar (native on macOS)
    // Duplicate actions so users who prefer the classic menu can use it.
    appMenu_ = menuBar()->addMenu(tr("OpenSCP"));
    actAbout_ = appMenu_->addAction(tr("About OpenSCP"), this,
                                    &MainWindow::showAboutDialog);
    actAbout_->setMenuRole(QAction::AboutRole);
    actPrefs_ = appMenu_->addAction(tr("Settings…"), this,
                                    &MainWindow::showSettingsDialog);
    actPrefs_->setMenuRole(QAction::PreferencesRole);
    // Standard cross‑platform shortcut (Cmd+, on macOS; Ctrl+, on
    // Linux/Windows)
    actPrefs_->setShortcut(QKeySequence::Preferences);
    appMenu_->addSeparator();
    actQuit_ = appMenu_->addAction(tr("Quit"), qApp, &QApplication::quit);
    actQuit_->setMenuRole(QAction::QuitRole);
    // Standard quit shortcut (Cmd+Q / Ctrl+Q)
    actQuit_->setShortcut(QKeySequence::Quit);

    fileMenu_ = menuBar()->addMenu(tr("File"));
    fileMenu_->addAction(actChooseLeft_);
    fileMenu_->addAction(actChooseRight_);
    fileMenu_->addSeparator();
    fileMenu_->addAction(actConnect_);
    fileMenu_->addAction(actDisconnect_);
    fileMenu_->addAction(actSites_);
    fileMenu_->addAction(actShowQueue_);
    fileMenu_->addAction(actSync_);
    fileMenu_->addAction(actShowHistory_);
    fileMenu_->addAction(actShowFavorites_);
    // On non‑macOS platforms, also show Preferences and Quit under "File"
    // to provide a familiar UX on Linux/Windows while keeping the "OpenSCP" app
    // menu.
#ifndef Q_OS_MAC
    fileMenu_->addSeparator();
    fileMenu_->addAction(actPrefs_);
    fileMenu_->addAction(actQuit_);
#endif

    // Help (avoid native help menu to skip the search box)
    auto *helpMenu = menuBar()->addMenu(tr("Help"));
    // On macOS, a menu titled exactly "Help" triggers the native search bar.
    // Keep visible label "Help" but avoid detection by inserting a zero‑width
    // space.
#ifdef Q_OS_MAC
    {
        const QString helpMenuTitle = helpMenu->title();
        if (helpMenuTitle.compare(QStringLiteral("Help"),
                                  Qt::CaseInsensitive) == 0) {
            helpMenu->setTitle(QStringLiteral("Hel") + QChar(0x200B) +
                               QStringLiteral("p"));
        }
    }
#endif
    helpMenu->menuAction()->setMenuRole(QAction::NoRole);
    // Prevent macOS from moving actions to the app menu: force NoRole
    {
        QAction *helpAboutAct = new QAction(tr("About OpenSCP"), this);
        helpAboutAct->setMenuRole(QAction::NoRole);
        connect(helpAboutAct, &QAction::triggered, this,
                &MainWindow::showAboutDialog);
        helpMenu->addAction(helpAboutAct);
    }
    {
        QAction *reportAct = new QAction(tr("Report a bug"), this);
        reportAct->setMenuRole(QAction::NoRole);
        connect(reportAct, &QAction::triggered, this, [] {
            QDesktopServices::openUrl(
                QUrl("https://github.com/luiscuellar31/openscp/issues"));
        });
        helpMenu->addAction(reportAct);
    }
}

void MainWindow::initializePanelInteractions() {
    // Double click/Enter navigation on both panes
    connect(rightView_, &QTreeView::activated, this,
            &MainWindow::rightItemActivated);
    connect(leftView_, &QTreeView::activated, this,
            &MainWindow::leftItemActivated);

    // Context menu on right pane
    rightView_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(rightView_, &QWidget::customContextMenuRequested, this,
            &MainWindow::showRightContextMenu);
    if (rightView_->selectionModel()) {
        connect(rightView_->selectionModel(),
                &QItemSelectionModel::selectionChanged, this,
                [this] { updateDeleteShortcutEnables(); });
    }

    // Context menu on left pane (local)
    leftView_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(leftView_, &QWidget::customContextMenuRequested, this,
            &MainWindow::showLeftContextMenu);

    // Enable delete shortcut only when there is a selection on the left pane
    if (leftView_->selectionModel()) {
        connect(leftView_->selectionModel(),
                &QItemSelectionModel::selectionChanged, this,
                [this] { updateDeleteShortcutEnables(); });
    }
}

void MainWindow::initializeRuntimeState() {
    {
        openscpui::AppSettings settings;
        downloadDir_ = defaultDownloadDirFromSettings(settings);
    }
    QDir().mkpath(downloadDir_);

    initializeConnectionSessionIndicators();
    statusBar()->showMessage(tr("Ready"));
    setWindowTitle(tr("OpenSCP — local/local (click Connect for remote)"));
    resize(1100, 650);
    refreshLeftPathNavigation();
    refreshRightPathNavigation();
    restoreMainWindowUiState();

    remoteOps_ = new RemoteOperationController(this);
    openscpui::SessionHealthMonitor::Callbacks healthCallbacks;
    healthCallbacks.canProbe = [this] {
        return rightIsRemote_ && sessionController_->client() && remoteOps_ &&
               remoteOps_->hasRequestedSession() &&
               sessionController_->options().has_value() &&
               !sessionController_->isDisconnecting() &&
               !sessionController_->isConnecting();
    };
    healthCallbacks.probePath = [this] {
        return (rightRemoteModel_ && !rightRemoteModel_->rootPath().isEmpty())
                   ? rightRemoteModel_->rootPath()
                   : QStringLiteral("/");
    };
    healthCallbacks.submitProbe = [this](const QString &path) {
        if (!remoteOps_)
            return quint64{0};
        RemoteOperationController::HealthCheckRequest request;
        request.path = path;
        return remoteOps_->submit(request);
    };
    healthCallbacks.cancelProbe = [this](quint64 jobId) {
        if (remoteOps_)
            remoteOps_->cancel(jobId);
    };
    healthCallbacks.periodicReason = [] { return tr("periodic"); };
    healthCallbacks.resumeReason = [](qint64 inactiveSeconds) {
        return tr("resume (%1s)").arg(inactiveSeconds);
    };
    healthCallbacks.probeSucceeded =
        [this](const openscpui::SessionHealthMonitor::ProbeContext &context) {
            if (context.forced && !context.reason.isEmpty()) {
                statusBar()->showMessage(
                    tr("Remote session validated (%1)").arg(context.reason),
                    2500);
            }
        };
    healthCallbacks.probeFailed =
        [this](const openscpui::SessionHealthMonitor::ProbeContext &context,
               const QString &error) {
            if (!rightIsRemote_ || !sessionController_->client() ||
                sessionController_->isDisconnecting() ||
                !isLikelyRemoteTransportError(error)) {
                return;
            }
            UiAlerts::warning(
                this, tr("Connection lost"),
                tr("The remote session no longer responds (%1).\n"
                   "OpenSCP will disconnect to avoid inconsistent "
                   "operations.\n%2")
                    .arg(context.reason,
                         shortRemoteError(error, tr("Transport error."))));
            disconnectRemote();
        };
    sessionHealthMonitor_.setCallbacks(std::move(healthCallbacks));

    remoteActionController_ = new openscpui::RemoteActionController(this);
    openscpui::RemoteActionController::Context remoteActionContext;
    remoteActionContext.dialogParent = this;
    remoteActionContext.statusBar = statusBar();
    remoteActionContext.operations = remoteOps_;
    remoteActionContext.isSessionActive = [this] { return rightIsRemote_; };
    remoteActionContext.refreshPath = [this](const QString &path) {
        requestRemoteListing(path, true);
    };
    remoteActionContext.remoteActivitySucceeded = [this] {
        sessionHealthMonitor_.recordActivity();
    };
    remoteActionContext.requestPermissions = [this](std::uint32_t currentMode,
                                                    bool isDirectory)
        -> std::optional<
            openscpui::RemoteActionController::PermissionSelection> {
        PermissionsDialog dialog(this);
        dialog.setMode(currentMode);
        if (dialog.exec() != QDialog::Accepted)
            return std::nullopt;
        openscpui::RemoteActionController::PermissionSelection selection;
        selection.mode = dialog.mode();
        selection.recursive = dialog.recursive() && isDirectory;
        return selection;
    };
    remoteActionController_->setContext(std::move(remoteActionContext));
    connect(
        remoteOps_, &RemoteOperationController::listCompleted, this,
        [this](const RemoteOperationController::ListResult &result) {
            if (result.result.job.id != activeRemoteListJob_ ||
                !rightIsRemote_ || !rightRemoteModel_) {
                return;
            }
            activeRemoteListJob_ = 0;
            if (result.result.outcome !=
                RemoteOperationController::Outcome::Succeeded) {
                if (activeRemoteListIsInitial_ &&
                    !initialRemoteFallbackAttempted_ &&
                    requestedRemotePath_ != QStringLiteral("/")) {
                    initialRemoteFallbackAttempted_ = true;
                    requestRemoteListing(QStringLiteral("/"), false, true);
                    return;
                }
                rightRemoteModel_->clearLoading();
                rightView_->setEnabled(true);
                statusBar()->showMessage(
                    tr("Remote folder unavailable. The session remains "
                       "connected; use Refresh to retry."),
                    7000);
                UiAlerts::warning(this, tr("Remote error"),
                                  tr("Could not open the remote folder.\n%1")
                                      .arg(result.result.error));
                return;
            }

            std::vector<openscp::FileInfo> entries;
            entries.reserve(static_cast<std::size_t>(result.entries.size()));
            for (const auto &entry : result.entries)
                entries.push_back(entry.info);
            rightRemoteModel_->setEntries(result.path, entries);
            rightView_->setEnabled(true);
            sessionHealthMonitor_.recordActivity();

            if (activeRemoteListIsRefresh_ && rightView_->selectionModel()) {
                QItemSelectionModel *selection = rightView_->selectionModel();
                selection->clearSelection();
                QModelIndex first;
                for (int row = 0; row < rightRemoteModel_->rowCount(); ++row) {
                    const QModelIndex index = rightRemoteModel_->index(row, 0);
                    if (!remoteRefreshSelectionNames_.contains(
                            rightRemoteModel_->nameAt(index))) {
                        continue;
                    }
                    selection->select(index, QItemSelectionModel::Select |
                                                 QItemSelectionModel::Rows);
                    if (!first.isValid())
                        first = index;
                }
                if (first.isValid())
                    selection->setCurrentIndex(first,
                                               QItemSelectionModel::NoUpdate);
                if (rightView_->verticalScrollBar()) {
                    rightView_->verticalScrollBar()->setValue(
                        remoteRefreshScrollValue_);
                }
            } else if (rightView_->selectionModel()) {
                rightView_->selectionModel()->clear();
                if (rightView_->verticalScrollBar())
                    rightView_->verticalScrollBar()->setValue(0);
            }
            remoteRefreshSelectionNames_.clear();
            activeRemoteListIsRefresh_ = false;
            activeRemoteListIsInitial_ = false;
        });
    connect(remoteOps_, &RemoteOperationController::healthCheckCompleted, this,
            [this](const RemoteOperationController::HealthResult &result) {
                using HealthOutcome =
                    openscpui::SessionHealthMonitor::ProbeOutcome;
                HealthOutcome outcome = HealthOutcome::Failed;
                switch (result.result.outcome) {
                case RemoteOperationController::Outcome::Succeeded:
                    outcome = HealthOutcome::Succeeded;
                    break;
                case RemoteOperationController::Outcome::Canceled:
                    outcome = HealthOutcome::Canceled;
                    break;
                case RemoteOperationController::Outcome::Superseded:
                    outcome = HealthOutcome::Superseded;
                    break;
                case RemoteOperationController::Outcome::Failed:
                    outcome = HealthOutcome::Failed;
                    break;
                }
                (void)sessionHealthMonitor_.completeProbe(
                    result.result.job.id, outcome, result.result.error);
            });

    // Transfer queue
    transferMgr_ = new TransferManager(this);
    connect(transferMgr_, &TransferManager::persistenceWarning, this,
            [this](const QString &warning) {
                statusBar()->showMessage(warning, 8000);
                UiAlerts::warning(this, tr("Transfer queue"), warning);
            });
    (void)transferMgr_->enablePersistence();
    // A single cold snapshot seeds the delta-based UI observer at startup.
    transferUiController_.initialize(transferMgr_->tasksSnapshot());
    connect(transferMgr_, &TransferManager::tasksAdded, this,
            [this](const QVector<quint64> &ids) {
                handleTransferUiUpdate(ids, {});
            });
    connect(transferMgr_, &TransferManager::tasksUpdated, this,
            [this](const QVector<quint64> &ids) {
                handleTransferUiUpdate(ids, {});
            });
    connect(transferMgr_, &TransferManager::tasksRemoved, this,
            [this](const QVector<quint64> &ids) {
                handleTransferUiUpdate({}, ids);
            });
    // Provide transfer manager to views (for async remote drag-out staging)
    if (auto *leftDragView = qobject_cast<DragAwareTreeView *>(leftView_)) {
        leftDragView->setTransferManager(transferMgr_);
        leftDragView->setRemoteOperationController(remoteOps_);
    }
    if (auto *rightDragView = qobject_cast<DragAwareTreeView *>(rightView_)) {
        rightDragView->setTransferManager(transferMgr_);
        rightDragView->setRemoteOperationController(remoteOps_);
    }
    initializeSyncCoordinator();

    // Startup cleanup (deferred): remove old staging batches if
    // autoCleanStaging is enabled
    QTimer::singleShot(0, this, [] {
        openscpui::AppSettings settings;
        const bool autoClean =
            settings.value(openscpui::settingskeys::kAutoCleanStaging, true)
                .toBool();
        if (!autoClean)
            return;
        QString root =
            settings.value(openscpui::settingskeys::kStagingRoot).toString();
        if (root.isEmpty())
            root = QDir::homePath() + "/Downloads/OpenSCP-Dragged";
        QDir stagingRootDir(root);
        if (!stagingRootDir.exists())
            return;
        const QDateTime now = QDateTime::currentDateTimeUtc();
        const int retentionDays = qBound(
            1,
            settings.value(openscpui::settingskeys::kStagingRetentionDays, 7)
                .toInt(),
            365);
        const qint64 maxAgeMs = qint64(retentionDays) * 24 * 60 * 60 * 1000;
        // Match timestamp batches: yyyyMMdd-HHmmss
        QRegularExpression re("^\\d{8}-\\d{6}$");
        const auto entries = stagingRootDir.entryInfoList(
            QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
        for (const QFileInfo &entryInfo : entries) {
            if (entryInfo.isSymLink())
                continue; // do not follow symlinks
            if (!re.match(entryInfo.fileName()).hasMatch())
                continue; // only batches
            const QDateTime lastModifiedUtc = entryInfo.lastModified().toUTC();
            if (lastModifiedUtc.isValid() &&
                lastModifiedUtc.msecsTo(now) > maxAgeMs) {
                QDir(entryInfo.absoluteFilePath()).removeRecursively();
            }
        }
    });

    // Warn if insecure storage is active (non‑Apple only when explicitly
    // enabled)
    if (SecretStore::insecureFallbackActive()) {
        auto *warn = new QLabel(
            tr("Warning: unencrypted secrets storage active (fallback)"), this);
        warn->setStyleSheet(
            "QLabel{ color:#b00020; font-weight:bold; padding:2px 6px; }");
        warn->setToolTip(
            tr("You are using unencrypted credentials storage enabled via "
               "environment variable. Disable "
               "OPENSCP_ENABLE_INSECURE_FALLBACK to hide this warning."));
        statusBar()->addPermanentWidget(warn, 0);
    }

    // Apply user preferences (hidden files, click mode, etc.)
    applyPreferences();
    updateDeleteShortcutEnables();

    // Startup preferences and migration
    {
        openscpui::AppSettings settings;
        // One-shot migration: if only showConnOnStart exists, copy to
        // openSiteManagerOnDisconnect
        if (!settings.contains(
                openscpui::settingskeys::kUiOpenSiteManagerOnDisconnect) &&
            settings.contains(
                openscpui::settingskeys::kUiShowConnectionOnStart)) {
            const bool showSiteManagerOnStart =
                settings
                    .value(openscpui::settingskeys::kUiShowConnectionOnStart,
                           true)
                    .toBool();
            settings.setValue(
                openscpui::settingskeys::kUiOpenSiteManagerOnDisconnect,
                showSiteManagerOnStart);
            settings.sync();
        }
        openSiteManagerOnStartup_ =
            settings
                .value(openscpui::settingskeys::kUiShowConnectionOnStart, true)
                .toBool();
        openSiteManagerOnDisconnect_ =
            settings
                .value(openscpui::settingskeys::kUiOpenSiteManagerOnDisconnect,
                       true)
                .toBool();
        if (openSiteManagerOnStartup_ && !QCoreApplication::closingDown() &&
            !sessionController_->client()) {
            QTimer::singleShot(0, this, [this] { showSiteManagerNonModal(); });
        }
    }
}

void MainWindow::rebuildContextMenu(QMenu *menu,
                                    const QVector<QAction *> &entries) const {
    if (!menu)
        return;

    menu->clear();
    bool lastWasSeparator = true;
    for (QAction *entry : entries) {
        if (!entry) {
            if (!lastWasSeparator && !menu->actions().isEmpty()) {
                menu->addSeparator();
                lastWasSeparator = true;
            }
            continue;
        }
        menu->addAction(entry);
        lastWasSeparator = false;
    }

    const QList<QAction *> actions = menu->actions();
    if (!actions.isEmpty() && actions.back()->isSeparator())
        menu->removeAction(actions.back());
}

void MainWindow::initializeConnectionSessionIndicators() {
    if (!statusBar())
        return;

    if (!connectionTypeLabel_) {
        connectionTypeLabel_ = new QLabel(this);
        connectionTypeLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        connectionTypeLabel_->setMinimumWidth(
            connectionTypeLabel_->fontMetrics().horizontalAdvance(
                tr("Type: HttpConnect")) +
            12);
        connectionTypeLabel_->setToolTip(
            tr("Active connection method for this session"));
        statusBar()->addPermanentWidget(connectionTypeLabel_);
    }

    if (!connectionElapsedLabel_) {
        connectionElapsedLabel_ = new QLabel(this);
        connectionElapsedLabel_->setAlignment(Qt::AlignRight |
                                              Qt::AlignVCenter);
        connectionElapsedLabel_->setMinimumWidth(
            connectionElapsedLabel_->fontMetrics().horizontalAdvance(
                tr("Session: 000:00:00")) +
            12);
        connectionElapsedLabel_->setToolTip(
            tr("Elapsed time for the current connection session"));
        statusBar()->addPermanentWidget(connectionElapsedLabel_);
    }

    connectionStatusCoordinator_.setRenderCallback(
        [this](
            const openscpui::ConnectionStatusCoordinator::Snapshot &snapshot) {
            if (connectionTypeLabel_) {
                const QString typeLabel =
                    snapshot.connected ? snapshot.connectionType : tr("None");
                const bool insecure =
                    !activeSecurityWarning_.trimmed().isEmpty();
                connectionTypeLabel_->setText(
                    insecure ? tr("Type: %1 • UNSAFE").arg(typeLabel)
                             : tr("Type: %1").arg(typeLabel));
                connectionTypeLabel_->setStyleSheet(
                    insecure ? QStringLiteral(
                                   "QLabel { color: #B00020; font-weight: "
                                   "700; }")
                             : QString());
                connectionTypeLabel_->setToolTip(
                    insecure ? activeSecurityWarning_
                             : tr("Active connection method for this session"));
            }

            if (connectionElapsedLabel_) {
                connectionElapsedLabel_->setText(
                    snapshot.connected ? tr("Session: %1")
                                             .arg(formatConnectionElapsed(
                                                 snapshot.elapsedSeconds))
                                       : tr("Session: --:--:--"));
            }
        });
    resetConnectionSessionIndicators();
}

void MainWindow::startConnectionSessionIndicators(
    const QString &connectionType) {
    if (!connectionTypeLabel_ || !connectionElapsedLabel_)
        initializeConnectionSessionIndicators();

    const QString normalizedType = connectionType.trimmed();
    connectionStatusCoordinator_.start(
        normalizedType.isEmpty() ? tr("Unknown") : normalizedType);
}

void MainWindow::resetConnectionSessionIndicators() {
    activeSecurityWarning_.clear();
    sessionNoHostVerification_ = false;
    connectionStatusCoordinator_.reset();
}

// Show the application About dialog.
void MainWindow::showAboutDialog() {
    AboutDialog dlg(this);
    dlg.exec();
}

// Open the Settings dialog and apply changes when accepted.
void MainWindow::showSettingsDialog() {
    SettingsDialog dlg(this);
    connect(&dlg, &SettingsDialog::settingsApplied, this,
            &MainWindow::applyPreferences);
    dlg.exec();
    // Reflect any applied changes in the running UI
    applyPreferences();
}

void MainWindow::showEvent(QShowEvent *e) {
    QMainWindow::showEvent(e);
    if (firstShow_) {
        firstShow_ = false;
        if (restoredWindowGeometry_)
            return;
        QRect avail;
        if (this->screen())
            avail = this->screen()->availableGeometry();
        else if (auto primaryScreen = QGuiApplication::primaryScreen())
            avail = primaryScreen->availableGeometry();
        if (avail.isValid()) {
            const QSize sz = size();
            const int centeredX = avail.center().x() - sz.width() / 2;
            const int centeredY = avail.center().y() - sz.height() / 2;
            move(centeredX, centeredY);
        }
    }
}

void MainWindow::closeEvent(QCloseEvent *e) {
    if (transferCleanupInProgress_) {
        pendingCloseAfterDisconnect_ = true;
        e->ignore();
        return;
    }
    if (sessionController_->isDisconnecting()) {
        pendingCloseAfterDisconnect_ = true;
        e->ignore();
        return;
    }
    if (rightIsRemote_) {
        pendingCloseAfterDisconnect_ = true;
        disconnectRemote();
        e->ignore();
        return;
    }
    saveMainWindowUiState();
    QMainWindow::closeEvent(e);
}

void MainWindow::resetMainWindowLayoutToDefaults() {
    {
        openscpui::AppSettings settings;
        settings.remove(openscpui::settingskeys::kMainWindowGeometry);
        settings.remove(openscpui::settingskeys::kMainWindowState);
        settings.remove(openscpui::settingskeys::kMainWindowSplitterState);
        settings.remove(openscpui::settingskeys::kMainWindowLeftHeaderState);
        settings.remove(openscpui::settingskeys::kMainWindowRightHeaderLocal);
        settings.remove(openscpui::settingskeys::kMainWindowRightHeaderRemote);
        settings.sync();
    }

    restoredWindowGeometry_ = false;

    resize(1100, 650);
    if (mainSplitter_) {
        const int half = qMax(220, width() / 2);
        mainSplitter_->setSizes({half, half});
    }

    if (leftView_) {
        if (leftView_->header())
            leftView_->header()->setStretchLastSection(true);
        leftView_->setColumnWidth(0, 280);
    }
    if (rightView_) {
        if (rightIsRemote_) {
            if (rightView_->header())
                rightView_->header()->setStretchLastSection(false);
            rightView_->setColumnWidth(0, 300);
            rightView_->setColumnWidth(1, 120);
            rightView_->setColumnWidth(2, 180);
            rightView_->setColumnWidth(3, 120);
        } else {
            if (rightView_->header())
                rightView_->header()->setStretchLastSection(true);
            rightView_->setColumnWidth(0, 280);
        }
    }

    statusBar()->showMessage(tr("Window layout restored to defaults"), 3500);
}

void MainWindow::saveRightHeaderState(bool remoteMode) const {
    if (!rightView_ || !rightView_->header())
        return;
    openscpui::AppSettings settings;
    const QString key =
        remoteMode ? QString::fromLatin1(
                         openscpui::settingskeys::kMainWindowRightHeaderRemote)
                   : QString::fromLatin1(
                         openscpui::settingskeys::kMainWindowRightHeaderLocal);
    settings.setValue(key, rightView_->header()->saveState());
}

bool MainWindow::restoreRightHeaderState(bool remoteMode) {
    if (!rightView_ || !rightView_->header())
        return false;
    openscpui::AppSettings settings;
    const QString key =
        remoteMode ? QString::fromLatin1(
                         openscpui::settingskeys::kMainWindowRightHeaderRemote)
                   : QString::fromLatin1(
                         openscpui::settingskeys::kMainWindowRightHeaderLocal);
    const QByteArray state = settings.value(key).toByteArray();
    if (state.isEmpty())
        return false;
    return rightView_->header()->restoreState(state);
}

void MainWindow::saveMainWindowUiState() const {
    openscpui::AppSettings settings;
    settings.setValue(openscpui::settingskeys::kMainWindowGeometry,
                      saveGeometry());
    settings.setValue(openscpui::settingskeys::kMainWindowState, saveState());
    if (mainSplitter_)
        settings.setValue(openscpui::settingskeys::kMainWindowSplitterState,
                          mainSplitter_->saveState());
    if (leftView_ && leftView_->header())
        settings.setValue(openscpui::settingskeys::kMainWindowLeftHeaderState,
                          leftView_->header()->saveState());
    saveRightHeaderState(rightIsRemote_);
    settings.sync();
}

void MainWindow::restoreMainWindowUiState() {
    openscpui::AppSettings settings;
    const QByteArray geometry =
        settings.value(openscpui::settingskeys::kMainWindowGeometry)
            .toByteArray();
    if (!geometry.isEmpty()) {
        restoreGeometry(geometry);
        restoredWindowGeometry_ = true;
    }
    const QByteArray winState =
        settings.value(openscpui::settingskeys::kMainWindowState).toByteArray();
    if (!winState.isEmpty())
        restoreState(winState);
    if (mainSplitter_) {
        const QByteArray splitterState =
            settings.value(openscpui::settingskeys::kMainWindowSplitterState)
                .toByteArray();
        if (!splitterState.isEmpty())
            mainSplitter_->restoreState(splitterState);
    }
    if (leftView_ && leftView_->header()) {
        const QByteArray leftHeader =
            settings.value(openscpui::settingskeys::kMainWindowLeftHeaderState)
                .toByteArray();
        if (!leftHeader.isEmpty())
            leftView_->header()->restoreState(leftHeader);
    }
    restoreRightHeaderState(false);
}

void MainWindow::refreshLeftPathNavigation() {
    const QString path = leftPath_ ? leftPath_->path() : QString();
    if (leftPath_)
        leftPath_->setPathFlavor(openscpui::PathFlavor::Local);
    refreshFavoriteToggleAction(actFavoriteToggleLeft_, path, false);
}

void MainWindow::refreshRightPathNavigation() {
    const QString path = rightPath_ ? rightPath_->path() : QString();
    if (rightPath_) {
        rightPath_->setPathFlavor(rightIsRemote_
                                      ? openscpui::PathFlavor::Remote
                                      : openscpui::PathFlavor::Local);
    }
    refreshFavoriteToggleAction(actFavoriteToggleRight_, path, rightIsRemote_);
}

void MainWindow::searchItemsInCurrentFolder(QTreeView *view,
                                            const QString &panelLabel) {
    if (!paneController_)
        return;
    openscpui::PaneController::SearchContext context;
    context.dialogParent = this;
    context.statusBar = statusBar();
    context.view = view;
    context.remoteModel = rightRemoteModel_;
    context.remoteOperations = remoteOps_;
    context.panelLabel = panelLabel;
    context.localBasePath = view == leftView_
                                ? (leftPath_ ? leftPath_->path() : QString())
                                : (rightPath_ ? rightPath_->path() : QString());
    context.isRemote = view == rightView_ && rightIsRemote_;
    context.includeHidden = prefShowHidden_;
    context.remoteActivitySucceeded = [this] {
        sessionHealthMonitor_.recordActivity();
    };
    paneController_->search(context);
}

void MainWindow::handleTransferUiUpdate(const QVector<quint64> &upsertIds,
                                        const QVector<quint64> &removedIds) {
    if (!transferMgr_) {
        transferUiController_.reset();
        return;
    }
    const QString remoteRoot =
        rightRemoteModel_ ? rightRemoteModel_->rootPath() : QString();
    const QVector<TransferTask> upserts =
        transferMgr_->tasksSnapshot(upsertIds);
    const openscpui::TransferUiUpdate update = transferUiController_.observe(
        upserts, removedIds, rightIsRemote_ && rightRemoteModel_, remoteRoot);
    if (!update.completionMessage.isEmpty())
        statusBar()->showMessage(update.completionMessage, 5000);
    if (!update.scheduleRemoteRefresh)
        return;

    QTimer::singleShot(150, this, [this] {
        transferUiController_.completeScheduledRefresh();
        if (!rightIsRemote_ || !rightRemoteModel_)
            return;
        requestRemoteListing(rightRemoteModel_->rootPath(), true);
    });
}

bool MainWindow::isScpTransferMode() const {
    const auto &options = sessionController_->options();
    if (!rightIsRemote_ || !options.has_value())
        return false;
    const openscp::ProtocolCapabilities caps =
        openscp::capabilitiesForProtocol(options->protocol);
    return (caps.can_upload || caps.can_download) && !caps.can_list;
}

void MainWindow::activateScpTransferModeUi(bool enabled) {
    if (!rightContentStack_ || !rightView_)
        return;
    if (enabled && scpTransferPanel_) {
        rightContentStack_->setCurrentWidget(scpTransferPanel_);
        if (rightPath_) {
            if (rightPath_->path().trimmed().isEmpty())
                rightPath_->setPath(QStringLiteral("/"));
        }
        if (scpModeHintLabel_) {
            scpModeHintLabel_->setText(
                tr("This protocol works in transfer-only mode.\n"
                   "Uploads use the remote folder path above.\n"
                   "Downloads require entering a remote file path."));
        }
        return;
    }

    rightContentStack_->setCurrentWidget(rightView_);
}

void MainWindow::applyPreferences() {
    openscpui::AppSettings settings;
    const bool showHidden =
        settings.value(openscpui::settingskeys::kUiShowHidden, false).toBool();
    const bool singleClick =
        settings.value(openscpui::settingskeys::kUiSingleClick, false).toBool();
    QString openBehaviorMode =
        settings.value(openscpui::settingskeys::kUiOpenBehaviorMode)
            .toString()
            .trimmed()
            .toLower();
    if (openBehaviorMode.isEmpty()) {
        const bool revealLegacy =
            settings
                .value(openscpui::settingskeys::kUiOpenRevealInFolder, false)
                .toBool();
        openBehaviorMode =
            revealLegacy ? QStringLiteral("reveal") : QStringLiteral("ask");
    }
    if (openBehaviorMode != QStringLiteral("ask") &&
        openBehaviorMode != QStringLiteral("reveal") &&
        openBehaviorMode != QStringLiteral("open")) {
        openBehaviorMode = QStringLiteral("ask");
    }
    prefOpenBehaviorMode_ = openBehaviorMode;
    prefShowQueueOnEnqueue_ =
        settings.value(openscpui::settingskeys::kUiShowQueueOnEnqueue, true)
            .toBool();
    prefNoHostVerificationTtlMin_ = qBound(
        1,
        settings
            .value(openscpui::settingskeys::kNoHostVerificationTtlMinutes, 15)
            .toInt(),
        120);
    const int sessionHealthIntervalMs =
        qBound(
            60,
            settings
                .value(openscpui::settingskeys::kSessionHealthIntervalSeconds,
                       600)
                .toInt(),
            86400) *
        1000;
    sessionHealthMonitor_.setInterval(sessionHealthIntervalMs);
    downloadDir_ = defaultDownloadDirFromSettings(settings);
    QDir().mkpath(downloadDir_);
    if (actShowQueue_) {
        const QString queueShortcutText =
            settings
                .value(openscpui::settingskeys::kShortcutOpenTransfers,
                       QStringLiteral("F12"))
                .toString()
                .trimmed();
        actShowQueue_->setShortcut(QKeySequence::fromString(
            queueShortcutText, QKeySequence::PortableText));
        actShowQueue_->setShortcutContext(Qt::ApplicationShortcut);
    }
    if (actShowHistory_) {
        const QString historyShortcutText =
            settings
                .value(openscpui::settingskeys::kShortcutOpenHistory,
                       QStringLiteral("Ctrl+Shift+H"))
                .toString()
                .trimmed();
        actShowHistory_->setShortcut(QKeySequence::fromString(
            historyShortcutText, QKeySequence::PortableText));
        actShowHistory_->setShortcutContext(Qt::ApplicationShortcut);
    }
    // Keep Site Manager auto-open preference up to date
    openSiteManagerOnDisconnect_ =
        settings
            .value(openscpui::settingskeys::kUiOpenSiteManagerOnDisconnect,
                   true)
            .toBool();
    applyTransferPreferences();

    // Local: model filters (hidden on/off)
    auto applyLocalFilters = [&](QFileSystemModel *fileModel) {
        if (!fileModel)
            return;
        QDir::Filters filters =
            QDir::AllEntries | QDir::NoDotAndDotDot | QDir::AllDirs;
        if (showHidden)
            filters = filters | QDir::Hidden | QDir::System;
        fileModel->setFilter(filters);
    };
    applyLocalFilters(leftModel_);
    applyLocalFilters(rightLocalModel_);

    // Remote: re-list with hidden filter
    prefShowHidden_ = showHidden;
    if (rightRemoteModel_) {
        rightRemoteModel_->setShowHidden(showHidden);
        requestRemoteListing(rightRemoteModel_->rootPath(), true);
    }

    // Single-click activation (connect/disconnect to clicked())
    if (prefSingleClick_ != singleClick) {
        // Disconnect previous connections if they existed
        if (leftClickConn_) {
            QObject::disconnect(leftClickConn_);
            leftClickConn_ = QMetaObject::Connection();
        }
        if (rightClickConn_) {
            QObject::disconnect(rightClickConn_);
            rightClickConn_ = QMetaObject::Connection();
        }
        if (singleClick) {
            if (leftView_)
                leftClickConn_ = connect(leftView_, &QTreeView::clicked, this,
                                         &MainWindow::leftItemActivated);
            if (rightView_)
                rightClickConn_ = connect(rightView_, &QTreeView::clicked, this,
                                          &MainWindow::rightItemActivated);
        }
        prefSingleClick_ = singleClick;
    }
}

void MainWindow::applyTransferPreferences() {
    if (!transferMgr_)
        return;
    openscpui::AppSettings settings;
    const int maxConcurrent = qBound(
        1,
        settings.value(openscpui::settingskeys::kTransferMaxConcurrent, 2)
            .toInt(),
        8);
    const int globalSpeed = qMax(
        0, settings.value(openscpui::settingskeys::kTransferGlobalSpeedKbps, 0)
               .toInt());
    transferMgr_->setMaxConcurrent(maxConcurrent);
    transferMgr_->setGlobalSpeedLimitKBps(globalSpeed);
    if (transferDlg_)
        QMetaObject::invokeMethod(transferDlg_, "refresh",
                                  Qt::QueuedConnection);
}

void MainWindow::addRecentLocalPath(const QString &path) {
    navigationStore_.addRecentLocalPath(path);
}

QString MainWindow::remoteNavigationScope() const {
    if (activeSavedSiteContext_ &&
        !activeSavedSiteContext_->siteId.trimmed().isEmpty()) {
        return openscpui::savedSiteNavigationScope(
            activeSavedSiteContext_->siteId);
    }
    const auto &options = sessionController_->options();
    if (!options)
        return {};
    return openscpui::remoteEndpointScope(*options);
}

void MainWindow::showOpenPathDialog(bool rightPane) {
    const bool remote = rightPane && rightIsRemote_;
    const auto location = remote ? openscpui::NavigationStore::Location::Remote
                                 : openscpui::NavigationStore::Location::Local;
    const QString remoteScope = remote ? remoteNavigationScope() : QString();
    const QString currentPath =
        rightPane ? (rightPath_ ? rightPath_->path() : QString())
                  : (leftPath_ ? leftPath_->path() : QString());
    const QStringList recentPaths =
        remote ? navigationStore_.recentRemotePaths(remoteScope)
               : navigationStore_.recentLocalPaths();

    openscpui::OpenPathDialog dialog(
        currentPath, recentPaths,
        navigationStore_.favorites(location, remoteScope),
        remote ? Qt::CaseSensitive : Qt::CaseInsensitive,
        mainWindowActionIcon(remote ? "action-open-folder-remote.svg"
                                    : "action-open-folder.svg"),
        this);

    const auto refreshDialogFavorites = [&] {
        dialog.setFavorites(navigationStore_.favorites(location, remoteScope));
    };
    connect(&dialog, &openscpui::OpenPathDialog::addFavoriteRequested, &dialog,
            [&, location, remote, remoteScope](const QString &path) {
                const QString normalized =
                    remote
                        ? openscpui::NavigationStore::normalizeRemotePath(path)
                        : openscpui::NavigationStore::normalizeLocalPath(path);
                if (normalized.isEmpty() || (remote && remoteScope.isEmpty())) {
                    return;
                }
                if (!navigationStore_.isFavorite(location, normalized,
                                                 remoteScope)) {
                    navigationStore_.toggleFavorite(location, normalized,
                                                    remoteScope);
                    statusBar()->showMessage(tr("Favorite added"), 2500);
                }
                refreshFavoritesActions();
                refreshDialogFavorites();
            });
    connect(
        &dialog, &openscpui::OpenPathDialog::removeFavoriteRequested, &dialog,
        [&, location, remote, remoteScope](const QString &path) {
            const QString normalized =
                remote ? openscpui::NavigationStore::normalizeRemotePath(path)
                       : openscpui::NavigationStore::normalizeLocalPath(path);
            if (normalized.isEmpty() ||
                !navigationStore_.isFavorite(location, normalized,
                                             remoteScope)) {
                return;
            }
            navigationStore_.toggleFavorite(location, normalized, remoteScope);
            refreshFavoritesActions();
            refreshDialogFavorites();
            statusBar()->showMessage(tr("Favorite removed"), 2500);
        });

    if (dialog.exec() != QDialog::Accepted)
        return;

    const QString selectedPath = dialog.selectedPath();
    if (remote)
        setRightRemoteRoot(selectedPath);
    else if (rightPane)
        setRightRoot(selectedPath);
    else
        setLeftRoot(selectedPath);
}

void MainWindow::toggleCurrentFavorite(bool rightPane) {
    const bool remote = rightPane && rightIsRemote_;
    const auto location = remote ? openscpui::NavigationStore::Location::Remote
                                 : openscpui::NavigationStore::Location::Local;
    const QString remoteScope = remote ? remoteNavigationScope() : QString();
    if (remote && remoteScope.isEmpty()) {
        statusBar()->showMessage(
            tr("Connect to a remote server to use remote favorites"), 3500);
        return;
    }

    const QString currentPath =
        rightPane ? (rightPath_ ? rightPath_->path() : QString())
                  : (leftPath_ ? leftPath_->path() : QString());
    const QString normalizedCurrent =
        remote ? openscpui::NavigationStore::normalizeRemotePath(currentPath)
               : openscpui::NavigationStore::normalizeLocalPath(currentPath);
    if (normalizedCurrent.isEmpty()) {
        refreshFavoritesActions();
        return;
    }

    const bool added = navigationStore_.toggleFavorite(
        location, normalizedCurrent, remoteScope);
    refreshFavoritesActions();
    statusBar()->showMessage(
        added ? tr("Favorite added") : tr("Favorite removed"), 2500);
}

void MainWindow::refreshFavoriteToggleAction(QAction *favoriteAction,
                                             const QString &currentPath,
                                             bool remote) {
    if (!favoriteAction)
        return;

    const auto location = remote ? openscpui::NavigationStore::Location::Remote
                                 : openscpui::NavigationStore::Location::Local;
    const QString remoteScope = remote ? remoteNavigationScope() : QString();
    if (remote && remoteScope.isEmpty()) {
        favoriteAction->setChecked(false);
        favoriteAction->setEnabled(false);
        favoriteAction->setIcon(
            mainWindowActionIcon("action-favorite-inactive.svg"));
        favoriteAction->setText(
            tr("Connect to a remote server to use remote favorites"));
        favoriteAction->setToolTip(
            tr("Connect to a remote server to use remote favorites"));
        return;
    }

    const QString normalizedCurrent =
        remote ? openscpui::NavigationStore::normalizeRemotePath(currentPath)
               : openscpui::NavigationStore::normalizeLocalPath(currentPath);
    if (normalizedCurrent.isEmpty()) {
        favoriteAction->setChecked(false);
        favoriteAction->setEnabled(false);
        favoriteAction->setIcon(
            mainWindowActionIcon("action-favorite-inactive.svg"));
        favoriteAction->setText(tr("Add current path to favorites"));
        favoriteAction->setToolTip(tr("Add current path to favorites"));
        return;
    }

    const bool currentIsFavorite =
        navigationStore_.isFavorite(location, normalizedCurrent, remoteScope);
    const QString actionText = currentIsFavorite
                                   ? tr("Remove current path from favorites")
                                   : tr("Add current path to favorites");
    favoriteAction->setEnabled(true);
    favoriteAction->setChecked(currentIsFavorite);
    favoriteAction->setIcon(mainWindowActionIcon(
        currentIsFavorite ? "action-favorite-active.svg"
                          : "action-favorite-inactive.svg"));
    favoriteAction->setText(actionText);
    favoriteAction->setToolTip(actionText);
}

void MainWindow::refreshFavoritesActions() {
    refreshFavoriteToggleAction(actFavoriteToggleLeft_,
                                leftPath_ ? leftPath_->path() : QString(),
                                false);
    refreshFavoriteToggleAction(actFavoriteToggleRight_,
                                rightPath_ ? rightPath_->path() : QString(),
                                rightIsRemote_);
}

void MainWindow::addRecentRemotePath(const QString &path) {
    navigationStore_.addRecentRemotePath(remoteNavigationScope(), path);
}

void MainWindow::addRecentServer(const openscp::SessionOptions &opt) {
    navigationStore_.addRecentServer(opt);
}

void MainWindow::showFavoritesDialog() {
    QWidget *focusBeforeDialog = QApplication::focusWidget();

    QDialog dlg(this);
    dlg.setObjectName(QStringLiteral("favoritesDialog"));
    dlg.setWindowTitle(tr("Favorites"));
    dlg.resize(640, 420);
    dlg.setMinimumSize(520, 340);

    auto *layout = new QVBoxLayout(&dlg);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    auto *tabs = new QTabWidget(&dlg);
    tabs->setObjectName(QStringLiteral("favoritesTabs"));
    layout->addWidget(tabs, 1);

    auto *localList = createNavigationTabList(tabs, tr("Local"));
    auto *remoteList = createNavigationTabList(tabs, tr("Remote"));
    localList->setObjectName(QStringLiteral("localFavoritesList"));
    remoteList->setObjectName(QStringLiteral("remoteFavoritesList"));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
    auto *openBtn =
        buttons->addButton(tr("Open selected"), QDialogButtonBox::ActionRole);
    auto *removeBtn = buttons->addButton(tr("Remove from favorites"),
                                         QDialogButtonBox::ActionRole);
    auto *clearBtn =
        buttons->addButton(tr("Clear favorites"), QDialogButtonBox::ActionRole);
    openBtn->setObjectName(QStringLiteral("openFavoriteButton"));
    removeBtn->setObjectName(QStringLiteral("removeFavoriteButton"));
    clearBtn->setObjectName(QStringLiteral("clearFavoritesButton"));
    layout->addWidget(buttons);

    auto activeList = [tabs, localList, remoteList]() -> QListWidget * {
        return tabs->currentIndex() == 1 ? remoteList : localList;
    };
    auto currentRemoteScope = [this]() {
        return rightIsRemote_ ? remoteNavigationScope() : QString();
    };
    auto updateActions = [=]() {
        QListWidget *list = activeList();
        const bool remote = tabs->currentIndex() == 1;
        const bool remoteAvailable = !remote || !currentRemoteScope().isEmpty();
        const QString selected =
            list && list->currentItem()
                ? list->currentItem()->data(Qt::UserRole).toString()
                : QString();
        const bool hasSelection =
            remoteAvailable && !selected.trimmed().isEmpty();
        openBtn->setEnabled(hasSelection);
        removeBtn->setEnabled(hasSelection);
        clearBtn->setEnabled(remoteAvailable && listHasUserData(list));
    };

    auto populate = [=, this]() {
        localList->clear();
        remoteList->clear();

        const QStringList localPaths = navigationStore_.favorites(
            openscpui::NavigationStore::Location::Local);
        for (const QString &rawPath : localPaths) {
            const QString path =
                openscpui::NavigationStore::normalizeLocalPath(rawPath);
            if (path.isEmpty())
                continue;
            auto *item = new QListWidgetItem(
                trimNavigationLabel(QDir::toNativeSeparators(path)), localList);
            item->setToolTip(path);
            item->setData(Qt::UserRole, path);
        }
        if (!listHasUserData(localList))
            addDisabledListPlaceholder(localList, tr("No favorites"));

        const QString remoteScope = currentRemoteScope();
        if (remoteScope.isEmpty()) {
            addDisabledListPlaceholder(
                remoteList,
                tr("Connect to a remote server to use remote favorites"));
        } else {
            const QStringList remotePaths = navigationStore_.favorites(
                openscpui::NavigationStore::Location::Remote, remoteScope);
            for (const QString &rawPath : remotePaths) {
                const QString path =
                    openscpui::NavigationStore::normalizeRemotePath(rawPath);
                if (path.isEmpty())
                    continue;
                auto *item =
                    new QListWidgetItem(trimNavigationLabel(path), remoteList);
                item->setToolTip(path);
                item->setData(Qt::UserRole, path);
            }
            if (!listHasUserData(remoteList))
                addDisabledListPlaceholder(remoteList, tr("No favorites"));
        }

        updateActions();
    };

    auto openSelected = [&, focusBeforeDialog]() {
        QListWidget *list = activeList();
        if (!list || !list->currentItem())
            return;
        const QString path = list->currentItem()->data(Qt::UserRole).toString();
        if (path.trimmed().isEmpty())
            return;

        if (tabs->currentIndex() == 1) {
            if (!rightIsRemote_ || currentRemoteScope().isEmpty()) {
                statusBar()->showMessage(
                    tr("Connect to a remote server to use remote favorites"),
                    3500);
                return;
            }
            setRightRemoteRoot(path);
        } else {
            const bool inRightPanel =
                focusWithinWidget(focusBeforeDialog, rightView_) ||
                focusWithinWidget(focusBeforeDialog, rightPath_) ||
                focusWithinWidget(focusBeforeDialog, rightPaneBar_);
            if (!rightIsRemote_ && inRightPanel)
                setRightRoot(path);
            else
                setLeftRoot(path);
        }
        dlg.accept();
    };

    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    connect(openBtn, &QPushButton::clicked, &dlg, openSelected);
    connect(removeBtn, &QPushButton::clicked, &dlg, [=, this]() {
        QListWidget *list = activeList();
        if (!list || !list->currentItem())
            return;
        const QString path = list->currentItem()->data(Qt::UserRole).toString();
        const bool remote = tabs->currentIndex() == 1;
        const QString remoteScope = remote ? currentRemoteScope() : QString();
        if (path.isEmpty() || (remote && remoteScope.isEmpty()))
            return;

        const auto location = remote
                                  ? openscpui::NavigationStore::Location::Remote
                                  : openscpui::NavigationStore::Location::Local;
        if (!navigationStore_.isFavorite(location, path, remoteScope)) {
            populate();
            return;
        }
        navigationStore_.toggleFavorite(location, path, remoteScope);
        refreshFavoritesActions();
        statusBar()->showMessage(tr("Favorite removed"), 2500);
        populate();
    });
    connect(clearBtn, &QPushButton::clicked, &dlg, [=, this]() {
        const bool remote = tabs->currentIndex() == 1;
        const QString remoteScope = remote ? currentRemoteScope() : QString();
        if (remote && remoteScope.isEmpty())
            return;
        if (UiAlerts::question(this, tr("Clear favorites"),
                               tr("Remove all favorites in this section?"),
                               QMessageBox::Yes | QMessageBox::No,
                               QMessageBox::No) != QMessageBox::Yes) {
            return;
        }

        navigationStore_.clearFavorites(
            remote ? openscpui::NavigationStore::Location::Remote
                   : openscpui::NavigationStore::Location::Local,
            remoteScope);
        refreshFavoritesActions();
        populate();
    });

    for (QListWidget *list : {localList, remoteList}) {
        connect(list, &QListWidget::itemDoubleClicked, &dlg,
                [openSelected](QListWidgetItem *) { openSelected(); });
        connect(list, &QListWidget::currentRowChanged, &dlg,
                [updateActions](int) { updateActions(); });
    }
    connect(tabs, &QTabWidget::currentChanged, &dlg,
            [updateActions](int) { updateActions(); });

    populate();
    dlg.exec();
}

void MainWindow::showHistoryMenu() {
    QWidget *focusBeforeDialog = QApplication::focusWidget();

    QDialog dlg(this);
    dlg.setWindowTitle(tr("History"));
    dlg.resize(720, 460);
    dlg.setMinimumSize(560, 360);

    auto *layout = new QVBoxLayout(&dlg);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    auto *tabs = new QTabWidget(&dlg);
    layout->addWidget(tabs, 1);

    auto *localList = createNavigationTabList(tabs, tr("Recent local paths"));
    auto *remoteList = createNavigationTabList(tabs, tr("Recent remote paths"));
    auto *legacyRemoteList =
        createNavigationTabList(tabs, tr("Unscoped legacy"));
    auto *serverList = createNavigationTabList(tabs, tr("Recent servers"));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
    auto *openBtn =
        buttons->addButton(tr("Open selected"), QDialogButtonBox::ActionRole);
    auto *favoriteBtn = buttons->addButton(tr("Add to favorites"),
                                           QDialogButtonBox::ActionRole);
    auto *clearBtn =
        buttons->addButton(tr("Clear history"), QDialogButtonBox::ActionRole);
    layout->addWidget(buttons);

    auto activeList = [tabs, localList, remoteList, legacyRemoteList,
                       serverList]() -> QListWidget * {
        switch (tabs->currentIndex()) {
        case 0:
            return localList;
        case 1:
            return remoteList;
        case 2:
            return legacyRemoteList;
        case 3:
            return serverList;
        default:
            return nullptr;
        }
    };

    auto updateActions = [=, this]() {
        if (!openBtn)
            return;
        QListWidget *list = activeList();
        if (!list || !list->currentItem()) {
            openBtn->setEnabled(false);
            if (favoriteBtn) {
                favoriteBtn->setEnabled(false);
                favoriteBtn->setText(tr("Add to favorites"));
            }
            return;
        }
        const QString value =
            list->currentItem()->data(Qt::UserRole).toString();
        openBtn->setEnabled(!value.trimmed().isEmpty());
        if (!favoriteBtn)
            return;

        const bool localPath = tabs->currentIndex() == 0;
        const bool remotePath = tabs->currentIndex() == 1;
        const auto location =
            localPath ? openscpui::NavigationStore::Location::Local
                      : openscpui::NavigationStore::Location::Remote;
        const QString remoteScope =
            remotePath ? remoteNavigationScope() : QString();
        const QString normalized =
            localPath
                ? openscpui::NavigationStore::normalizeLocalPath(value)
                : (remotePath
                       ? openscpui::NavigationStore::normalizeRemotePath(value)
                       : QString());
        if ((!localPath && (!remotePath || remoteScope.isEmpty())) ||
            normalized.isEmpty()) {
            favoriteBtn->setEnabled(false);
            favoriteBtn->setText(tr("Add to favorites"));
            return;
        }

        const bool alreadyFavorite =
            navigationStore_.isFavorite(location, normalized, remoteScope);
        favoriteBtn->setEnabled(true);
        favoriteBtn->setText(alreadyFavorite ? tr("Remove from favorites")
                                             : tr("Add to favorites"));
    };

    auto populate = [=, this]() {
        localList->clear();
        remoteList->clear();
        legacyRemoteList->clear();
        serverList->clear();

        const QStringList localPaths = navigationStore_.recentLocalPaths();
        const QStringList remotePaths =
            navigationStore_.recentRemotePaths(remoteNavigationScope());
        const QStringList legacyRemotePaths =
            navigationStore_.legacyRemotePaths();
        const QStringList recentServers = navigationStore_.recentServers();

        bool hasEntries = false;

        for (const QString &rawPath : localPaths) {
            const QString normalized =
                openscpui::NavigationStore::normalizeLocalPath(rawPath);
            if (normalized.isEmpty())
                continue;
            auto *item = new QListWidgetItem(
                trimNavigationLabel(QDir::toNativeSeparators(normalized)),
                localList);
            item->setToolTip(normalized);
            item->setData(Qt::UserRole, normalized);
            hasEntries = true;
        }
        if (localList->count() == 0)
            addDisabledListPlaceholder(localList, tr("No recent history"));

        for (const QString &rawPath : remotePaths) {
            const QString normalized =
                openscpui::NavigationStore::normalizeRemotePath(rawPath);
            if (normalized.isEmpty())
                continue;
            auto *item = new QListWidgetItem(trimNavigationLabel(normalized),
                                             remoteList);
            item->setToolTip(normalized);
            item->setData(Qt::UserRole, normalized);
            hasEntries = true;
        }
        if (remoteList->count() == 0)
            addDisabledListPlaceholder(remoteList, tr("No recent history"));

        for (const QString &rawPath : legacyRemotePaths) {
            const QString normalized =
                openscpui::NavigationStore::normalizeRemotePath(rawPath);
            if (normalized.isEmpty())
                continue;
            auto *item = new QListWidgetItem(trimNavigationLabel(normalized),
                                             legacyRemoteList);
            item->setToolTip(tr("Legacy path without a server identity: %1")
                                 .arg(normalized));
            item->setData(Qt::UserRole, normalized);
            hasEntries = true;
        }
        if (legacyRemoteList->count() == 0)
            addDisabledListPlaceholder(legacyRemoteList,
                                       tr("No legacy history"));

        for (const QString &encoded : recentServers) {
            openscp::SessionOptions preset;
            QString label;
            if (!openscpui::NavigationStore::decodeRecentServer(
                    encoded, &preset, &label)) {
                continue;
            }
            auto *item =
                new QListWidgetItem(trimNavigationLabel(label), serverList);
            item->setToolTip(label);
            item->setData(Qt::UserRole, encoded);
            hasEntries = true;
        }
        if (serverList->count() == 0)
            addDisabledListPlaceholder(serverList, tr("No recent history"));

        if (clearBtn)
            clearBtn->setEnabled(hasEntries);
        updateActions();
    };

    auto openSelected = [&, focusBeforeDialog]() {
        QListWidget *list = activeList();
        if (!list || !list->currentItem())
            return;
        const QString value =
            list->currentItem()->data(Qt::UserRole).toString();
        if (value.trimmed().isEmpty())
            return;

        switch (tabs->currentIndex()) {
        case 0: {
            const bool inRightPanel =
                focusWithinWidget(focusBeforeDialog, rightView_) ||
                focusWithinWidget(focusBeforeDialog, rightPath_) ||
                focusWithinWidget(focusBeforeDialog, rightPaneBar_);
            if (!rightIsRemote_ && inRightPanel)
                setRightRoot(value);
            else
                setLeftRoot(value);
            dlg.accept();
            break;
        }
        case 1:
            if (!rightIsRemote_) {
                statusBar()->showMessage(tr("Connect to a remote server to "
                                            "open remote path history."),
                                         3500);
                return;
            }
            setRightRemoteRoot(value);
            dlg.accept();
            break;
        case 2:
            if (!rightIsRemote_) {
                statusBar()->showMessage(
                    tr("Connect to a remote server to open legacy remote "
                       "history."),
                    3500);
                return;
            }
            if (UiAlerts::question(
                    this, tr("Open unscoped legacy path?"),
                    tr("This path is not tied to a saved site or endpoint and "
                       "may belong to another server.\n\n"
                       "Open it on the current server?"),
                    QMessageBox::Yes | QMessageBox::No,
                    QMessageBox::No) != QMessageBox::Yes) {
                return;
            }
            setRightRemoteRoot(value);
            dlg.accept();
            break;
        case 3: {
            if (rightIsRemote_) {
                statusBar()->showMessage(
                    tr("Disconnect the current remote session before opening "
                       "another server."),
                    4000);
                return;
            }
            openscp::SessionOptions preset;
            if (!openscpui::NavigationStore::decodeRecentServer(value, &preset,
                                                                nullptr)) {
                return;
            }
            dlg.accept();
            openConnectDialogWithPreset(preset);
            break;
        }
        default:
            break;
        }
    };

    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    connect(openBtn, &QPushButton::clicked, &dlg, openSelected);
    connect(favoriteBtn, &QPushButton::clicked, &dlg, [=, this]() {
        QListWidget *list = activeList();
        if (!list || !list->currentItem())
            return;
        const QString value =
            list->currentItem()->data(Qt::UserRole).toString();
        const bool localPath = tabs->currentIndex() == 0;
        const bool remotePath = tabs->currentIndex() == 1;
        const auto location =
            localPath ? openscpui::NavigationStore::Location::Local
                      : openscpui::NavigationStore::Location::Remote;
        const QString remoteScope =
            remotePath ? remoteNavigationScope() : QString();
        const QString normalized =
            localPath
                ? openscpui::NavigationStore::normalizeLocalPath(value)
                : (remotePath
                       ? openscpui::NavigationStore::normalizeRemotePath(value)
                       : QString());
        if ((!localPath && (!remotePath || remoteScope.isEmpty())) ||
            normalized.isEmpty()) {
            return;
        }

        const bool added =
            navigationStore_.toggleFavorite(location, normalized, remoteScope);
        refreshFavoritesActions();
        statusBar()->showMessage(
            added ? tr("Favorite added") : tr("Favorite removed"), 2500);
        updateActions();
    });
    connect(clearBtn, &QPushButton::clicked, &dlg, [=, this]() {
        const auto ret = QMessageBox::question(
            this, tr("Clear history"),
            tr("Remove all recent paths and servers from history?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (ret != QMessageBox::Yes)
            return;
        navigationStore_.clearAllHistory();
        statusBar()->showMessage(tr("History cleared"), 3000);
        populate();
    });

    for (QListWidget *list :
         {localList, remoteList, legacyRemoteList, serverList}) {
        connect(list, &QListWidget::itemDoubleClicked, &dlg,
                [openSelected](QListWidgetItem *) { openSelected(); });
        connect(list, &QListWidget::currentRowChanged, &dlg,
                [updateActions](int) { updateActions(); });
    }
    connect(tabs, &QTabWidget::currentChanged, &dlg,
            [updateActions](int) { updateActions(); });

    populate();
    dlg.exec();
}

void MainWindow::updateDeleteShortcutEnables() {
    auto hasColSel = [&](QTreeView *treeView) -> bool {
        if (!treeView || !treeView->selectionModel())
            return false;
        return !treeView->selectionModel()->selectedRows(kNameColumn).isEmpty();
    };
    const bool leftHasSel = hasColSel(leftView_);
    const bool rightHasSel = hasColSel(rightView_);
    const bool scpMode = isScpTransferMode();
    const bool rightWrite =
        (!rightIsRemote_) || (rightIsRemote_ && rightRemoteMutationsSupported_);

    // Left: enable according to selection (exception: Up)
    if (actCopyF5_)
        actCopyF5_->setEnabled(leftHasSel);
    if (actMoveF6_)
        actMoveF6_->setEnabled(leftHasSel && !scpMode);
    if (actDelete_)
        actDelete_->setEnabled(leftHasSel);
    if (actRenameLeft_)
        actRenameLeft_->setEnabled(leftHasSel);
    if (actNewDirLeft_)
        actNewDirLeft_->setEnabled(true); // always enabled on local
    if (actNewFileLeft_)
        actNewFileLeft_->setEnabled(true); // always enabled on local
    if (actUpLeft_) {
        QDir leftDir(leftPath_ ? leftPath_->path() : QString());
        bool canUp = leftDir.cdUp();
        actUpLeft_->setEnabled(canUp);
    }

    // Right: enable according to selection + permissions (exceptions: Up,
    // Upload, Download)
    if (scpMode) {
        if (actCopyRightTb_)
            actCopyRightTb_->setEnabled(false);
        if (actMoveRightTb_)
            actMoveRightTb_->setEnabled(false);
        if (actDeleteRight_)
            actDeleteRight_->setEnabled(false);
        if (actRenameRight_)
            actRenameRight_->setEnabled(false);
        if (actNewDirRight_)
            actNewDirRight_->setEnabled(false);
        if (actNewFileRight_)
            actNewFileRight_->setEnabled(false);
        if (actUploadRight_)
            actUploadRight_->setEnabled(true);
        if (actDownloadF7_)
            actDownloadF7_->setEnabled(true);
        if (actRefreshRight_)
            actRefreshRight_->setEnabled(false);
        if (actOpenTerminalRight_)
            actOpenTerminalRight_->setEnabled(true);
        if (actSearchRight_)
            actSearchRight_->setEnabled(false);
        if (actMoveRight_)
            actMoveRight_->setEnabled(false);
        if (actCopyRight_)
            actCopyRight_->setEnabled(false);
        if (actUpRight_) {
            const QString cur = normalizeRemotePath(
                rightPath_ ? rightPath_->path() : QString());
            actUpRight_->setEnabled(cur != "/");
        }
        return;
    }

    if (actCopyRightTb_)
        actCopyRightTb_->setEnabled(rightHasSel);
    if (actMoveRightTb_)
        actMoveRightTb_->setEnabled(rightHasSel && rightWrite);
    if (actDeleteRight_)
        actDeleteRight_->setEnabled(rightHasSel && rightWrite);
    if (actRenameRight_)
        actRenameRight_->setEnabled(rightHasSel && rightWrite);
    if (actNewDirRight_)
        actNewDirRight_->setEnabled(
            rightWrite); // enabled if local or remote is writable
    if (actNewFileRight_)
        actNewFileRight_->setEnabled(rightWrite);
    if (actUploadRight_)
        actUploadRight_->setEnabled(
            rightIsRemote_ && rightRemoteMutationsSupported_); // exception
    if (actDownloadF7_)
        actDownloadF7_->setEnabled(
            rightIsRemote_); // exception: enabled without selection
    if (actRefreshRight_)
        actRefreshRight_->setEnabled(
            rightIsRemote_); // exception: enabled without selection
    if (actOpenTerminalRight_)
        actOpenTerminalRight_->setEnabled(
            rightIsRemote_ && sessionController_->options().has_value());
    if (actSearchRight_)
        actSearchRight_->setEnabled(true);
    if (actUpRight_) {
        QString cur = rightRemoteModel_ ? rightRemoteModel_->rootPath()
                                        : rightPath_->path();
        if (rightIsRemote_) {
            cur = normalizeRemotePath(cur);
            actUpRight_->setEnabled(!cur.isEmpty() && cur != "/");
        } else {
            QDir currentDir(cur);
            actUpRight_->setEnabled(currentDir.cdUp());
        }
    }
}
