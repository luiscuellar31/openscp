// MainWindow orchestrator: builds UI shell, wires actions, and applies
// preferences. Detailed local/remote/transfer logic lives in dedicated units.
#include "MainWindow.hpp"
#include "AboutDialog.hpp"
#include "ConnectionDialog.hpp"
#include "DragAwareTreeView.hpp"
#include "PermissionsDialog.hpp"
#include "RemoteModel.hpp"
#include "SecretStore.hpp"
#include "SettingsDialog.hpp"
#include "SiteManagerDialog.hpp"
#include "TransferManager.hpp"
#include "TransferQueueDialog.hpp"
#include "openscp/Libssh2SftpClient.hpp"
#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QDirIterator>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEasingCurve>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHash>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QListView>
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
#include <QSet>
#include <QSettings>
#include <QShortcut>
#include <QShowEvent>
#include <QSize>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QTemporaryFile>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>
#include <atomic>
#include <cstring>
#include <memory>
#include <thread>

static constexpr int NAME_COL = 0;

MainWindow::~MainWindow() = default; // define the destructor here

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    // Globally center dialogs relative to the main window
    qApp->installEventFilter(this);
    // Models
    leftModel_ = new QFileSystemModel(this);
    rightLocalModel_ = new QFileSystemModel(this);

    leftModel_->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot |
                          QDir::AllDirs);
    rightLocalModel_->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot |
                                QDir::AllDirs);

    // Initial paths: HOME
    const QString home = QDir::homePath();
    leftModel_->setRootPath(home);
    rightLocalModel_->setRootPath(home);

    // Views
    leftView_ = new DragAwareTreeView(this);
    rightView_ = new DragAwareTreeView(this);

    leftView_->setModel(leftModel_);
    rightView_->setModel(rightLocalModel_);
    // Avoid expanding subtrees on double‑click; navigate by changing root
    leftView_->setExpandsOnDoubleClick(false);
    rightView_->setExpandsOnDoubleClick(false);
    leftView_->setRootIndex(leftModel_->index(home));
    rightView_->setRootIndex(rightLocalModel_->index(home));

    // Basic view tuning
    auto tuneView = [](QTreeView *v) {
        v->setSelectionMode(QAbstractItemView::ExtendedSelection);
        v->setSortingEnabled(true);
        v->sortByColumn(0, Qt::AscendingOrder);
        v->header()->setStretchLastSection(true);
        v->setColumnWidth(0, 280);
    };
    tuneView(leftView_);
    tuneView(rightView_);
    leftView_->setDragEnabled(true);
    rightView_->setDragEnabled(true); // allow starting drags from right pane

    // Accept drops on both panes
    rightView_->setAcceptDrops(true);
    rightView_->setDragDropMode(QAbstractItemView::DragDrop);
    rightView_->viewport()->setAcceptDrops(true);
    rightView_->setDefaultDropAction(Qt::CopyAction);
    leftView_->setAcceptDrops(true);
    leftView_->setDragDropMode(QAbstractItemView::DragDrop);
    leftView_->viewport()->setAcceptDrops(true);
    leftView_->setDefaultDropAction(Qt::CopyAction);
    // Install event filters on viewports to receive drag/drop
    rightView_->viewport()->installEventFilter(this);
    leftView_->viewport()->installEventFilter(this);

    // Path entries (top)
    leftPath_ = new QLineEdit(home, this);
    rightPath_ = new QLineEdit(home, this);
    connect(leftPath_, &QLineEdit::returnPressed, this,
            &MainWindow::leftPathEntered);
    connect(rightPath_, &QLineEdit::returnPressed, this,
            &MainWindow::rightPathEntered);

    // Central splitter with two panes
    auto *splitter = new QSplitter(this);
    auto *leftPane = new QWidget(this);
    auto *rightPane = new QWidget(this);

    auto *leftLayout = new QVBoxLayout(leftPane);
    auto *rightLayout = new QVBoxLayout(rightPane);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setContentsMargins(0, 0, 0, 0);

    // Left pane sub‑toolbar
    leftPaneBar_ = new QToolBar("LeftBar", leftPane);
    leftPaneBar_->setIconSize(QSize(18, 18));
    leftPaneBar_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    // Helper for icons from local resources
    auto resIcon = [](const char *fname) -> QIcon {
        return QIcon(QStringLiteral(":/assets/icons/") + QLatin1String(fname));
    };
    // Left sub‑toolbar: Up, Copy, Move, Delete, Rename, New folder
    actUpLeft_ = leftPaneBar_->addAction(tr("Up"), this, &MainWindow::goUpLeft);
    actUpLeft_->setIcon(resIcon("action-go-up.svg"));
    actUpLeft_->setToolTip(actUpLeft_->text());
    // Button "Open left folder" next to Up
    actChooseLeft_ = leftPaneBar_->addAction(tr("Open left folder"), this,
                                             &MainWindow::chooseLeftDir);
    actChooseLeft_->setIcon(resIcon("action-open-folder.svg"));
    actChooseLeft_->setToolTip(actChooseLeft_->text());
    leftPaneBar_->addSeparator();
    actCopyF5_ =
        leftPaneBar_->addAction(tr("Copy"), this, &MainWindow::copyLeftToRight);
    actCopyF5_->setIcon(resIcon("action-copy.svg"));
    actCopyF5_->setToolTip(actCopyF5_->text());
    // Shortcut F5 on left panel (scope: left view and its children)
    actCopyF5_->setShortcut(QKeySequence(Qt::Key_F5));
    actCopyF5_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    if (leftView_)
        leftView_->addAction(actCopyF5_);
    actMoveF6_ =
        leftPaneBar_->addAction(tr("Move"), this, &MainWindow::moveLeftToRight);
    actMoveF6_->setIcon(resIcon("action-move-to-right.svg"));
    actMoveF6_->setToolTip(actMoveF6_->text());
    // Shortcut F6 on left panel
    actMoveF6_->setShortcut(QKeySequence(Qt::Key_F6));
    actMoveF6_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    if (leftView_)
        leftView_->addAction(actMoveF6_);
    actDelete_ = leftPaneBar_->addAction(tr("Delete"), this,
                                         &MainWindow::deleteFromLeft);
    actDelete_->setIcon(resIcon("action-delete.svg"));
    actDelete_->setToolTip(actDelete_->text());
    actDelete_->setShortcut(QKeySequence(Qt::Key_Delete));
    actDelete_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    if (leftView_)
        leftView_->addAction(actDelete_);
    // Action: copy from right panel to left (remote/local -> left)
    actCopyRight_ = new QAction(tr("Copy to left panel"), this);
    connect(actCopyRight_, &QAction::triggered, this,
            &MainWindow::copyRightToLeft);
    actCopyRight_->setIcon(
        QIcon(QLatin1String(":/assets/icons/action-copy.svg")));
    // Action: move from right panel to left
    actMoveRight_ = new QAction(tr("Move to left panel"), this);
    connect(actMoveRight_, &QAction::triggered, this,
            &MainWindow::moveRightToLeft);
    actMoveRight_->setIcon(
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
    actRenameLeft_->setIcon(resIcon("action-rename.svg"));
    actRenameLeft_->setToolTip(actRenameLeft_->text());
    actNewDirLeft_->setIcon(resIcon("action-new-folder.svg"));
    actNewDirLeft_->setToolTip(actNewDirLeft_->text());
    actNewFileLeft_->setIcon(resIcon("action-new-file.svg"));
    actNewFileLeft_->setToolTip(actNewFileLeft_->text());
    // Shortcuts (left panel scope)
    actRenameLeft_->setShortcut(QKeySequence(Qt::Key_F2));
    actRenameLeft_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    if (leftView_)
        leftView_->addAction(actRenameLeft_);
    actNewDirLeft_->setShortcut(QKeySequence(Qt::Key_F9));
    actNewDirLeft_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    if (leftView_)
        leftView_->addAction(actNewDirLeft_);
    actNewFileLeft_->setShortcut(QKeySequence(Qt::Key_F10));
    actNewFileLeft_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    if (leftView_)
        leftView_->addAction(actNewFileLeft_);
    leftPaneBar_->addAction(actRenameLeft_);
    leftPaneBar_->addAction(actNewFileLeft_);
    leftPaneBar_->addAction(actNewDirLeft_);
    leftLayout->addWidget(leftPaneBar_);

    // Left panel: toolbar -> path -> view
    leftLayout->addWidget(leftPath_);
    leftLayout->addWidget(leftView_);

    // Right pane sub‑toolbar
    rightPaneBar_ = new QToolBar("RightBar", rightPane);
    rightPaneBar_->setIconSize(QSize(18, 18));
    rightPaneBar_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    actUpRight_ =
        rightPaneBar_->addAction(tr("Up"), this, &MainWindow::goUpRight);
    actUpRight_->setIcon(resIcon("action-go-up.svg"));
    actUpRight_->setToolTip(actUpRight_->text());
    // Button "Open right folder" next to Up
    actChooseRight_ = rightPaneBar_->addAction(tr("Open right folder"), this,
                                               &MainWindow::chooseRightDir);
    actChooseRight_->setIcon(resIcon("action-open-folder.svg"));
    actChooseRight_->setToolTip(actChooseRight_->text());

    // Right panel actions (create first, then add in requested order)
    actDownloadF7_ = new QAction(tr("Download"), this);
    connect(actDownloadF7_, &QAction::triggered, this,
            &MainWindow::downloadRightToLeft);
    actDownloadF7_->setEnabled(false); // starts disabled on local
    actDownloadF7_->setIcon(resIcon("action-download.svg"));
    actDownloadF7_->setToolTip(actDownloadF7_->text());

    actUploadRight_ = new QAction(tr("Upload…"), this);
    connect(actUploadRight_, &QAction::triggered, this,
            &MainWindow::uploadViaDialog);
    actUploadRight_->setIcon(resIcon("action-upload.svg"));
    actUploadRight_->setToolTip(actUploadRight_->text());
    // Shortcut F8 on right panel to upload via dialog (remote only)
    actUploadRight_->setShortcut(QKeySequence(Qt::Key_F8));
    actUploadRight_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    if (rightView_)
        rightView_->addAction(actUploadRight_);

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
    actNewDirRight_->setIcon(resIcon("action-new-folder.svg"));
    actNewDirRight_->setToolTip(actNewDirRight_->text());
    actRenameRight_->setIcon(resIcon("action-rename.svg"));
    actRenameRight_->setToolTip(actRenameRight_->text());
    actDeleteRight_->setIcon(resIcon("action-delete.svg"));
    actDeleteRight_->setToolTip(actDeleteRight_->text());
    actNewFileRight_->setIcon(resIcon("action-new-file.svg"));
    actNewFileRight_->setToolTip(actNewFileRight_->text());
    // Shortcuts (right panel scope)
    actRenameRight_->setShortcut(QKeySequence(Qt::Key_F2));
    actRenameRight_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    if (rightView_)
        rightView_->addAction(actRenameRight_);
    actNewDirRight_->setShortcut(QKeySequence(Qt::Key_F9));
    actNewDirRight_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    if (rightView_)
        rightView_->addAction(actNewDirRight_);
    actNewFileRight_->setShortcut(QKeySequence(Qt::Key_F10));
    actNewFileRight_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    if (rightView_)
        rightView_->addAction(actNewFileRight_);

    // Order: Copy, Move, Delete, Rename, New folder, then Download/Upload
    rightPaneBar_->addSeparator();
    // Toolbar buttons with generic texts (Copy/Move)
    actCopyRightTb_ = new QAction(tr("Copy"), this);
    connect(actCopyRightTb_, &QAction::triggered, this,
            &MainWindow::copyRightToLeft);
    actMoveRightTb_ = new QAction(tr("Move"), this);
    connect(actMoveRightTb_, &QAction::triggered, this,
            &MainWindow::moveRightToLeft);
    actCopyRightTb_->setIcon(resIcon("action-copy.svg"));
    actCopyRightTb_->setToolTip(actCopyRightTb_->text());
    actMoveRightTb_->setIcon(resIcon("action-move-to-left.svg"));
    actMoveRightTb_->setToolTip(actMoveRightTb_->text());
    // Shortcuts F5/F6 on right panel (scope: right view)
    actCopyRightTb_->setShortcut(QKeySequence(Qt::Key_F5));
    actCopyRightTb_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    if (rightView_)
        rightView_->addAction(actCopyRightTb_);
    actMoveRightTb_->setShortcut(QKeySequence(Qt::Key_F6));
    actMoveRightTb_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    if (rightView_)
        rightView_->addAction(actMoveRightTb_);
    rightPaneBar_->addAction(actCopyRightTb_);
    rightPaneBar_->addAction(actMoveRightTb_);
    rightPaneBar_->addAction(actDeleteRight_);
    rightPaneBar_->addAction(actRenameRight_);
    rightPaneBar_->addAction(actNewFileRight_);
    rightPaneBar_->addAction(actNewDirRight_);
    rightPaneBar_->addSeparator();
    rightPaneBar_->addAction(actDownloadF7_);
    rightPaneBar_->addAction(actUploadRight_);
    // Delete shortcut also on right panel (limited to right panel widget)
    if (actDeleteRight_) {
        actDeleteRight_->setShortcut(QKeySequence(Qt::Key_Delete));
        actDeleteRight_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        if (rightView_)
            rightView_->addAction(actDeleteRight_);
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
            if (!sel || sel->selectedRows(NAME_COL).isEmpty()) {
                statusBar()->showMessage(tr("Select items to download"), 2000);
                return;
            }
            downloadRightToLeft();
        });
    }
    // Disable strictly-remote actions at startup
    if (actDownloadF7_)
        actDownloadF7_->setEnabled(false);
    actUploadRight_->setEnabled(false);
    if (actNewFileRight_)
        actNewFileRight_->setEnabled(false);

    // Right panel: toolbar -> path -> view
    rightLayout->addWidget(rightPaneBar_);
    rightLayout->addWidget(rightPath_);
    rightLayout->addWidget(rightView_);

    // Mount panes into the splitter
    splitter->addWidget(leftPane);
    splitter->addWidget(rightPane);
    setCentralWidget(splitter);

    // Main toolbar (top)
    auto *tb = addToolBar("Main");
    tb->setToolButtonStyle(Qt::ToolButtonIconOnly);
    tb->setMovable(false);
    // Keep the system default size for the main toolbar and make sub‑toolbars
    // slightly smaller
    const int mainIconPx =
        tb->style()->pixelMetric(QStyle::PM_ToolBarIconSize, nullptr, tb);
    const int subIconPx =
        qMax(16, mainIconPx - 4); // sub‑toolbars slightly smaller
    leftPaneBar_->setIconSize(QSize(subIconPx, subIconPx));
    rightPaneBar_->setIconSize(QSize(subIconPx, subIconPx));
    // Copy/move/delete actions now live in the left sub‑toolbar
    actConnect_ =
        tb->addAction(tr("Connect (SFTP)"), this, &MainWindow::connectSftp);
    actConnect_->setIcon(resIcon("action-connect.svg"));
    actConnect_->setToolTip(actConnect_->text());
    tb->addSeparator();
    actDisconnect_ =
        tb->addAction(tr("Disconnect"), this, &MainWindow::disconnectSftp);
    actDisconnect_->setIcon(resIcon("action-disconnect.svg"));
    actDisconnect_->setToolTip(actDisconnect_->text());
    actDisconnect_->setEnabled(false);

    // Show text to the LEFT of the icon for Connect/Disconnect buttons only
    if (QWidget *w = tb->widgetForAction(actConnect_)) {
        if (auto *b = qobject_cast<QToolButton *>(w)) {
            b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            b->setText(tr("Connect"));
        }
    }
    if (QWidget *w = tb->widgetForAction(actDisconnect_)) {
        if (auto *b = qobject_cast<QToolButton *>(w)) {
            b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            b->setText(tr("Disconnect"));
        }
    }
    tb->addSeparator();
    actSites_ = tb->addAction(tr("Saved sites"), [this] {
        SiteManagerDialog dlg(this);
        if (dlg.exec() == QDialog::Accepted) {
            openscp::SessionOptions opt{};
            if (dlg.selectedOptions(opt)) {
                startSftpConnect(opt);
            }
        }
    });
    actSites_->setIcon(resIcon("action-open-saved-sites.svg"));
    actSites_->setToolTip(actSites_->text());
    tb->addSeparator();
    actShowQueue_ =
        tb->addAction(tr("Transfers"), [this] { showTransferQueue(); });
    actShowQueue_->setIcon(resIcon("action-open-transfer-queue.svg"));
    actShowQueue_->setToolTip(actShowQueue_->text());
    // Show text beside icon for Sites and Queue too
    if (QWidget *w = tb->widgetForAction(actSites_)) {
        if (auto *b = qobject_cast<QToolButton *>(w)) {
            b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            b->setText(tr("Saved sites"));
        }
    }
    if (QWidget *w = tb->widgetForAction(actShowQueue_)) {
        if (auto *b = qobject_cast<QToolButton *>(w)) {
            b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            b->setText(tr("Transfers"));
        }
    }
    // Global shortcut to open the transfer queue
    actShowQueue_->setShortcut(QKeySequence(Qt::Key_F12));
    actShowQueue_->setShortcutContext(Qt::ApplicationShortcut);
    this->addAction(actShowQueue_);

    // Global fullscreen toggle (standard platform shortcut)
    // macOS: Ctrl+Cmd+F, Linux: F11
    {
        QAction *actToggleFs = new QAction(tr("Full screen"), this);
        actToggleFs->setShortcut(QKeySequence::FullScreen);
        actToggleFs->setShortcutContext(Qt::ApplicationShortcut);
        connect(actToggleFs, &QAction::triggered, this, [this] {
            const bool fs = (windowState() & Qt::WindowFullScreen);
            if (fs)
                setWindowState(windowState() & ~Qt::WindowFullScreen);
            else
                setWindowState(windowState() | Qt::WindowFullScreen);
        });
        this->addAction(actToggleFs);
    }
    // Separator to the right of the queue button
    tb->addSeparator();
    // Queue is always enabled by default; no toggle

    // Spacer to push next action to the far right
    {
        QWidget *spacer = new QWidget(this);
        spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        tb->addWidget(spacer);
    }
    // Visual separator before the right-side buttons
    tb->addSeparator();
    // About button (to the left of Settings)
    actAboutToolbar_ =
        tb->addAction(resIcon("action-open-about-us.svg"), tr("About OpenSCP"),
                      this, &MainWindow::showAboutDialog);
    if (actAboutToolbar_)
        actAboutToolbar_->setToolTip(actAboutToolbar_->text());
    // Settings button (far right)
    actPrefsToolbar_ =
        tb->addAction(resIcon("action-open-settings.svg"), tr("Settings"), this,
                      &MainWindow::showSettingsDialog);
    actPrefsToolbar_->setToolTip(actPrefsToolbar_->text());

    // Global shortcuts were already added to their respective actions

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
        const QString t = helpMenu->title();
        if (t.compare(QStringLiteral("Help"), Qt::CaseInsensitive) == 0) {
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

    {
        QSettings s("OpenSCP", "OpenSCP");
        downloadDir_ = defaultDownloadDirFromSettings(s);
    }
    QDir().mkpath(downloadDir_);

    statusBar()->showMessage(tr("Ready"));
    setWindowTitle(tr("OpenSCP — local/local (click Connect for remote)"));
    resize(1100, 650);

    // Transfer queue
    transferMgr_ = new TransferManager(this);
    // Provide transfer manager to views (for async remote drag-out staging)
    if (auto *lv = qobject_cast<DragAwareTreeView *>(leftView_))
        lv->setTransferManager(transferMgr_);
    if (auto *rv = qobject_cast<DragAwareTreeView *>(rightView_))
        rv->setTransferManager(transferMgr_);

    // Startup cleanup (deferred): remove old staging batches if
    // autoCleanStaging is enabled
    QTimer::singleShot(0, this, [this] {
        QSettings s("OpenSCP", "OpenSCP");
        const bool autoClean =
            s.value("Advanced/autoCleanStaging", true).toBool();
        if (!autoClean)
            return;
        QString root = s.value("Advanced/stagingRoot").toString();
        if (root.isEmpty())
            root = QDir::homePath() + "/Downloads/OpenSCP-Dragged";
        QDir r(root);
        if (!r.exists())
            return;
        const QDateTime now = QDateTime::currentDateTimeUtc();
        const qint64 maxAgeMs = qint64(7) * 24 * 60 * 60 * 1000; // 7 days
        // Match timestamp batches: yyyyMMdd-HHmmss
        QRegularExpression re("^\\d{8}-\\d{6}$");
        const auto entries = r.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot |
                                             QDir::NoSymLinks);
        for (const QFileInfo &fi : entries) {
            if (fi.isSymLink())
                continue; // do not follow symlinks
            if (!re.match(fi.fileName()).hasMatch())
                continue; // only batches
            const QDateTime m = fi.lastModified().toUTC();
            if (m.isValid() && m.msecsTo(now) > maxAgeMs) {
                QDir(fi.absoluteFilePath()).removeRecursively();
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
               "OPEN_SCP_ENABLE_INSECURE_FALLBACK to hide this warning."));
        statusBar()->addPermanentWidget(warn, 0);
    }

    // Apply user preferences (hidden files, click mode, etc.)
    applyPreferences();
    updateDeleteShortcutEnables();

    // Startup preferences and migration
    {
        QSettings s("OpenSCP", "OpenSCP");
        // One-shot migration: if only showConnOnStart exists, copy to
        // openSiteManagerOnDisconnect
        if (!s.contains("UI/openSiteManagerOnDisconnect") &&
            s.contains("UI/showConnOnStart")) {
            const bool v = s.value("UI/showConnOnStart", true).toBool();
            s.setValue("UI/openSiteManagerOnDisconnect", v);
            s.sync();
        }
        m_openSiteManagerOnStartup =
            s.value("UI/showConnOnStart", true).toBool();
        m_openSiteManagerOnDisconnect =
            s.value("UI/openSiteManagerOnDisconnect", true).toBool();
        if (m_openSiteManagerOnStartup && !QCoreApplication::closingDown() &&
            !sftp_) {
            QTimer::singleShot(0, this, [this] { showSiteManagerNonModal(); });
        }
    }
}

// Show the application About dialog.
void MainWindow::showAboutDialog() {
    AboutDialog dlg(this);
    dlg.exec();
}

// Open the Settings dialog and apply changes when accepted.
void MainWindow::showSettingsDialog() {
    SettingsDialog dlg(this);
    dlg.exec();
    // Reflect any applied changes in the running UI
    applyPreferences();
}

void MainWindow::showEvent(QShowEvent *e) {
    QMainWindow::showEvent(e);
    if (firstShow_) {
        firstShow_ = false;
        QRect avail;
        if (this->screen())
            avail = this->screen()->availableGeometry();
        else if (auto ps = QGuiApplication::primaryScreen())
            avail = ps->availableGeometry();
        if (avail.isValid()) {
            const QSize sz = size();
            const int x = avail.center().x() - sz.width() / 2;
            const int y = avail.center().y() - sz.height() / 2;
            move(x, y);
        }
    }
}

void MainWindow::applyPreferences() {
    QSettings s("OpenSCP", "OpenSCP");
    const bool showHidden = s.value("UI/showHidden", false).toBool();
    const bool singleClick = s.value("UI/singleClick", false).toBool();
    QString openBehaviorMode =
        s.value("UI/openBehaviorMode").toString().trimmed().toLower();
    if (openBehaviorMode.isEmpty()) {
        const bool revealLegacy =
            s.value("UI/openRevealInFolder", false).toBool();
        openBehaviorMode =
            revealLegacy ? QStringLiteral("reveal") : QStringLiteral("ask");
    }
    if (openBehaviorMode != QStringLiteral("ask") &&
        openBehaviorMode != QStringLiteral("reveal") &&
        openBehaviorMode != QStringLiteral("open")) {
        openBehaviorMode = QStringLiteral("ask");
    }
    prefOpenBehaviorMode_ = openBehaviorMode;
    prefShowQueueOnEnqueue_ = s.value("UI/showQueueOnEnqueue", true).toBool();
    prefNoHostVerificationTtlMin_ = qBound(
        1, s.value("Security/noHostVerificationTtlMin", 15).toInt(), 120);
    downloadDir_ = defaultDownloadDirFromSettings(s);
    QDir().mkpath(downloadDir_);
    // Keep Site Manager auto-open preference up to date
    m_openSiteManagerOnDisconnect =
        s.value("UI/openSiteManagerOnDisconnect", true).toBool();
    applyTransferPreferences();

    // Local: model filters (hidden on/off)
    auto applyLocalFilters = [&](QFileSystemModel *m) {
        if (!m)
            return;
        QDir::Filters f =
            QDir::AllEntries | QDir::NoDotAndDotDot | QDir::AllDirs;
        if (showHidden)
            f = f | QDir::Hidden | QDir::System;
        m->setFilter(f);
    };
    applyLocalFilters(leftModel_);
    applyLocalFilters(rightLocalModel_);

    // Remote: re-list with hidden filter
    prefShowHidden_ = showHidden;
    if (rightRemoteModel_) {
        rightRemoteModel_->setShowHidden(showHidden);
        QString dummy;
        rightRemoteModel_->setRootPath(rightRemoteModel_->rootPath(), &dummy);
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
    QSettings s("OpenSCP", "OpenSCP");
    const int maxConcurrent =
        qBound(1, s.value("Transfer/maxConcurrent", 2).toInt(), 8);
    const int globalSpeed =
        qMax(0, s.value("Transfer/globalSpeedKBps", 0).toInt());
    transferMgr_->setMaxConcurrent(maxConcurrent);
    transferMgr_->setGlobalSpeedLimitKBps(globalSpeed);
    if (transferDlg_)
        QMetaObject::invokeMethod(transferDlg_, "refresh",
                                  Qt::QueuedConnection);
}

void MainWindow::updateDeleteShortcutEnables() {
    auto hasColSel = [&](QTreeView *v) -> bool {
        if (!v || !v->selectionModel())
            return false;
        return !v->selectionModel()->selectedRows(NAME_COL).isEmpty();
    };
    const bool leftHasSel = hasColSel(leftView_);
    const bool rightHasSel = hasColSel(rightView_);
    const bool rightWrite =
        (!rightIsRemote_) || (rightIsRemote_ && rightRemoteWritable_);

    // Left: enable according to selection (exception: Up)
    if (actCopyF5_)
        actCopyF5_->setEnabled(leftHasSel);
    if (actMoveF6_)
        actMoveF6_->setEnabled(leftHasSel);
    if (actDelete_)
        actDelete_->setEnabled(leftHasSel);
    if (actRenameLeft_)
        actRenameLeft_->setEnabled(leftHasSel);
    if (actNewDirLeft_)
        actNewDirLeft_->setEnabled(true); // always enabled on local
    if (actNewFileLeft_)
        actNewFileLeft_->setEnabled(true); // always enabled on local
    if (actUpLeft_) {
        QDir d(leftPath_ ? leftPath_->text() : QString());
        bool canUp = d.cdUp();
        actUpLeft_->setEnabled(canUp);
    }

    // Right: enable according to selection + permissions (exceptions: Up,
    // Upload, Download)
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
        actUploadRight_->setEnabled(rightIsRemote_ &&
                                    rightRemoteWritable_); // exception
    if (actDownloadF7_)
        actDownloadF7_->setEnabled(
            rightIsRemote_); // exception: enabled without selection
    if (actUpRight_) {
        QString cur = rightRemoteModel_ ? rightRemoteModel_->rootPath()
                                        : rightPath_->text();
        if (rightIsRemote_) {
            if (cur.endsWith('/'))
                cur.chop(1);
            actUpRight_->setEnabled(!cur.isEmpty() && cur != "/");
        } else {
            QDir d(cur);
            actUpRight_->setEnabled(d.cdUp());
        }
    }
}
