// OpenSCP main window: dual‑pane file manager with SFTP support.
// Provides local operations (copy/move/delete) and remote ones (browse, upload, download,
// create/rename/delete), a transfer queue with resume, and known_hosts validation.
#include "MainWindow.hpp"
#include "openscp/Libssh2SftpClient.hpp"
#include "ConnectionDialog.hpp"
#include "RemoteModel.hpp"
#include <QApplication>
#include <QVBoxLayout>
#include <QSplitter>
#include <QToolBar>
#include <QSize>
#include <QFileDialog>
#include <QStatusBar>
#include <QLabel>
#include <QHeaderView>
#include <QMessageBox>
#include <QFileInfo>
#include <QDir>
#include <QFile>
#include <QKeySequence>
#include <QDirIterator>
#include <QInputDialog>
#include <QAbstractButton>
#include <QDesktopServices>
#include <QStandardPaths>
#include <QUrl>
#include <QProgressDialog>
#include <QLocale>
#include <QPushButton>
#include <QListView>
#include <QMenu>
#include <QMenuBar>
#include <QDateTime>
#include <QCheckBox>
#include "PermissionsDialog.hpp"
#include "SiteManagerDialog.hpp"
#include <QMimeData>
#include <QDropEvent>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include "TransferManager.hpp"
#include "TransferQueueDialog.hpp"
#include "SecretStore.hpp"
#include "AboutDialog.hpp"
#include "SettingsDialog.hpp"
#include <QEventLoop>
#include <QCoreApplication>
#include <QShowEvent>
#include <QDialog>
#include <QScreen>
#include <QGuiApplication>
#include <QStyle>
#include <QIcon>
#include <QTemporaryFile>
#include <QSettings>
#include <QRegularExpression>
#include <QTimer>
#include <QSet>
#include <QHash>
#include <QShortcut>
#include <QToolButton>
#include <QProcess>
#include <cstring>
#include "DragAwareTreeView.hpp"
#include <atomic>
#include <thread>
#include <chrono>
#include <memory>

static constexpr int NAME_COL = 0;

// Best-effort memory scrubbing helpers for sensitive data
static inline void secureClear(QString& s) {
    for (int i = 0, n = s.size(); i < n; ++i) s[i] = QChar(u'\0');
    s.clear();
}
static inline void secureClear(QByteArray& b) {
    if (b.isEmpty()) return;
    volatile char* p = reinterpret_cast<volatile char*>(b.data());
    for (int i = 0; i < b.size(); ++i) p[i] = 0;
    b.clear();
    b.squeeze();
}

MainWindow::~MainWindow() = default; // define the destructor here

// Recursively copy a file or directory.
// Returns true on success; otherwise false and writes an error message.
static bool copyEntryRecursively(const QString& srcPath, const QString& dstPath, QString& error) {
    QFileInfo srcInfo(srcPath);

    if (srcInfo.isFile()) {
        // Ensure destination directory
        QDir().mkpath(QFileInfo(dstPath).dir().absolutePath());
        if (QFile::exists(dstPath)) QFile::remove(dstPath);
        if (!QFile::copy(srcPath, dstPath)) {
            error = QString(QCoreApplication::translate("MainWindow", "No se pudo copiar archivo: %1")).arg(srcPath);
            return false;
        }
        return true;
    }

    if (srcInfo.isDir()) {
        // Create destination directory
        if (!QDir().mkpath(dstPath)) {
            error = QString(QCoreApplication::translate("MainWindow", "No se pudo crear carpeta destino: %1")).arg(dstPath);
            return false;
        }
        // Iterate recursively
        QDirIterator it(srcPath, QDir::NoDotAndDotDot | QDir::AllEntries, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            const QFileInfo fi = it.fileInfo();
            const QString rel = QDir(srcPath).relativeFilePath(fi.absoluteFilePath());
            const QString target = QDir(dstPath).filePath(rel);

            if (fi.isDir()) {
                if (!QDir().mkpath(target)) {
                    error = QString(QCoreApplication::translate("MainWindow", "No se pudo crear subcarpeta destino: %1")).arg(target);
                    return false;
                }
            } else {
                // Ensure parent directory exists
                QDir().mkpath(QFileInfo(target).dir().absolutePath());
                if (QFile::exists(target)) QFile::remove(target);
                if (!QFile::copy(fi.absoluteFilePath(), target)) {
                    error = QString(QCoreApplication::translate("MainWindow", "Falló al copiar: %1")).arg(fi.absoluteFilePath());
                    return false;
                }
            }
        }
        return true;
    }

    error = QCoreApplication::translate("MainWindow", "Entrada de origen ni archivo ni carpeta.");
    return false;
}

// Compute a temporary local path to preview/open ad‑hoc downloads.
static QString tempDownloadPathFor(const QString& remoteName) {
    QString base = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (base.isEmpty()) base = QDir::homePath() + "/Downloads";
    QDir().mkpath(base);
    return QDir(base).filePath(remoteName);
}

// Reveal a file in the system file manager (select/highlight when possible),
// falling back to opening the containing folder.
static void revealInFolder(const QString& filePath) {
#if defined(Q_OS_MAC)
    // macOS: use 'open -R' to reveal in Finder
    QProcess::startDetached("open", { "-R", filePath });
#elif defined(Q_OS_WIN)
    // Windows: explorer /select,<path>
    QString arg = "/select," + QDir::toNativeSeparators(filePath);
    QProcess::startDetached("explorer", { arg });
#else
    // Linux/others: try to open the containing directory
    const QString dir = QFileInfo(filePath).dir().absolutePath();
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
#endif
}

// Validate simple file/folder names (no paths)
static bool isValidEntryName(const QString& name, QString* why = nullptr) {
    if (name == "." || name == "..") {
        if (why) *why = QCoreApplication::translate("MainWindow", "Nombre inválido: no puede ser '.' ni '..'.");
        return false;
    }
    if (name.contains('/') || name.contains('\\')) {
        if (why) *why = QCoreApplication::translate("MainWindow", "Nombre inválido: no puede contener separadores ('/' o '\\').");
        return false;
    }
    for (const QChar& ch : name) {
        ushort u = ch.unicode();
        if (u < 0x20u || u == 0x7Fu) { // ASCII control characters
            if (why) *why = QCoreApplication::translate("MainWindow", "Nombre inválido: no puede tener caracteres de control.");
            return false;
        }
    }
    return true;
}

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    // Globally center dialogs relative to the main window
    qApp->installEventFilter(this);
    // Models
    leftModel_       = new QFileSystemModel(this);
    rightLocalModel_ = new QFileSystemModel(this);

    leftModel_->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::AllDirs);
    rightLocalModel_->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::AllDirs);

    // Initial paths: HOME
    const QString home = QDir::homePath();
    leftModel_->setRootPath(home);
    rightLocalModel_->setRootPath(home);

    // Views
    leftView_  = new DragAwareTreeView(this);
    rightView_ = new DragAwareTreeView(this);

    leftView_->setModel(leftModel_);
    rightView_->setModel(rightLocalModel_);
    // Avoid expanding subtrees on double‑click; navigate by changing root
    leftView_->setExpandsOnDoubleClick(false);
    rightView_->setExpandsOnDoubleClick(false);
    leftView_->setRootIndex(leftModel_->index(home));
    rightView_->setRootIndex(rightLocalModel_->index(home));

    // Basic view tuning
    auto tuneView = [](QTreeView* v) {
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
    leftPath_  = new QLineEdit(home, this);
    rightPath_ = new QLineEdit(home, this);
    connect(leftPath_,  &QLineEdit::returnPressed, this, &MainWindow::leftPathEntered);
    connect(rightPath_, &QLineEdit::returnPressed, this, &MainWindow::rightPathEntered);

    // Central splitter with two panes
    auto* splitter = new QSplitter(this);
    auto* leftPane  = new QWidget(this);
    auto* rightPane = new QWidget(this);

    auto* leftLayout  = new QVBoxLayout(leftPane);
    auto* rightLayout = new QVBoxLayout(rightPane);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setContentsMargins(0, 0, 0, 0);

    // Left pane sub‑toolbar
    leftPaneBar_ = new QToolBar("LeftBar", leftPane);
    leftPaneBar_->setIconSize(QSize(18, 18));
    leftPaneBar_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    // Helper for icons from local resources
    auto resIcon = [](const char* fname) -> QIcon {
        return QIcon(QStringLiteral(":/assets/icons/") + QLatin1String(fname));
    };
    // Left sub‑toolbar: Up, Copy, Move, Delete, Rename, New folder
    actUpLeft_ = leftPaneBar_->addAction(tr("Arriba"), this, &MainWindow::goUpLeft);
    actUpLeft_->setIcon(resIcon("action-go-up.svg"));
    actUpLeft_->setToolTip(actUpLeft_->text());
    // Button "Open left folder" next to Up
    actChooseLeft_ = leftPaneBar_->addAction(tr("Abrir carpeta izquierda"), this, &MainWindow::chooseLeftDir);
    actChooseLeft_->setIcon(resIcon("action-open-folder.svg"));
    actChooseLeft_->setToolTip(actChooseLeft_->text());
    leftPaneBar_->addSeparator();
    actCopyF5_ = leftPaneBar_->addAction(tr("Copiar"), this, &MainWindow::copyLeftToRight);
    actCopyF5_->setIcon(resIcon("action-copy.svg"));
    actCopyF5_->setToolTip(actCopyF5_->text());
    // Shortcut F5 on left panel (scope: left view and its children)
    actCopyF5_->setShortcut(QKeySequence(Qt::Key_F5));
    actCopyF5_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    if (leftView_) leftView_->addAction(actCopyF5_);
    actMoveF6_ = leftPaneBar_->addAction(tr("Mover"), this, &MainWindow::moveLeftToRight);
    actMoveF6_->setIcon(resIcon("action-move-to-right.svg"));
    actMoveF6_->setToolTip(actMoveF6_->text());
    // Shortcut F6 on left panel
    actMoveF6_->setShortcut(QKeySequence(Qt::Key_F6));
    actMoveF6_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    if (leftView_) leftView_->addAction(actMoveF6_);
    actDelete_ = leftPaneBar_->addAction(tr("Borrar"), this, &MainWindow::deleteFromLeft);
    actDelete_->setIcon(resIcon("action-delete.svg"));
    actDelete_->setToolTip(actDelete_->text());
    actDelete_->setShortcut(QKeySequence(Qt::Key_Delete));
    actDelete_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    if (leftView_) leftView_->addAction(actDelete_);
    // Action: copy from right panel to left (remote/local -> left)
    actCopyRight_ = new QAction(tr("Copiar al panel izquierdo"), this);
    connect(actCopyRight_, &QAction::triggered, this, &MainWindow::copyRightToLeft);
    actCopyRight_->setIcon(QIcon(QLatin1String(":/assets/icons/action-copy.svg")));
    // Action: move from right panel to left
    actMoveRight_ = new QAction(tr("Mover al panel izquierdo"), this);
    connect(actMoveRight_, &QAction::triggered, this, &MainWindow::moveRightToLeft);
    actMoveRight_->setIcon(QIcon(QLatin1String(":/assets/icons/action-move-to-left.svg")));
    // Additional local actions (also in toolbar)
    actNewDirLeft_  = new QAction(tr("Nueva carpeta"), this);
    connect(actNewDirLeft_, &QAction::triggered, this, &MainWindow::newDirLeft);
    actRenameLeft_  = new QAction(tr("Renombrar"), this);
    connect(actRenameLeft_, &QAction::triggered, this, &MainWindow::renameLeftSelected);
    actNewFileLeft_ = new QAction(tr("Nuevo archivo"), this);
    connect(actNewFileLeft_, &QAction::triggered, this, &MainWindow::newFileLeft);
    actRenameLeft_->setIcon(resIcon("action-rename.svg"));
    actRenameLeft_->setToolTip(actRenameLeft_->text());
    actNewDirLeft_->setIcon(resIcon("action-new-folder.svg"));
    actNewDirLeft_->setToolTip(actNewDirLeft_->text());
    actNewFileLeft_->setIcon(resIcon("action-new-file.svg"));
    actNewFileLeft_->setToolTip(actNewFileLeft_->text());
    // Shortcuts (left panel scope)
    actRenameLeft_->setShortcut(QKeySequence(Qt::Key_F2));
    actRenameLeft_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    if (leftView_) leftView_->addAction(actRenameLeft_);
    actNewDirLeft_->setShortcut(QKeySequence(Qt::Key_F9));
    actNewDirLeft_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    if (leftView_) leftView_->addAction(actNewDirLeft_);
    actNewFileLeft_->setShortcut(QKeySequence(Qt::Key_F10));
    actNewFileLeft_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    if (leftView_) leftView_->addAction(actNewFileLeft_);
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
    actUpRight_ = rightPaneBar_->addAction(tr("Arriba"), this, &MainWindow::goUpRight);
    actUpRight_->setIcon(resIcon("action-go-up.svg"));
    actUpRight_->setToolTip(actUpRight_->text());
    // Button "Open right folder" next to Up
    actChooseRight_  = rightPaneBar_->addAction(tr("Abrir carpeta derecha"),    this, &MainWindow::chooseRightDir);
    actChooseRight_->setIcon(resIcon("action-open-folder.svg"));
    actChooseRight_->setToolTip(actChooseRight_->text());

    // Right panel actions (create first, then add in requested order)
    actDownloadF7_ = new QAction(tr("Descargar"), this);
    connect(actDownloadF7_, &QAction::triggered, this, &MainWindow::downloadRightToLeft);
    actDownloadF7_->setEnabled(false);   // starts disabled on local
    actDownloadF7_->setIcon(resIcon("action-download.svg"));
    actDownloadF7_->setToolTip(actDownloadF7_->text());

    actUploadRight_ = new QAction(tr("Subir…"), this);
    connect(actUploadRight_, &QAction::triggered, this, &MainWindow::uploadViaDialog);
    actUploadRight_->setIcon(resIcon("action-upload.svg"));
    actUploadRight_->setToolTip(actUploadRight_->text());
    // Shortcut F8 on right panel to upload via dialog (remote only)
    actUploadRight_->setShortcut(QKeySequence(Qt::Key_F8));
    actUploadRight_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    if (rightView_) rightView_->addAction(actUploadRight_);

    actNewDirRight_  = new QAction(tr("Nueva carpeta"), this);
    connect(actNewDirRight_,  &QAction::triggered, this, &MainWindow::newDirRight);
    actRenameRight_  = new QAction(tr("Renombrar"), this);
    connect(actRenameRight_,  &QAction::triggered, this, &MainWindow::renameRightSelected);
    actDeleteRight_  = new QAction(tr("Borrar"), this);
    connect(actDeleteRight_,  &QAction::triggered, this, &MainWindow::deleteRightSelected);
    actNewFileRight_ = new QAction(tr("Nuevo archivo"), this);
    connect(actNewFileRight_, &QAction::triggered, this, &MainWindow::newFileRight);
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
    if (rightView_) rightView_->addAction(actRenameRight_);
    actNewDirRight_->setShortcut(QKeySequence(Qt::Key_F9));
    actNewDirRight_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    if (rightView_) rightView_->addAction(actNewDirRight_);
    actNewFileRight_->setShortcut(QKeySequence(Qt::Key_F10));
    actNewFileRight_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    if (rightView_) rightView_->addAction(actNewFileRight_);

    // Order: Copy, Move, Delete, Rename, New folder, then Download/Upload
    rightPaneBar_->addSeparator();
    // Toolbar buttons with generic texts (Copy/Move)
    actCopyRightTb_ = new QAction(tr("Copiar"), this);
    connect(actCopyRightTb_, &QAction::triggered, this, &MainWindow::copyRightToLeft);
    actMoveRightTb_ = new QAction(tr("Mover"), this);
    connect(actMoveRightTb_, &QAction::triggered, this, &MainWindow::moveRightToLeft);
    actCopyRightTb_->setIcon(resIcon("action-copy.svg"));
    actCopyRightTb_->setToolTip(actCopyRightTb_->text());
    actMoveRightTb_->setIcon(resIcon("action-move-to-left.svg"));
    actMoveRightTb_->setToolTip(actMoveRightTb_->text());
    // Shortcuts F5/F6 on right panel (scope: right view)
    actCopyRightTb_->setShortcut(QKeySequence(Qt::Key_F5));
    actCopyRightTb_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    if (rightView_) rightView_->addAction(actCopyRightTb_);
    actMoveRightTb_->setShortcut(QKeySequence(Qt::Key_F6));
    actMoveRightTb_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    if (rightView_) rightView_->addAction(actMoveRightTb_);
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
        if (rightView_) rightView_->addAction(actDeleteRight_);
    }
    // Keyboard shortcut F7 on right panel: only acts when remote and with selection
    if (rightView_) {
        auto* scF7 = new QShortcut(QKeySequence(Qt::Key_F7), rightView_);
        scF7->setContext(Qt::WidgetWithChildrenShortcut);
        connect(scF7, &QShortcut::activated, this, [this] {
            if (!rightIsRemote_) return; // only when remote
            auto sel = rightView_->selectionModel();
            if (!sel || sel->selectedRows(NAME_COL).isEmpty()) {
                statusBar()->showMessage(tr("Selecciona elementos para descargar"), 2000);
                return;
            }
            downloadRightToLeft();
        });
    }
    // Disable strictly-remote actions at startup
    if (actDownloadF7_) actDownloadF7_->setEnabled(false);
    actUploadRight_->setEnabled(false);
    if (actNewFileRight_) actNewFileRight_->setEnabled(false);

    // Right panel: toolbar -> path -> view
    rightLayout->addWidget(rightPaneBar_);
    rightLayout->addWidget(rightPath_);
    rightLayout->addWidget(rightView_);

    // Mount panes into the splitter
    splitter->addWidget(leftPane);
    splitter->addWidget(rightPane);
    setCentralWidget(splitter);

    // Main toolbar (top)
    auto* tb = addToolBar("Main");
    tb->setToolButtonStyle(Qt::ToolButtonIconOnly);
    tb->setMovable(false);
    // Keep the system default size for the main toolbar and make sub‑toolbars slightly smaller
    const int mainIconPx = tb->style()->pixelMetric(QStyle::PM_ToolBarIconSize, nullptr, tb);
    const int subIconPx  = qMax(16, mainIconPx - 4); // sub‑toolbars slightly smaller
    leftPaneBar_->setIconSize(QSize(subIconPx, subIconPx));
    rightPaneBar_->setIconSize(QSize(subIconPx, subIconPx));
    // Copy/move/delete actions now live in the left sub‑toolbar
    actConnect_    = tb->addAction(tr("Conectar (SFTP)"), this, &MainWindow::connectSftp);
    actConnect_->setIcon(resIcon("action-connect.svg"));
    actConnect_->setToolTip(actConnect_->text());
    tb->addSeparator();
    actDisconnect_ = tb->addAction(tr("Desconectar"),     this, &MainWindow::disconnectSftp);
    actDisconnect_->setIcon(resIcon("action-disconnect.svg"));
    actDisconnect_->setToolTip(actDisconnect_->text());
    actDisconnect_->setEnabled(false);

    // Show text to the LEFT of the icon for Connect/Disconnect buttons only
    if (QWidget* w = tb->widgetForAction(actConnect_)) {
        if (auto* b = qobject_cast<QToolButton*>(w)) {
            b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            b->setText(tr("Conectar"));
        }
    }
    if (QWidget* w = tb->widgetForAction(actDisconnect_)) {
        if (auto* b = qobject_cast<QToolButton*>(w)) {
            b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            b->setText(tr("Desconectar"));
        }
    }
    tb->addSeparator();
    actSites_ = tb->addAction(tr("Sitios guardados"), [this] {
        SiteManagerDialog dlg(this);
        if (dlg.exec() == QDialog::Accepted) {
            openscp::SessionOptions opt{};
            if (dlg.selectedOptions(opt)) {
                std::string err;
                if (!establishSftpAsync(opt, err)) { QMessageBox::critical(this, tr("Error de conexión"), QString::fromStdString(err)); return; }
                applyRemoteConnectedUI(opt);
            }
        }
    });
    actSites_->setIcon(resIcon("action-open-saved-sites.svg"));
    actSites_->setToolTip(actSites_->text());
    tb->addSeparator();
    actShowQueue_ = tb->addAction(tr("Transferencias"), [this] {
        if (!transferDlg_) transferDlg_ = new TransferQueueDialog(transferMgr_, this);
        transferDlg_->show(); transferDlg_->raise(); transferDlg_->activateWindow();
    });
    actShowQueue_->setIcon(resIcon("action-open-transfer-queue.svg"));
    actShowQueue_->setToolTip(actShowQueue_->text());
    // Show text beside icon for Sites and Queue too
    if (QWidget* w = tb->widgetForAction(actSites_)) {
        if (auto* b = qobject_cast<QToolButton*>(w)) {
            b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            b->setText(tr("Sitios guardados"));
        }
    }
    if (QWidget* w = tb->widgetForAction(actShowQueue_)) {
        if (auto* b = qobject_cast<QToolButton*>(w)) {
            b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            b->setText(tr("Transferencias"));
        }
    }
    // Global shortcut to open the transfer queue
    actShowQueue_->setShortcut(QKeySequence(Qt::Key_F12));
    actShowQueue_->setShortcutContext(Qt::ApplicationShortcut);
    this->addAction(actShowQueue_);

    // Global fullscreen toggle (standard platform shortcut)
    // macOS: Ctrl+Cmd+F, Linux: F11
    {
        QAction* actToggleFs = new QAction(tr("Pantalla completa"), this);
        actToggleFs->setShortcut(QKeySequence::FullScreen);
        actToggleFs->setShortcutContext(Qt::ApplicationShortcut);
        connect(actToggleFs, &QAction::triggered, this, [this] {
            const bool fs = (windowState() & Qt::WindowFullScreen);
            if (fs) setWindowState(windowState() & ~Qt::WindowFullScreen);
            else    setWindowState(windowState() |  Qt::WindowFullScreen);
        });
        this->addAction(actToggleFs);
    }
    // Separator to the right of the queue button
    tb->addSeparator();
    // Queue is always enabled by default; no toggle

    // Spacer to push next action to the far right
    {
        QWidget* spacer = new QWidget(this);
        spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        tb->addWidget(spacer);
    }
    // Visual separator before the right-side buttons
    tb->addSeparator();
    // About button (to the left of Settings)
    actAboutToolbar_ = tb->addAction(resIcon("action-open-about-us.svg"), tr("Acerca de OpenSCP"), this, &MainWindow::showAboutDialog);
    if (actAboutToolbar_) actAboutToolbar_->setToolTip(actAboutToolbar_->text());
    // Settings button (far right)
    actPrefsToolbar_ = tb->addAction(resIcon("action-open-settings.svg"), tr("Ajustes"), this, &MainWindow::showSettingsDialog);
    actPrefsToolbar_->setToolTip(actPrefsToolbar_->text());

    // Global shortcuts were already added to their respective actions

    // Menu bar (native on macOS)
    // Duplicate actions so users who prefer the classic menu can use it.
    appMenu_  = menuBar()->addMenu(tr("OpenSCP"));
    actAbout_ = appMenu_->addAction(tr("Acerca de OpenSCP"), this, &MainWindow::showAboutDialog);
    actAbout_->setMenuRole(QAction::AboutRole);
    actPrefs_ = appMenu_->addAction(tr("Ajustes…"), this, &MainWindow::showSettingsDialog);
    actPrefs_->setMenuRole(QAction::PreferencesRole);
    // Standard cross‑platform shortcut (Cmd+, on macOS; Ctrl+, on Linux/Windows)
    actPrefs_->setShortcut(QKeySequence::Preferences);
    appMenu_->addSeparator();
    actQuit_  = appMenu_->addAction(tr("Salir"), qApp, &QApplication::quit);
    actQuit_->setMenuRole(QAction::QuitRole);
    // Standard quit shortcut (Cmd+Q / Ctrl+Q)
    actQuit_->setShortcut(QKeySequence::Quit);

    fileMenu_ = menuBar()->addMenu(tr("Archivo"));
    fileMenu_->addAction(actChooseLeft_);
    fileMenu_->addAction(actChooseRight_);
    fileMenu_->addSeparator();
    fileMenu_->addAction(actConnect_);
    fileMenu_->addAction(actDisconnect_);
    fileMenu_->addAction(actSites_);
    fileMenu_->addAction(actShowQueue_);
    // On non‑macOS platforms, also show Preferences and Quit under "File"
    // to provide a familiar UX on Linux/Windows while keeping the "OpenSCP" app menu.
#ifndef Q_OS_MAC
    fileMenu_->addSeparator();
    fileMenu_->addAction(actPrefs_);
    fileMenu_->addAction(actQuit_);
#endif

    // Help (avoid native help menu to skip the search box)
    auto* helpMenu = menuBar()->addMenu(tr("Ayuda"));
    // On macOS, a menu titled exactly "Help" triggers the native search bar.
    // Keep visible label "Help" but avoid detection by inserting a zero‑width space.
#ifdef Q_OS_MAC
    {
        const QString t = helpMenu->title();
        if (t.compare(QStringLiteral("Help"), Qt::CaseInsensitive) == 0) {
            helpMenu->setTitle(QStringLiteral("Hel") + QChar(0x200B) + QStringLiteral("p"));
        }
    }
#endif
    helpMenu->menuAction()->setMenuRole(QAction::NoRole);
    // Prevent macOS from moving actions to the app menu: force NoRole
    {
        QAction* helpAboutAct = new QAction(tr("Acerca de OpenSCP"), this);
        helpAboutAct->setMenuRole(QAction::NoRole);
        connect(helpAboutAct, &QAction::triggered, this, &MainWindow::showAboutDialog);
        helpMenu->addAction(helpAboutAct);
    }
    {
        QAction* reportAct = new QAction(tr("Informar un error"), this);
        reportAct->setMenuRole(QAction::NoRole);
        connect(reportAct, &QAction::triggered, this, []{
            QDesktopServices::openUrl(QUrl("https://github.com/luiscuellar31/openscp/issues"));
        });
        helpMenu->addAction(reportAct);
    }

    // Double click/Enter navigation on both panes
    connect(rightView_, &QTreeView::activated, this, &MainWindow::rightItemActivated);
    connect(leftView_,  &QTreeView::activated, this, &MainWindow::leftItemActivated);

    // Context menu on right pane
    rightView_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(rightView_, &QWidget::customContextMenuRequested, this, &MainWindow::showRightContextMenu);
    if (rightView_->selectionModel()) {
        connect(rightView_->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this]{ updateDeleteShortcutEnables(); });
    }

    // Context menu on left pane (local)
    leftView_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(leftView_, &QWidget::customContextMenuRequested, this, &MainWindow::showLeftContextMenu);

    // Enable delete shortcut only when there is a selection on the left pane
    if (leftView_->selectionModel()) {
        connect(leftView_->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this]{ updateDeleteShortcutEnables(); });
    }

    downloadDir_ = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (downloadDir_.isEmpty())
        downloadDir_ = QDir::homePath() + "/Downloads";
    QDir().mkpath(downloadDir_);

    statusBar()->showMessage(tr("Listo"));
    setWindowTitle(tr("OpenSCP — local/local (clic en Conectar para remoto)"));
    resize(1100, 650);

    // Transfer queue
    transferMgr_ = new TransferManager(this);
    // Provide transfer manager to views (for async remote drag-out staging)
    if (auto* lv = qobject_cast<DragAwareTreeView*>(leftView_))  lv->setTransferManager(transferMgr_);
    if (auto* rv = qobject_cast<DragAwareTreeView*>(rightView_)) rv->setTransferManager(transferMgr_);

    // Startup cleanup (deferred): remove old staging batches if autoCleanStaging is enabled
    QTimer::singleShot(0, this, [this]{
        QSettings s("OpenSCP", "OpenSCP");
        const bool autoClean = s.value("Advanced/autoCleanStaging", true).toBool();
        if (!autoClean) return;
        QString root = s.value("Advanced/stagingRoot").toString();
        if (root.isEmpty()) root = QDir::homePath() + "/Downloads/OpenSCP-Dragged";
        QDir r(root);
        if (!r.exists()) return;
        const QDateTime now = QDateTime::currentDateTimeUtc();
        const qint64 maxAgeMs = qint64(7) * 24 * 60 * 60 * 1000; // 7 days
        // Match timestamp batches: yyyyMMdd-HHmmss
        QRegularExpression re("^\\d{8}-\\d{6}$");
        const auto entries = r.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
        for (const QFileInfo& fi : entries) {
            if (fi.isSymLink()) continue; // do not follow symlinks
            if (!re.match(fi.fileName()).hasMatch()) continue; // only batches
            const QDateTime m = fi.lastModified().toUTC();
            if (m.isValid() && m.msecsTo(now) > maxAgeMs) {
                QDir(fi.absoluteFilePath()).removeRecursively();
            }
        }
    });

    // Warn if insecure storage is active (non‑Apple only when explicitly enabled)
    if (SecretStore::insecureFallbackActive()) {
        auto* warn = new QLabel(tr("Advertencia: almacenamiento de secretos sin cifrar activado (fallback)"), this);
        warn->setStyleSheet("QLabel{ color:#b00020; font-weight:bold; padding:2px 6px; }");
        warn->setToolTip(tr("Estás usando un almacenamiento de credenciales sin cifrar activado por variable de entorno. Desactiva OPEN_SCP_ENABLE_INSECURE_FALLBACK para ocultar este aviso."));
        statusBar()->addPermanentWidget(warn, 0);
    }

    // Apply user preferences (hidden files, click mode, etc.)
    applyPreferences();
    updateDeleteShortcutEnables();

    // Startup preferences and migration
    {
        QSettings s("OpenSCP", "OpenSCP");
        // One-shot migration: if only showConnOnStart exists, copy to openSiteManagerOnDisconnect
        if (!s.contains("UI/openSiteManagerOnDisconnect") && s.contains("UI/showConnOnStart")) {
            const bool v = s.value("UI/showConnOnStart", true).toBool();
            s.setValue("UI/openSiteManagerOnDisconnect", v);
            s.sync();
        }
        m_openSiteManagerOnStartup    = s.value("UI/showConnOnStart", true).toBool();
        m_openSiteManagerOnDisconnect = s.value("UI/openSiteManagerOnDisconnect", true).toBool();
        if (m_openSiteManagerOnStartup && !QCoreApplication::closingDown() && !sftp_) {
            QTimer::singleShot(0, this, [this]{ showSiteManagerNonModal(); });
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

// Browse and set the left pane root directory.
void MainWindow::chooseLeftDir() {
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Selecciona carpeta izquierda"), leftPath_->text());
    if (!dir.isEmpty()) setLeftRoot(dir);
}

// Browse and set the right pane root directory (local mode).
void MainWindow::chooseRightDir() {
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Selecciona carpeta derecha"), rightPath_->text());
    if (!dir.isEmpty()) setRightRoot(dir);
}

// Navigate left pane to the path typed by the user.
void MainWindow::leftPathEntered() {
    setLeftRoot(leftPath_->text());
}

// Navigate right pane (local or remote) to the path typed by the user.
void MainWindow::rightPathEntered() {
    if (rightIsRemote_) setRightRemoteRoot(rightPath_->text());
    else setRightRoot(rightPath_->text());
}

// Set the left pane root, validating the path and updating view/status.
void MainWindow::setLeftRoot(const QString& path) {
    if (QDir(path).exists()) {
        leftPath_->setText(path);
        leftView_->setRootIndex(leftModel_->index(path));
        statusBar()->showMessage(tr("Izquierda: ") + path, 3000);
        updateDeleteShortcutEnables();
    } else {
        QMessageBox::warning(this, tr("Ruta inválida"), tr("La carpeta no existe."));
    }
}

// Set the right (local) pane root and update view/status.
void MainWindow::setRightRoot(const QString& path) {
    if (QDir(path).exists()) {
        rightPath_->setText(path);
        rightView_->setRootIndex(rightLocalModel_->index(path)); // <-- here
        statusBar()->showMessage(tr("Derecha: ") + path, 3000);
        updateDeleteShortcutEnables();
    } else {
        QMessageBox::warning(this, tr("Ruta inválida"), tr("La carpeta no existe."));
    }
}

static QString joinRemotePath(const QString& base, const QString& name) {
    if (base == "/") return "/" + name;
    return base.endsWith('/') ? base + name : base + "/" + name;
}

// Center the window on first show for better UX.
void MainWindow::showEvent(QShowEvent* e) {
    QMainWindow::showEvent(e);
    if (firstShow_) {
        firstShow_ = false;
        QRect avail;
        if (this->screen()) avail = this->screen()->availableGeometry();
        else if (auto ps = QGuiApplication::primaryScreen()) avail = ps->availableGeometry();
        if (avail.isValid()) {
            const QSize sz = size();
            const int x = avail.center().x() - sz.width() / 2;
            const int y = avail.center().y() - sz.height() / 2;
            move(x, y);
        }
    }
}

void MainWindow::copyLeftToRight() {
    if (rightIsRemote_) {
        // ---- REMOTE branch: upload files (PUT) to the current remote directory ----
        if (!sftp_ || !rightRemoteModel_) {
            QMessageBox::warning(this, tr("SFTP"), tr("No hay sesión SFTP activa."));
            return;
        }

        // Selection on the left panel (local source)
        auto sel = leftView_->selectionModel();
        if (!sel) {
            QMessageBox::warning(this, tr("Copiar"), tr("No hay selección disponible."));
            return;
        }
        const auto rows = sel->selectedRows(NAME_COL);
        if (rows.isEmpty()) {
            QMessageBox::information(this, tr("Copiar"), tr("No hay entradas seleccionadas en el panel izquierdo."));
            return;
        }

        // Always enqueue uploads
        const QString remoteBase = rightRemoteModel_->rootPath();
        int enq = 0;
        for (const QModelIndex& idx : rows) {
            const QFileInfo fi = leftModel_->fileInfo(idx);
            if (fi.isDir()) {
                const QString remoteDirBase = joinRemotePath(remoteBase, fi.fileName());
                QDirIterator it(fi.absoluteFilePath(), QDir::NoDotAndDotDot | QDir::AllEntries, QDirIterator::Subdirectories);
                while (it.hasNext()) {
                    it.next();
                    const QFileInfo sfi = it.fileInfo();
                    if (!sfi.isFile()) continue;
                    const QString rel = QDir(fi.absoluteFilePath()).relativeFilePath(sfi.absoluteFilePath());
                    const QString rTarget = joinRemotePath(remoteDirBase, rel);
                    transferMgr_->enqueueUpload(sfi.absoluteFilePath(), rTarget);
                    ++enq;
                }
            } else {
                const QString rTarget = joinRemotePath(remoteBase, fi.fileName());
                transferMgr_->enqueueUpload(fi.absoluteFilePath(), rTarget);
                ++enq;
            }
        }
        if (enq > 0) {
            statusBar()->showMessage(QString(tr("Encolados: %1 subidas")).arg(enq), 4000);
            if (!transferDlg_) transferDlg_ = new TransferQueueDialog(transferMgr_, this);
            transferDlg_->show(); transferDlg_->raise(); transferDlg_->activateWindow();
        }
        return;
    }

    // ---- LOCAL→LOCAL branch: existing logic as-is ----
    const QString dstDirPath = rightPath_->text();
    QDir dstDir(dstDirPath);
    if (!dstDir.exists()) {
        QMessageBox::warning(this, tr("Destino inválido"), tr("La carpeta de destino no existe."));
        return;
    }

    auto sel = leftView_->selectionModel();
    if (!sel) {
        QMessageBox::warning(this, tr("Copiar"), tr("No hay selección disponible."));
        return;
    }
    const auto rows = sel->selectedRows(NAME_COL);
    if (rows.isEmpty()) {
        QMessageBox::information(this, tr("Copiar"), tr("No hay entradas seleccionadas en el panel izquierdo."));
        return;
    }

    enum class OverwritePolicy { Ask, OverwriteAll, SkipAll };
    OverwritePolicy policy = OverwritePolicy::Ask;

    int ok = 0, fail = 0, skipped = 0;
    QString lastError;

    for (const QModelIndex& idx : rows) {
        const QFileInfo fi = leftModel_->fileInfo(idx);
        const QString target = dstDir.filePath(fi.fileName());

        if (QFileInfo::exists(target)) {
            if (policy == OverwritePolicy::Ask) {
                auto ret = QMessageBox::question(
                    this,
                    tr("Conflicto"),
                    QString(tr("«%1» ya existe en destino.\n¿Sobrescribir?")) .arg(fi.fileName()),
                    QMessageBox::Yes | QMessageBox::No | QMessageBox::YesToAll | QMessageBox::NoToAll
                );
                if (ret == QMessageBox::YesToAll) policy = OverwritePolicy::OverwriteAll;
                else if (ret == QMessageBox::NoToAll) policy = OverwritePolicy::SkipAll;

                if (ret == QMessageBox::No || policy == OverwritePolicy::SkipAll) {
                    ++skipped;
                    continue;
                }
            }
            QFileInfo tfi(target);
            if (tfi.isDir()) QDir(target).removeRecursively();
            else QFile::remove(target);
        }

        QString err;
        if (copyEntryRecursively(fi.absoluteFilePath(), target, err)) {
            ++ok;
        } else {
            ++fail;
            lastError = err;
        }
    }

    QString msg = QString(tr("Copiados: %1  |  Fallidos: %2  |  Saltados: %3"))
                      .arg(ok)
                      .arg(fail)
                      .arg(skipped);
    if (fail > 0 && !lastError.isEmpty()) msg += "\n" + tr("Último error: ") + lastError;
    statusBar()->showMessage(msg, 6000);
}

void MainWindow::moveLeftToRight() {
    if (rightIsRemote_) {
        if (!sftp_ || !rightRemoteModel_) { QMessageBox::warning(this, tr("SFTP"), tr("No hay sesión SFTP activa.")); return; }
        const auto rows = leftView_->selectionModel()->selectedRows(NAME_COL);
        if (rows.isEmpty()) { QMessageBox::information(this, tr("Mover"), tr("No hay entradas seleccionadas en el panel izquierdo.")); return; }
        if (QMessageBox::question(this, tr("Confirmar mover"),
                                  tr("Esto subirá al servidor y eliminará el origen local.\n¿Deseas continuar?")) != QMessageBox::Yes) return;

        enum class OverwritePolicy { Ask, OverwriteAll, SkipAll };
        OverwritePolicy policy = OverwritePolicy::Ask;
        int ok = 0, fail = 0, skipped = 0;
        QString lastError;
        const QString remoteBase = rightRemoteModel_->rootPath();

        auto ensureRemoteDir = [&](const QString& dir) {
            bool isD = false;
            std::string e;
            bool ex = sftp_->exists(dir.toStdString(), isD, e);
            if (!e.empty()) return false;
            if (!ex) {
                std::string me;
                if (!sftp_->mkdir(dir.toStdString(), me, 0755)) return false;
            }
            return true;
        };

        for (const QModelIndex& idx : rows) {
            const QFileInfo fi = leftModel_->fileInfo(idx);
            if (fi.isDir()) {
                const QString baseRemoteDir = joinRemotePath(remoteBase, fi.fileName());
                if (!ensureRemoteDir(baseRemoteDir)) { ++fail; continue; }
                bool allOk = true;
                QDirIterator it(fi.absoluteFilePath(), QDir::NoDotAndDotDot | QDir::AllEntries, QDirIterator::Subdirectories);
                while (it.hasNext()) {
                    it.next();
                    const QFileInfo sfi = it.fileInfo();
                    const QString rel = QDir(fi.absoluteFilePath()).relativeFilePath(sfi.absoluteFilePath());
                    const QString rTarget = joinRemotePath(baseRemoteDir, rel);
                    if (sfi.isDir()) {
                        if (!ensureRemoteDir(rTarget)) { allOk = false; break; }
                        continue;
                    }
                    bool isD = false;
                    std::string sErr;
                    bool exists = sftp_->exists(rTarget.toStdString(), isD, sErr);
                    if (!sErr.empty()) { lastError = QString::fromStdString(sErr); allOk = false; break; }
                    if (exists) {
                        if (policy == OverwritePolicy::Ask) {
                            auto ret = QMessageBox::question(
                                this,
                                tr("Conflicto remoto"),
                                QString(tr("«%1» ya existe.\n¿Sobrescribir?")) .arg(rel),
                                QMessageBox::Yes | QMessageBox::No | QMessageBox::YesToAll | QMessageBox::NoToAll
                            );
                            if (ret == QMessageBox::YesToAll) policy = OverwritePolicy::OverwriteAll;
                            else if (ret == QMessageBox::NoToAll) policy = OverwritePolicy::SkipAll;
                            if (ret == QMessageBox::No || policy == OverwritePolicy::SkipAll) { ++skipped; continue; }
                        } else if (policy == OverwritePolicy::SkipAll) { ++skipped; continue; }
                    }
                    QProgressDialog dlg(QString(tr("Subiendo %1")).arg(rel), tr("Cancelar"), 0, 100, this);
                    dlg.setWindowModality(Qt::ApplicationModal);
                    dlg.setMinimumDuration(0);
                    std::string err;
                    bool res = sftp_->put(
                        sfi.absoluteFilePath().toStdString(), rTarget.toStdString(), err,
                        [&](std::size_t done, std::size_t total) { int pct = (total > 0) ? int((done * 100) / total) : 0; dlg.setValue(pct); qApp->processEvents(); },
                        [&]() { return dlg.wasCanceled(); },
                        false
                    );
                    dlg.setValue(100);
                    if (!res) { lastError = QString::fromStdString(err); allOk = false; break; }
                }
                if (allOk) {
                    if (QDir(fi.absoluteFilePath()).removeRecursively()) ++ok;
                    else { ++fail; lastError = tr("No se pudo borrar origen: ") + fi.absoluteFilePath(); }
                } else {
                    ++fail;
                }
            } else {
                const QString rTarget = joinRemotePath(remoteBase, fi.fileName());
                bool isD = false;
                std::string sErr;
                bool exists = sftp_->exists(rTarget.toStdString(), isD, sErr);
                if (!sErr.empty()) { lastError = QString::fromStdString(sErr); ++fail; continue; }
                if (exists) {
                    if (policy == OverwritePolicy::Ask) {
                        auto ret = QMessageBox::question(
                            this,
                            tr("Conflicto remoto"),
                            QString(tr("«%1» ya existe.\n¿Sobrescribir?")) .arg(fi.fileName()),
                            QMessageBox::Yes | QMessageBox::No | QMessageBox::YesToAll | QMessageBox::NoToAll
                        );
                        if (ret == QMessageBox::YesToAll) policy = OverwritePolicy::OverwriteAll;
                        else if (ret == QMessageBox::NoToAll) policy = OverwritePolicy::SkipAll;
                        if (ret == QMessageBox::No || policy == OverwritePolicy::SkipAll) { ++skipped; continue; }
                    } else if (policy == OverwritePolicy::SkipAll) { ++skipped; continue; }
                }
                QProgressDialog dlg(QString(tr("Subiendo %1")).arg(fi.fileName()), tr("Cancelar"), 0, 100, this);
                dlg.setWindowModality(Qt::ApplicationModal);
                dlg.setMinimumDuration(0);
                std::string err;
                bool res = sftp_->put(
                    fi.absoluteFilePath().toStdString(), rTarget.toStdString(), err,
                    [&](std::size_t done, std::size_t total) { int pct = (total > 0) ? int((done * 100) / total) : 0; dlg.setValue(pct); qApp->processEvents(); },
                    [&]() { return dlg.wasCanceled(); },
                    false
                );
                dlg.setValue(100);
                if (res) {
                    if (QFile::remove(fi.absoluteFilePath())) ++ok;
                    else { ++fail; lastError = tr("No se pudo borrar origen: ") + fi.absoluteFilePath(); }
                } else {
                    ++fail;
                    lastError = QString::fromStdString(err);
                }
            }
        }
        QString m = QString(tr("Movidos OK: %1  |  Fallidos: %2  |  Omitidos: %3")).arg(ok).arg(fail).arg(skipped);
        if (fail > 0 && !lastError.isEmpty()) m += "\n" + tr("Último error: ") + lastError;
        statusBar()->showMessage(m, 5000);
        QString dummy;
        rightRemoteModel_->setRootPath(remoteBase, &dummy);
        return;
    }

    // ---- Existing LOCAL→LOCAL branch ----
    const QString dstDirPath = rightPath_->text();
    QDir dstDir(dstDirPath);
    if (!dstDir.exists()) { QMessageBox::warning(this, tr("Destino inválido"), tr("La carpeta de destino no existe.")); return; }
    const auto rows = leftView_->selectionModel()->selectedRows(NAME_COL);
    if (rows.isEmpty()) { QMessageBox::information(this, tr("Mover"), tr("No hay entradas seleccionadas en el panel izquierdo.")); return; }
    if (QMessageBox::question(this, tr("Confirmar mover"),
                              tr("Esto copiará y luego eliminará el origen.\n¿Deseas continuar?")) != QMessageBox::Yes) return;
    int ok = 0, fail = 0;
    QString lastError;
    for (const QModelIndex& idx : rows) {
        const QFileInfo fi = leftModel_->fileInfo(idx);
        const QString target = dstDir.filePath(fi.fileName());
        QString err;
        if (copyEntryRecursively(fi.absoluteFilePath(), target, err)) {
            bool removed = fi.isDir() ? QDir(fi.absoluteFilePath()).removeRecursively() : QFile::remove(fi.absoluteFilePath());
            if (removed) ok++;
            else { fail++; lastError = tr("No se pudo borrar origen: ") + fi.absoluteFilePath(); }
        } else {
            fail++;
            lastError = err;
        }
    }
    QString m = QString(tr("Movidos OK: %1  |  Fallidos: %2")).arg(ok).arg(fail);
    if (fail > 0 && !lastError.isEmpty()) m += "\n" + tr("Último error: ") + lastError;
    statusBar()->showMessage(m, 5000);
}

void MainWindow::deleteFromLeft() {
    const auto rows = leftView_->selectionModel()->selectedRows(NAME_COL);
    if (rows.isEmpty()) { QMessageBox::information(this, tr("Borrar"), tr("No hay entradas seleccionadas en el panel izquierdo.")); return; }
    if (QMessageBox::warning(this, tr("Confirmar borrado"),
                              tr("Esto eliminará permanentemente los elementos seleccionados en el panel izquierdo.\n¿Deseas continuar?"),
                              QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) return;
    int ok = 0, fail = 0;
    for (const QModelIndex& idx : rows) {
        const QFileInfo fi = leftModel_->fileInfo(idx);
        bool removed = fi.isDir() ? QDir(fi.absoluteFilePath()).removeRecursively() : QFile::remove(fi.absoluteFilePath());
        if (removed) ++ok; else ++fail;
    }
    statusBar()->showMessage(QString(tr("Borrados: %1  |  Fallidos: %2")).arg(ok).arg(fail), 5000);
}

void MainWindow::goUpLeft() {
    QString cur = leftPath_->text();
    QDir d(cur);
    if (!d.cdUp()) return;
    setLeftRoot(d.absolutePath());
    updateDeleteShortcutEnables();
}

void MainWindow::goUpRight() {
    if (rightIsRemote_) {
        if (!rightRemoteModel_) return;
        QString cur = rightRemoteModel_->rootPath();
        if (cur == "/" || cur.isEmpty()) return;
        if (cur.endsWith('/')) cur.chop(1);
        int slash = cur.lastIndexOf('/');
        QString parent = (slash <= 0) ? "/" : cur.left(slash);
        setRightRemoteRoot(parent);
    } else {
        QString cur = rightPath_->text();
        QDir d(cur);
        if (!d.cdUp()) return;
        setRightRoot(d.absolutePath());
        updateDeleteShortcutEnables();
    }
}

// Open the connection dialog and establish an SFTP session.
void MainWindow::connectSftp() {
    ConnectionDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted) return;
    std::string err;
    auto opt = dlg.options();
    // Apply global security preferences also for ad‑hoc connections (Advanced settings)
    {
        QSettings s("OpenSCP", "OpenSCP");
        opt.known_hosts_hash_names = s.value("Security/knownHostsHashed", true).toBool();
        opt.show_fp_hex = s.value("Security/fpHex", false).toBool();
    }
    if (!establishSftpAsync(opt, err)) {
        QMessageBox::critical(this, tr("Error de conexión"), QString::fromStdString(err));
        return;
    }
    applyRemoteConnectedUI(opt);
}

// Tear down the current SFTP session and restore local mode.
void MainWindow::disconnectSftp() {
    if (m_isDisconnecting) return;
    m_isDisconnecting = true;
    // Detach client from the queue to avoid dangling pointers
    if (transferMgr_) transferMgr_->clearClient();
    if (sftp_) sftp_->disconnect();
    sftp_.reset();
    if (rightRemoteModel_) {
        rightView_->setModel(rightLocalModel_);
        if (rightView_->selectionModel()) {
            connect(rightView_->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this]{ updateDeleteShortcutEnables(); });
        }
        delete rightRemoteModel_;
        rightRemoteModel_ = nullptr;
    }
    rightIsRemote_ = false;
    rightRemoteWritable_ = false;
    if (actConnect_) actConnect_->setEnabled(true);
    if (actDisconnect_) actDisconnect_->setEnabled(false);
    if (actDownloadF7_) actDownloadF7_->setEnabled(false);
    if (actUploadRight_) actUploadRight_->setEnabled(false);
    // Local mode: re-enable local actions on the right panel
    if (actNewDirRight_)   actNewDirRight_->setEnabled(true);
    if (actNewFileRight_)  actNewFileRight_->setEnabled(true);
    if (actRenameRight_)   actRenameRight_->setEnabled(true);
    if (actDeleteRight_)   actDeleteRight_->setEnabled(true);
    if (actMoveRight_)     actMoveRight_->setEnabled(true);
    if (actMoveRightTb_)   actMoveRightTb_->setEnabled(true);
    if (actCopyRightTb_)   actCopyRightTb_->setEnabled(true);
    if (actChooseRight_) {
        actChooseRight_->setIcon(QIcon(QLatin1String(":/assets/icons/action-open-folder.svg")));
        actChooseRight_->setEnabled(true);
        actChooseRight_->setToolTip(actChooseRight_->text());
    }
    statusBar()->showMessage(tr("Desconectado"), 3000);
    setWindowTitle(tr("OpenSCP — local/local"));
    updateDeleteShortcutEnables();

    // Per spec: non‑modal Site Manager after disconnect (if enabled), without blocking UI
    m_isDisconnecting = false;
    if (!QCoreApplication::closingDown() && m_openSiteManagerOnDisconnect) {
        QTimer::singleShot(0, this, [this]{ showSiteManagerNonModal(); });
    }
}

void MainWindow::setOpenSiteManagerOnDisconnect(bool on) {
    if (m_openSiteManagerOnDisconnect == on) return;
    m_openSiteManagerOnDisconnect = on;
    QSettings s("OpenSCP", "OpenSCP");
    s.setValue("UI/openSiteManagerOnDisconnect", on);
    s.sync();
}

void MainWindow::showSiteManagerNonModal() {
    if (QApplication::activeModalWidget()) {
        m_pendingOpenSiteManager = true;
        QObject* modal = QApplication::activeModalWidget();
        if (modal) connect(modal, &QObject::destroyed, this, &MainWindow::maybeOpenSiteManagerAfterModal, Qt::UniqueConnection);
        return; // don't open underneath a modal
    }
    if (m_siteManager) {
        m_siteManager->show();
        m_siteManager->raise();
        m_siteManager->activateWindow();
        return;
    }
    auto* dlg = new SiteManagerDialog(this);
    m_siteManager = dlg;
    dlg->setAttribute(Qt::WA_DeleteOnClose, true);
    connect(dlg, &QObject::destroyed, this, [this]{ m_siteManager.clear(); });
    connect(dlg, &QDialog::finished, this, [this, dlg](int res){
        if (res == QDialog::Accepted && dlg) {
            openscp::SessionOptions opt{};
            if (dlg->selectedOptions(opt)) {
                std::string err;
                if (!establishSftpAsync(opt, err)) {
                    QMessageBox::critical(this, tr("Error de conexión"), QString::fromStdString(err));
                } else {
                    applyRemoteConnectedUI(opt);
                }
            }
        }
    });
    QTimer::singleShot(0, dlg, [dlg]{ dlg->show(); dlg->raise(); dlg->activateWindow(); });
}

void MainWindow::setOpenSiteManagerOnStartup(bool on) {
    if (m_openSiteManagerOnStartup == on) return;
    m_openSiteManagerOnStartup = on;
    QSettings s("OpenSCP", "OpenSCP");
    s.setValue("UI/showConnOnStart", on);
    s.sync();
}

void MainWindow::maybeOpenSiteManagerAfterModal() {
    if (!QApplication::activeModalWidget() && m_pendingOpenSiteManager) {
        m_pendingOpenSiteManager = false;
        QTimer::singleShot(0, this, [this]{ showSiteManagerNonModal(); });
    }
}

// Navigate remote pane to a new remote directory.
void MainWindow::setRightRemoteRoot(const QString& path) {
    if (!rightIsRemote_ || !rightRemoteModel_) return;
    QString e;
    if (!rightRemoteModel_->setRootPath(path, &e)) {
        QMessageBox::warning(this, tr("Error remoto"), e);
        return;
    }
    rightPath_->setText(path);
    updateRemoteWriteability();
    updateDeleteShortcutEnables();
}

// Handle activation (double-click/Enter) on the right pane.
void MainWindow::rightItemActivated(const QModelIndex& idx) {
    // Local mode (right panel is local): navigate into directories
    if (!rightIsRemote_) {
        if (!rightLocalModel_) return;
        const QFileInfo fi = rightLocalModel_->fileInfo(idx);
        if (fi.isDir()) {
            setRightRoot(fi.absoluteFilePath());
        } else if (fi.isFile()) {
            // Decide open behavior (first-run prompt) for local files
            const QString localPath = fi.absoluteFilePath();
            QSettings s("OpenSCP", "OpenSCP");
            bool chosen = s.value("UI/openBehaviorChosen", false).toBool();
            bool reveal = s.value("UI/openRevealInFolder", false).toBool();
            if (!chosen) {
                QMessageBox box(this);
                box.setIcon(QMessageBox::Question);
                box.setWindowTitle(tr("Preferencia de apertura"));
                box.setText(tr("¿Cómo deseas abrir los archivos por defecto?\nPuedes cambiarlo luego en Ajustes."));
                // Normal grey: Open file (NoRole). Blue/default: Show folder (AcceptRole).
                QPushButton* btnOpen = box.addButton(tr("Abrir archivo"), QMessageBox::NoRole);
                QPushButton* btnReveal = box.addButton(tr("Mostrar carpeta"), QMessageBox::AcceptRole);
                box.setDefaultButton(btnReveal);
                box.exec();
                reveal = (box.clickedButton() == btnReveal);
                s.setValue("UI/openRevealInFolder", reveal);
                s.setValue("UI/openBehaviorChosen", true);
                s.sync();
                prefOpenRevealInFolder_ = reveal;
            } else {
                reveal = prefOpenRevealInFolder_;
            }
            if (reveal) revealInFolder(localPath);
            else QDesktopServices::openUrl(QUrl::fromLocalFile(localPath));
        }
        return;
    }
    // Remote mode: navigate or download/open file
    if (!rightRemoteModel_) return;
    if (rightRemoteModel_->isDir(idx)) {
        const QString name = rightRemoteModel_->nameAt(idx);
        QString next = rightRemoteModel_->rootPath();
        if (!next.endsWith('/')) next += '/';
        next += name;
        setRightRemoteRoot(next);
        return;
    }
    const QString name = rightRemoteModel_->nameAt(idx);
    {
        QString why; if (!isValidEntryName(name, &why)) { QMessageBox::warning(this, tr("Nombre inválido"), why); return; }
    }
    QString remotePath = rightRemoteModel_->rootPath();
    if (!remotePath.endsWith('/')) remotePath += '/';
    remotePath += name;
    const QString localPath = tempDownloadPathFor(name);
    // Avoid duplicates: if there is already an active download with same src/dst, do not enqueue again
    bool alreadyActive = false;
    {
        const auto& tasks = transferMgr_->tasks();
        for (const auto& t : tasks) {
            if (t.type == TransferTask::Type::Download && t.src == remotePath && t.dst == localPath) {
                if (t.status == TransferTask::Status::Queued || t.status == TransferTask::Status::Running || t.status == TransferTask::Status::Paused) {
                    alreadyActive = true; break;
                }
            }
        }
    }
    if (!alreadyActive) {
        // Enqueue download so it appears in the queue (instead of direct download)
        transferMgr_->enqueueDownload(remotePath, localPath);
        statusBar()->showMessage(QString(tr("Encolados: %1 descargas")).arg(1), 3000);
        if (!transferDlg_) transferDlg_ = new TransferQueueDialog(transferMgr_, this);
        transferDlg_->show(); transferDlg_->raise(); transferDlg_->activateWindow();
    } else {
        // There was already an identical task in the queue; optionally show it
        if (!transferDlg_) transferDlg_ = new TransferQueueDialog(transferMgr_, this);
        transferDlg_->show(); transferDlg_->raise(); transferDlg_->activateWindow();
        statusBar()->showMessage(tr("Descarga ya encolada"), 2000);
    }
    // Open the file when the corresponding task finishes (avoid duplicate listeners)
    static QSet<QString> sOpenListeners;
    const QString key = remotePath + "->" + localPath;
    if (!sOpenListeners.contains(key)) {
        sOpenListeners.insert(key);
        auto connPtr = std::make_shared<QMetaObject::Connection>();
        *connPtr = connect(transferMgr_, &TransferManager::tasksChanged, this, [this, remotePath, localPath, key, connPtr]() {
            const auto& tasks = transferMgr_->tasks();
            for (const auto& t : tasks) {
                if (t.type == TransferTask::Type::Download && t.src == remotePath && t.dst == localPath) {
                    if (t.status == TransferTask::Status::Done) {
                        // Decide how to open: reveal in folder (security) vs open directly
                        QSettings s("OpenSCP", "OpenSCP");
                        bool chosen = s.value("UI/openBehaviorChosen", false).toBool();
                        bool reveal = s.value("UI/openRevealInFolder", false).toBool();
                        if (!chosen) {
                            // Ask the user the first time a file is opened
                            QMessageBox box(this);
                            box.setIcon(QMessageBox::Question);
                            box.setWindowTitle(tr("Preferencia de apertura"));
                            box.setText(tr("¿Cómo deseas abrir los archivos por defecto?\nPuedes cambiarlo luego en Ajustes."));
                            // Normal grey: Open file (NoRole). Blue/default: Show folder (AcceptRole).
                            QPushButton* btnOpen = box.addButton(tr("Abrir archivo"), QMessageBox::NoRole);
                            QPushButton* btnReveal = box.addButton(tr("Mostrar carpeta"), QMessageBox::AcceptRole);
                            box.setDefaultButton(btnReveal);
                            box.exec();
                            if (box.clickedButton() == btnReveal) {
                                reveal = true;
                            } else {
                                reveal = false;
                            }
                            s.setValue("UI/openRevealInFolder", reveal);
                            s.setValue("UI/openBehaviorChosen", true);
                            s.sync();
                            // Keep the in-memory cache aligned
                            prefOpenRevealInFolder_ = reveal;
                        } else {
                            reveal = prefOpenRevealInFolder_;
                        }

                        if (reveal) revealInFolder(localPath);
                        else QDesktopServices::openUrl(QUrl::fromLocalFile(localPath));
                        statusBar()->showMessage(tr("Descargado: ") + localPath, 5000);
                        QObject::disconnect(*connPtr);
                        sOpenListeners.remove(key);
                    } else if (t.status == TransferTask::Status::Error || t.status == TransferTask::Status::Canceled) {
                        QObject::disconnect(*connPtr);
                        sOpenListeners.remove(key);
                    }
                    break;
                }
            }
        });
    }
    }

// Double click on the left panel: if it's a folder, enter it and replace root
void MainWindow::leftItemActivated(const QModelIndex& idx) {
    if (!leftModel_) return;
    const QFileInfo fi = leftModel_->fileInfo(idx);
    if (fi.isDir()) {
        setLeftRoot(fi.absoluteFilePath());
    } else if (fi.isFile()) {
        // Decide open behavior (first-run prompt) for local files
        const QString localPath = fi.absoluteFilePath();
        QSettings s("OpenSCP", "OpenSCP");
        bool chosen = s.value("UI/openBehaviorChosen", false).toBool();
        bool reveal = s.value("UI/openRevealInFolder", false).toBool();
        if (!chosen) {
            QMessageBox box(this);
            box.setIcon(QMessageBox::Question);
            box.setWindowTitle(tr("Preferencia de apertura"));
            box.setText(tr("¿Cómo deseas abrir los archivos por defecto?\nPuedes cambiarlo luego en Ajustes."));
            // Normal grey: Open file (NoRole). Blue/default: Show folder (AcceptRole).
            QPushButton* btnOpen = box.addButton(tr("Abrir archivo"), QMessageBox::NoRole);
            QPushButton* btnReveal = box.addButton(tr("Mostrar carpeta"), QMessageBox::AcceptRole);
            box.setDefaultButton(btnReveal);
            box.exec();
            reveal = (box.clickedButton() == btnReveal);
            s.setValue("UI/openRevealInFolder", reveal);
            s.setValue("UI/openBehaviorChosen", true);
            s.sync();
            prefOpenRevealInFolder_ = reveal;
        } else {
            reveal = prefOpenRevealInFolder_;
        }
        if (reveal) revealInFolder(localPath);
        else QDesktopServices::openUrl(QUrl::fromLocalFile(localPath));
    }
}

// Enqueue downloads from the right (remote) pane to a chosen local folder.
void MainWindow::downloadRightToLeft() {
    if (!rightIsRemote_) { QMessageBox::information(this, tr("Descargar"), tr("El panel derecho no es remoto.")); return; }
    if (!sftp_ || !rightRemoteModel_) { QMessageBox::warning(this, tr("SFTP"), tr("No hay sesión SFTP activa.")); return; }
    const QString picked = QFileDialog::getExistingDirectory(this, tr("Selecciona carpeta de destino (local)"), downloadDir_.isEmpty() ? QDir::homePath() : downloadDir_);
    if (picked.isEmpty()) return;
    downloadDir_ = picked;
    QDir dst(downloadDir_);
    if (!dst.exists()) { QMessageBox::warning(this, tr("Destino inválido"), tr("La carpeta de destino no existe.")); return; }
    auto sel = rightView_->selectionModel();
    QModelIndexList rows;
    if (sel) rows = sel->selectedRows(NAME_COL);
    if (rows.isEmpty()) {
        // Download everything visible (first level) if there is no selection
        int rc = rightRemoteModel_ ? rightRemoteModel_->rowCount() : 0;
        for (int r = 0; r < rc; ++r) rows << rightRemoteModel_->index(r, NAME_COL);
        if (rows.isEmpty()) { QMessageBox::information(this, tr("Descargar"), tr("Nada para descargar.")); return; }
    }
    int enq = 0;
    int bad = 0;
    const QString remoteBase = rightRemoteModel_->rootPath();
    for (const QModelIndex& idx : rows) {
        const QString name = rightRemoteModel_->nameAt(idx);
        {
            QString why; if (!isValidEntryName(name, &why)) { ++bad; continue; }
        }
        QString rpath = remoteBase;
        if (!rpath.endsWith('/')) rpath += '/';
        rpath += name;
        const QString lpath = dst.filePath(name);
        if (rightRemoteModel_->isDir(idx)) {
            QVector<QPair<QString, QString>> stack;
            stack.push_back({ rpath, lpath });
            while (!stack.isEmpty()) {
                auto pair = stack.back();
                stack.pop_back();
                const QString curR = pair.first;
                const QString curL = pair.second;
                QDir().mkpath(curL);
                std::vector<openscp::FileInfo> out;
                std::string lerr;
                if (!sftp_->list(curR.toStdString(), out, lerr)) continue;
                for (const auto& e : out) {
                    const QString ename = QString::fromStdString(e.name);
                    QString why; if (!isValidEntryName(ename, &why)) { ++bad; continue; }
                    const QString childR = (curR.endsWith('/') ? curR + ename : curR + "/" + ename);
                    const QString childL = QDir(curL).filePath(ename);
                    if (e.is_dir) stack.push_back({ childR, childL });
                    else { transferMgr_->enqueueDownload(childR, childL); ++enq; }
                }
            }
        } else {
            transferMgr_->enqueueDownload(rpath, lpath);
            ++enq;
        }
    }
    if (enq > 0) {
        QString msg = QString(tr("Encolados: %1 descargas")).arg(enq);
        if (bad > 0) msg += QString("  |  ") + tr("Omitidos inválidos: %1").arg(bad);
        statusBar()->showMessage(msg, 4000);
        if (!transferDlg_) transferDlg_ = new TransferQueueDialog(transferMgr_, this);
        transferDlg_->show(); transferDlg_->raise(); transferDlg_->activateWindow();
    }
}

// Copy the selection from the right panel to the left.
// - Remote -> enqueue downloads (non-blocking).
// - Local  -> local-to-local copy (with overwrite policy).
void MainWindow::copyRightToLeft() {
    QDir dst(leftPath_->text());
    if (!dst.exists()) { QMessageBox::warning(this, tr("Destino inválido"), tr("La carpeta de destino (panel izquierdo) no existe.")); return; }
    auto sel = rightView_->selectionModel();
    if (!sel) { QMessageBox::warning(this, tr("Copiar"), tr("No hay selección.")); return; }
    const auto rows = sel->selectedRows(NAME_COL);
    if (rows.isEmpty()) { QMessageBox::information(this, tr("Copiar"), tr("Nada seleccionado.")); return; }

    if (!rightIsRemote_) {
        // Local -> Local copy (right to left)
        enum class OverwritePolicy { Ask, OverwriteAll, SkipAll };
        OverwritePolicy policy = OverwritePolicy::Ask;
        int ok = 0, fail = 0, skipped = 0;
        QString lastError;
        for (const QModelIndex& idx : rows) {
            const QFileInfo fi = rightLocalModel_->fileInfo(idx);
            const QString target = dst.filePath(fi.fileName());
            if (QFileInfo::exists(target)) {
                if (policy == OverwritePolicy::Ask) {
                    auto ret = QMessageBox::question(
                        this,
                        tr("Conflicto"),
                        QString(tr("«%1» ya existe en destino.\n¿Sobrescribir?")) .arg(fi.fileName()),
                        QMessageBox::Yes | QMessageBox::No | QMessageBox::YesToAll | QMessageBox::NoToAll
                    );
                    if (ret == QMessageBox::YesToAll) policy = OverwritePolicy::OverwriteAll;
                    else if (ret == QMessageBox::NoToAll) policy = OverwritePolicy::SkipAll;
                    if (ret == QMessageBox::No || policy == OverwritePolicy::SkipAll) { ++skipped; continue; }
                }
                QFileInfo tfi(target);
                if (tfi.isDir()) QDir(target).removeRecursively(); else QFile::remove(target);
            }
            QString err;
            if (copyEntryRecursively(fi.absoluteFilePath(), target, err)) ++ok; else { ++fail; lastError = err; }
        }
        QString m = QString(tr("Copiados: %1  |  Fallidos: %2  |  Saltados: %3")).arg(ok).arg(fail).arg(skipped);
        if (fail > 0 && !lastError.isEmpty()) m += "\n" + tr("Último error: ") + lastError;
        statusBar()->showMessage(m, 5000);
        setRightRoot(rightPath_->text());
        return;
    }

    // Remote -> Local: enqueue downloads
    if (!sftp_ || !rightRemoteModel_) { QMessageBox::warning(this, tr("SFTP"), tr("No hay sesión SFTP activa.")); return; }
    int enq = 0;
    int bad = 0;
    const QString remoteBase = rightRemoteModel_->rootPath();
    for (const QModelIndex& idx : rows) {
        const QString name = rightRemoteModel_->nameAt(idx);
        {
            QString why; if (!isValidEntryName(name, &why)) { ++bad; continue; }
        }
        QString rpath = remoteBase;
        if (!rpath.endsWith('/')) rpath += '/';
        rpath += name;
        const QString lpath = dst.filePath(name);
        if (rightRemoteModel_->isDir(idx)) {
            QVector<QPair<QString, QString>> stack;
            stack.push_back({ rpath, lpath });
            while (!stack.isEmpty()) {
                auto pair = stack.back();
                stack.pop_back();
                const QString curR = pair.first;
                const QString curL = pair.second;
                QDir().mkpath(curL);
                std::vector<openscp::FileInfo> out;
                std::string lerr;
                if (!sftp_->list(curR.toStdString(), out, lerr)) continue;
                for (const auto& e : out) {
                    const QString ename = QString::fromStdString(e.name);
                    QString why; if (!isValidEntryName(ename, &why)) { ++bad; continue; }
                    const QString childR = (curR.endsWith('/') ? curR + ename : curR + "/" + ename);
                    const QString childL = QDir(curL).filePath(ename);
                    if (e.is_dir) stack.push_back({ childR, childL });
                    else { transferMgr_->enqueueDownload(childR, childL); ++enq; }
                }
            }
        } else {
            transferMgr_->enqueueDownload(rpath, lpath);
            ++enq;
        }
    }
    if (enq > 0) {
        QString msg = QString(tr("Encolados: %1 descargas")).arg(enq);
        if (bad > 0) msg += QString("  |  ") + tr("Omitidos inválidos: %1").arg(bad);
        statusBar()->showMessage(msg, 4000);
        if (!transferDlg_) transferDlg_ = new TransferQueueDialog(transferMgr_, this);
        transferDlg_->show(); transferDlg_->raise(); transferDlg_->activateWindow();
    }
}

// Move the selection from the right panel to the left.
// - Remote -> download with progress and delete remotely on success.
// - Local  -> local copy and delete the source.
void MainWindow::moveRightToLeft() {
    auto sel = rightView_->selectionModel();
    if (!sel || sel->selectedRows(NAME_COL).isEmpty()) { QMessageBox::information(this, tr("Mover"), tr("Nada seleccionado.")); return; }
    QDir dst(leftPath_->text());
    if (!dst.exists()) { QMessageBox::warning(this, tr("Destino inválido"), tr("La carpeta de destino (panel izquierdo) no existe.")); return; }

    if (!rightIsRemote_) {
        // Local -> Local: move (copy then delete)
        enum class OverwritePolicy { Ask, OverwriteAll, SkipAll };
        OverwritePolicy policy = OverwritePolicy::Ask;
        int ok = 0, fail = 0, skipped = 0;
        QString lastError;
        const auto rows = sel->selectedRows(NAME_COL);
        for (const QModelIndex& idx : rows) {
            const QFileInfo fi = rightLocalModel_->fileInfo(idx);
            const QString target = dst.filePath(fi.fileName());
            if (QFileInfo::exists(target)) {
                if (policy == OverwritePolicy::Ask) {
                    auto ret = QMessageBox::question(
                        this,
                        tr("Conflicto"),
                        QString(tr("«%1» ya existe en destino.\n¿Sobrescribir?")) .arg(fi.fileName()),
                        QMessageBox::Yes | QMessageBox::No | QMessageBox::YesToAll | QMessageBox::NoToAll
                    );
                    if (ret == QMessageBox::YesToAll) policy = OverwritePolicy::OverwriteAll;
                    else if (ret == QMessageBox::NoToAll) policy = OverwritePolicy::SkipAll;
                    if (ret == QMessageBox::No || policy == OverwritePolicy::SkipAll) { ++skipped; continue; }
                }
                QFileInfo tfi(target);
                if (tfi.isDir()) QDir(target).removeRecursively(); else QFile::remove(target);
            }
            QString err;
            if (copyEntryRecursively(fi.absoluteFilePath(), target, err)) {
                bool removed = fi.isDir() ? QDir(fi.absoluteFilePath()).removeRecursively() : QFile::remove(fi.absoluteFilePath());
                if (removed) ++ok; else { ++fail; lastError = tr("No se pudo borrar origen: ") + fi.absoluteFilePath(); }
            } else { ++fail; lastError = err; }
        }
        QString m = QString(tr("Movidos OK: %1  |  Fallidos: %2  |  Omitidos: %3")).arg(ok).arg(fail).arg(skipped);
        if (fail > 0 && !lastError.isEmpty()) m += "\n" + tr("Último error: ") + lastError;
        statusBar()->showMessage(m, 5000);
        setRightRoot(rightPath_->text());
        return;
    }

    // Remote -> Local: enqueue downloads and delete remote on completion
    if (!sftp_ || !rightRemoteModel_) { QMessageBox::warning(this, tr("SFTP"), tr("No hay sesión SFTP activa.")); return; }
    const auto rows = sel->selectedRows(NAME_COL);
    const QString remoteBase = rightRemoteModel_->rootPath();
    QVector<QPair<QString, QString>> pairs; // (remote, local) files to download
    int bad = 0;
    struct TopSel { QString rpath; bool isDir; };
    QVector<TopSel> top;
    int enq = 0;
    for (const QModelIndex& idx : rows) {
        const QString name = rightRemoteModel_->nameAt(idx);
        { QString why; if (!isValidEntryName(name, &why)) { ++bad; continue; } }
        QString rpath = remoteBase; if (!rpath.endsWith('/')) rpath += '/'; rpath += name;
        const QString lpath = dst.filePath(name);
        const bool isDir = rightRemoteModel_->isDir(idx);
        top.push_back({ rpath, isDir });
        if (isDir) {
            QVector<QPair<QString, QString>> stack; stack.push_back({ rpath, lpath });
            while (!stack.isEmpty()) {
                auto pair = stack.back(); stack.pop_back();
                const QString curR = pair.first; const QString curL = pair.second; QDir().mkpath(curL);
                std::vector<openscp::FileInfo> out; std::string lerr;
                if (!sftp_->list(curR.toStdString(), out, lerr)) continue;
                for (const auto& e : out) {
                    const QString ename = QString::fromStdString(e.name);
                    QString why; if (!isValidEntryName(ename, &why)) { ++bad; continue; }
                    const QString childR = (curR.endsWith('/') ? curR + ename : curR + "/" + ename);
                    const QString childL = QDir(curL).filePath(ename);
                    if (e.is_dir) { stack.push_back({ childR, childL }); }
                    else { QDir().mkpath(QFileInfo(childL).dir().absolutePath()); pairs.push_back({ childR, childL }); }
                }
            }
        } else {
            QDir().mkpath(QFileInfo(lpath).dir().absolutePath());
            pairs.push_back({ rpath, lpath });
        }
    }
    for (const auto& p : pairs) { transferMgr_->enqueueDownload(p.first, p.second); ++enq; }
    if (enq > 0) {
        QString msg = QString(tr("Encolados: %1 descargas (mover)")).arg(enq);
        if (bad > 0) msg += QString("  |  ") + tr("Omitidos inválidos: %1").arg(bad);
        statusBar()->showMessage(msg, 4000);
        if (!transferDlg_) transferDlg_ = new TransferQueueDialog(transferMgr_, this);
        transferDlg_->show(); transferDlg_->raise(); transferDlg_->activateWindow();
    }
    // Per-item deletion: as each download finishes OK, delete that remote file;
    // when a folder has no pending files left, delete the folder.
    if (enq > 0) {
        struct MoveState {
            QSet<QString> filesPending;                 // remote files pending deletion
            QSet<QString> filesProcessed;               // remote files already processed (avoid duplicates)
            QHash<QString, QString> fileToTopDir;       // remote file -> top dir rpath
            QHash<QString, int> remainingInTopDir;      // top dir -> count of pending successful files
            QSet<QString> topDirs;                      // rpaths of top entries that are directories
            QSet<QString> deletedDirs;                  // top dirs already deleted
        };
        auto state = std::make_shared<MoveState>();
        // Initialize top dir mapping and counters
        for (const auto& tsel : top) if (tsel.isDir) { state->topDirs.insert(tsel.rpath); state->remainingInTopDir.insert(tsel.rpath, 0); }
        for (const auto& pr : pairs) {
            state->filesPending.insert(pr.first);
            // Locate containing top directory
            QString foundTop;
            for (const auto& tsel : top) {
                if (!tsel.isDir) continue;
                const QString prefix = tsel.rpath.endsWith('/') ? tsel.rpath : (tsel.rpath + '/');
                if (pr.first == tsel.rpath || pr.first.startsWith(prefix)) { foundTop = tsel.rpath; break; }
            }
            if (!foundTop.isEmpty()) {
                state->fileToTopDir.insert(pr.first, foundTop);
                state->remainingInTopDir[foundTop] = state->remainingInTopDir.value(foundTop) + 1;
            }
        }
        // If there are directories with 0 files, try to delete them only if empty
        for (auto it = state->remainingInTopDir.begin(); it != state->remainingInTopDir.end(); ++it) {
            if (it.value() == 0) {
                std::vector<openscp::FileInfo> out; std::string lerr;
                if (sftp_ && sftp_->list(it.key().toStdString(), out, lerr) && out.empty()) {
                    std::string derr; if (sftp_->removeDir(it.key().toStdString(), derr)) {
                        state->deletedDirs.insert(it.key());
                    }
                }
            }
        }
        auto connPtr = std::make_shared<QMetaObject::Connection>();
        *connPtr = connect(transferMgr_, &TransferManager::tasksChanged, this, [this, state, remoteBase, connPtr, pairs]() {
            const auto& tasks = transferMgr_->tasks();
            // 1) For each successfully completed task, delete the corresponding remote file (once)
            for (const auto& t : tasks) {
                if (t.type != TransferTask::Type::Download) continue;
                if (t.status != TransferTask::Status::Done) continue;
                const QString r = t.src;
                if (!state->filesPending.contains(r)) continue; // does not belong to this move or already deleted
                if (state->filesProcessed.contains(r)) continue;
                // Try to delete the remote file
                std::string ferr; bool okDel = sftp_ && sftp_->removeFile(r.toStdString(), ferr);
                state->filesProcessed.insert(r);
                if (okDel) {
                    state->filesPending.remove(r);
                    // Decrement counter for the top directory it belongs to
                    const QString topDir = state->fileToTopDir.value(r);
                    if (!topDir.isEmpty()) {
                        int rem = state->remainingInTopDir.value(topDir) - 1;
                        state->remainingInTopDir[topDir] = rem;
                        if (rem == 0 && !state->deletedDirs.contains(topDir)) {
                            // All files under this top dir were moved: delete folder only if empty
                            std::vector<openscp::FileInfo> out; std::string lerr;
                            if (sftp_ && sftp_->list(topDir.toStdString(), out, lerr) && out.empty()) {
                                std::string derr; if (sftp_->removeDir(topDir.toStdString(), derr)) {
                                    state->deletedDirs.insert(topDir);
                                }
                            }
                        }
                    }
                } else {
                    // Not deleted: keep it out to avoid endless retries; could retry if desired
                    state->filesPending.remove(r);
                }
            }

            // 2) Disconnect when all related tasks have reached a final state
            bool allFinal = true;
            for (const auto& pr : pairs) {
                bool found = false, final = false;
                for (const auto& t : tasks) {
                    if (t.type == TransferTask::Type::Download && t.src == pr.first && t.dst == pr.second) {
                        found = true;
                        if (t.status == TransferTask::Status::Done || t.status == TransferTask::Status::Error || t.status == TransferTask::Status::Canceled) final = true;
                        break;
                    }
                }
                if (!found || !final) { allFinal = false; break; }
            }
            if (allFinal) {
                // Refrescar vista remota una vez al final
                QString dummy; if (rightRemoteModel_) rightRemoteModel_->setRootPath(remoteBase, &dummy);
                updateRemoteWriteability();
                QObject::disconnect(*connPtr);
            }
        });
    }
}

void MainWindow::uploadViaDialog() {
    if (!rightIsRemote_ || !sftp_ || !rightRemoteModel_) { QMessageBox::information(this, tr("Subir"), tr("El panel derecho no es remoto o no hay sesión activa.")); return; }
    const QString startDir = uploadDir_.isEmpty() ? QDir::homePath() : uploadDir_;
    QFileDialog dlg(this, tr("Selecciona archivos o carpetas a subir"), startDir);
    dlg.setFileMode(QFileDialog::ExistingFiles);
    dlg.setOption(QFileDialog::DontUseNativeDialog, true);
    dlg.setViewMode(QFileDialog::Detail);
    if (auto* lv = dlg.findChild<QListView*>("listView")) lv->setSelectionMode(QAbstractItemView::ExtendedSelection);
    if (auto* tv = dlg.findChild<QTreeView*>())           tv->setSelectionMode(QAbstractItemView::ExtendedSelection);
    if (dlg.exec() != QDialog::Accepted) return;
    const QStringList picks = dlg.selectedFiles();
    if (picks.isEmpty()) return;
    uploadDir_ = QFileInfo(picks.first()).dir().absolutePath();
    QStringList files;
    for (const QString& p : picks) {
        QFileInfo fi(p);
        if (fi.isDir()) {
            QDirIterator it(p, QDir::NoDotAndDotDot | QDir::AllEntries, QDirIterator::Subdirectories);
            while (it.hasNext()) { it.next(); if (it.fileInfo().isFile()) files << it.filePath(); }
        } else if (fi.isFile()) {
            files << fi.absoluteFilePath();
        }
    }
    if (files.isEmpty()) { statusBar()->showMessage(tr("Nada para subir."), 4000); return; }
    int enq = 0;
    const QString remoteBase = rightRemoteModel_->rootPath();
    for (const QString& localPath : files) {
        const QFileInfo fi(localPath);
        QString relBase = fi.path().startsWith(uploadDir_) ? fi.path().mid(uploadDir_.size()).trimmed() : QString();
        if (relBase.startsWith('/')) relBase.remove(0, 1);
        QString targetDir = relBase.isEmpty() ? remoteBase : joinRemotePath(remoteBase, relBase);
        if (!targetDir.isEmpty() && targetDir != remoteBase) {
            bool isDir = false;
            std::string se;
            bool ex = sftp_->exists(targetDir.toStdString(), isDir, se);
            if (!ex && se.empty()) {
                std::string me;
                sftp_->mkdir(targetDir.toStdString(), me, 0755);
            }
        }
        const QString rTarget = joinRemotePath(targetDir, fi.fileName());
        transferMgr_->enqueueUpload(localPath, rTarget);
        ++enq;
    }
    if (enq > 0) {
        statusBar()->showMessage(QString(tr("Encolados: %1 subidas")).arg(enq), 4000);
        if (!transferDlg_) transferDlg_ = new TransferQueueDialog(transferMgr_, this);
        transferDlg_->show(); transferDlg_->raise(); transferDlg_->activateWindow();
    }
}



// Create a new directory in the right pane (local or remote).
void MainWindow::newDirRight() {
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Nueva carpeta"), tr("Nombre:"), QLineEdit::Normal, {}, &ok);
    if (!ok || name.isEmpty()) return;
    QString why; if (!isValidEntryName(name, &why)) { QMessageBox::warning(this, tr("Nombre inválido"), why); return; }
    if (rightIsRemote_) {
        if (!sftp_ || !rightRemoteModel_) return;
        const QString path = joinRemotePath(rightRemoteModel_->rootPath(), name);
        std::string err;
        if (!sftp_->mkdir(path.toStdString(), err, 0755)) { QMessageBox::critical(this, tr("SFTP"), QString::fromStdString(err)); return; }
        QString dummy;
        rightRemoteModel_->setRootPath(rightRemoteModel_->rootPath(), &dummy);
        updateRemoteWriteability();
    } else {
        QDir base(rightPath_->text());
        if (!base.mkpath(base.filePath(name))) { QMessageBox::critical(this, tr("Local"), tr("No se pudo crear carpeta.")); return; }
        setRightRoot(base.absolutePath());
    }
}

// Create a new empty file in the right pane (local only).
void MainWindow::newFileRight() {
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Nuevo archivo"), tr("Nombre:"), QLineEdit::Normal, {}, &ok);
    if (!ok || name.isEmpty()) return;
    QString why; if (!isValidEntryName(name, &why)) { QMessageBox::warning(this, tr("Nombre inválido"), why); return; }
    if (rightIsRemote_) {
        if (!sftp_ || !rightRemoteModel_) return;
        const QString remotePath = joinRemotePath(rightRemoteModel_->rootPath(), name);
        bool isDir = false; std::string e;
        bool exists = sftp_->exists(remotePath.toStdString(), isDir, e);
        if (exists) {
            if (QMessageBox::question(this, tr("Archivo existe"),
                                      tr("«%1» ya existe.\n¿Sobrescribir?").arg(name),
                                      QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) return;
        } else if (!e.empty()) {
            QMessageBox::critical(this, tr("SFTP"), QString::fromStdString(e));
            return;
        }

        QTemporaryFile tmp;
        if (!tmp.open()) { QMessageBox::critical(this, tr("Temporal"), tr("No se pudo crear un archivo temporal.")); return; }
        tmp.close();
        std::string err;
        bool okPut = sftp_->put(tmp.fileName().toStdString(), remotePath.toStdString(), err);
        if (!okPut) { QMessageBox::critical(this, tr("SFTP"), QString::fromStdString(err)); return; }
        QString dummy; rightRemoteModel_->setRootPath(rightRemoteModel_->rootPath(), &dummy);
        updateRemoteWriteability();
        statusBar()->showMessage(tr("Archivo creado: ") + remotePath, 4000);
    } else {
        QDir base(rightPath_->text());
        const QString path = base.filePath(name);
        if (QFileInfo::exists(path)) {
            if (QMessageBox::question(this, tr("Archivo existe"),
                                      tr("«%1» ya existe.\n¿Sobrescribir?").arg(name),
                                      QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) return;
        }
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) { QMessageBox::critical(this, tr("Local"), tr("No se pudo crear archivo.")); return; }
        f.close();
        setRightRoot(base.absolutePath());
        statusBar()->showMessage(tr("Archivo creado: ") + path, 4000);
    }
}

// Rename the selected entry on the right pane (local or remote).
void MainWindow::renameRightSelected() {
    auto sel = rightView_->selectionModel();
    if (!sel) return;
    const auto rows = sel->selectedRows();
    if (rows.size() != 1) { QMessageBox::information(this, tr("Renombrar"), tr("Selecciona exactamente un elemento.")); return; }
    if (rightIsRemote_) {
        if (!sftp_ || !rightRemoteModel_) return;
        const QModelIndex idx = rows.first();
        const QString oldName = rightRemoteModel_->nameAt(idx);
        bool ok = false;
        const QString newName = QInputDialog::getText(this, tr("Renombrar"), tr("Nuevo nombre:"), QLineEdit::Normal, oldName, &ok);
        if (!ok || newName.isEmpty() || newName == oldName) return;
        const QString base = rightRemoteModel_->rootPath();
        const QString from = joinRemotePath(base, oldName);
        const QString to   = joinRemotePath(base, newName);
        std::string err;
        if (!sftp_->rename(from.toStdString(), to.toStdString(), err, false)) { QMessageBox::critical(this, tr("SFTP"), QString::fromStdString(err)); return; }
        QString dummy;
        rightRemoteModel_->setRootPath(base, &dummy);
        updateRemoteWriteability();
    } else {
        const QModelIndex idx = rows.first();
        const QFileInfo fi = rightLocalModel_->fileInfo(idx);
        bool ok = false;
        const QString newName = QInputDialog::getText(this, tr("Renombrar"), tr("Nuevo nombre:"), QLineEdit::Normal, fi.fileName(), &ok);
        if (!ok || newName.isEmpty() || newName == fi.fileName()) return;
        const QString newPath = QDir(fi.absolutePath()).filePath(newName);
        bool renamed = QFile::rename(fi.absoluteFilePath(), newPath);
        if (!renamed) renamed = QDir(fi.absolutePath()).rename(fi.absoluteFilePath(), newPath);
        if (!renamed) { QMessageBox::critical(this, tr("Local"), tr("No se pudo renombrar.")); return; }
        setRightRoot(rightPath_->text());
    }
}

// Rename the selected entry on the left (local) pane.
void MainWindow::renameLeftSelected() {
    auto sel = leftView_->selectionModel();
    if (!sel) return;
    const auto rows = sel->selectedRows();
    if (rows.size() != 1) { QMessageBox::information(this, tr("Renombrar"), tr("Selecciona exactamente un elemento.")); return; }
    const QModelIndex idx = rows.first();
    const QFileInfo fi = leftModel_->fileInfo(idx);
    bool ok = false;
    const QString newName = QInputDialog::getText(this, tr("Renombrar"), tr("Nuevo nombre:"), QLineEdit::Normal, fi.fileName(), &ok);
    if (!ok || newName.isEmpty() || newName == fi.fileName()) return;
    const QString newPath = QDir(fi.absolutePath()).filePath(newName);
    bool renamed = QFile::rename(fi.absoluteFilePath(), newPath);
    if (!renamed) renamed = QDir(fi.absolutePath()).rename(fi.absoluteFilePath(), newPath);
    if (!renamed) { QMessageBox::critical(this, tr("Local"), tr("No se pudo renombrar.")); return; }
    setLeftRoot(leftPath_->text());
}

// Create a new directory in the left (local) pane.
void MainWindow::newDirLeft() {
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Nueva carpeta"), tr("Nombre:"), QLineEdit::Normal, {}, &ok);
    if (!ok || name.isEmpty()) return;
    QString why; if (!isValidEntryName(name, &why)) { QMessageBox::warning(this, tr("Nombre inválido"), why); return; }
    QDir base(leftPath_->text());
    if (!base.mkpath(base.filePath(name))) { QMessageBox::critical(this, tr("Local"), tr("No se pudo crear carpeta.")); return; }
    setLeftRoot(base.absolutePath());
}

// Create a new empty file in the left (local) pane.
void MainWindow::newFileLeft() {
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Nuevo archivo"), tr("Nombre:"), QLineEdit::Normal, {}, &ok);
    if (!ok || name.isEmpty()) return;
    QString why; if (!isValidEntryName(name, &why)) { QMessageBox::warning(this, tr("Nombre inválido"), why); return; }
    QDir base(leftPath_->text());
    const QString path = base.filePath(name);
    if (QFileInfo::exists(path)) {
        if (QMessageBox::question(this, tr("Archivo existe"),
                                  tr("«%1» ya existe.\n¿Sobrescribir?").arg(name),
                                  QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) return;
    }
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) { QMessageBox::critical(this, tr("Local"), tr("No se pudo crear archivo.")); return; }
    f.close();
    setLeftRoot(base.absolutePath());
    statusBar()->showMessage(tr("Archivo creado: ") + path, 4000);
}

// Delete the selected entries from the right pane (local or remote).
void MainWindow::deleteRightSelected() {
    auto sel = rightView_->selectionModel();
    if (!sel) return;
    const auto rows = sel->selectedRows();
    if (rows.isEmpty()) { QMessageBox::information(this, tr("Borrar"), tr("Nada seleccionado.")); return; }
    if (rightIsRemote_) {
        if (!sftp_ || !rightRemoteModel_) return;
        if (QMessageBox::warning(this, tr("Confirmar borrado"), tr("Esto eliminará permanentemente en el servidor remoto.\n¿Continuar?"), QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) return;
        int ok = 0, fail = 0;
        QString lastErr;
        const QString base = rightRemoteModel_->rootPath();
        std::function<bool(const QString&)> delRec = [&](const QString& p) {
            // Determine if path is a directory or a file using stat/exists
            bool isDir = false;
            std::string xerr;
            if (!sftp_->exists(p.toStdString(), isDir, xerr)) {
                // If it doesn't exist, treat as success
                if (xerr.empty()) return true;
                lastErr = QString::fromStdString(xerr);
                return false;
            }
            if (!isDir) {
                std::string ferr;
                if (!sftp_->removeFile(p.toStdString(), ferr)) { lastErr = QString::fromStdString(ferr); return false; }
                return true;
            }
            // Directory: list and remove children first (depth-first)
            std::vector<openscp::FileInfo> out;
            std::string lerr;
            if (!sftp_->list(p.toStdString(), out, lerr)) {
                lastErr = QString::fromStdString(lerr);
                return false;
            }
            for (const auto& e : out) {
                const QString child = joinRemotePath(p, QString::fromStdString(e.name));
                if (!delRec(child)) return false;
            }
            std::string derr;
            if (!sftp_->removeDir(p.toStdString(), derr)) { lastErr = QString::fromStdString(derr); return false; }
            return true;
        };
        for (const QModelIndex& idx : rows) {
            const QString name = rightRemoteModel_->nameAt(idx);
            const QString path = joinRemotePath(base, name);
            if (delRec(path)) ++ok; else ++fail;
        }
    QString msg = QString(tr("Borrados OK: %1  |  Fallidos: %2")).arg(ok).arg(fail);
        if (fail > 0 && !lastErr.isEmpty()) msg += "\n" + tr("Último error: ") + lastErr;
        statusBar()->showMessage(msg, 6000);
        QString dummy;
        rightRemoteModel_->setRootPath(base, &dummy);
        updateRemoteWriteability();
    } else {
        if (QMessageBox::warning(this, tr("Confirmar borrado"), tr("Esto eliminará permanentemente en el disco local.\n¿Continuar?"), QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) return;
        int ok = 0, fail = 0;
        for (const QModelIndex& idx : rows) {
            const QFileInfo fi = rightLocalModel_->fileInfo(idx);
            bool removed = fi.isDir() ? QDir(fi.absoluteFilePath()).removeRecursively() : QFile::remove(fi.absoluteFilePath());
            if (removed) ++ok; else ++fail;
        }
        statusBar()->showMessage(QString(tr("Borrados: %1  |  Fallidos: %2")).arg(ok).arg(fail), 5000);
        setRightRoot(rightPath_->text());
    }
}

// Show context menu for the right pane based on current state.
void MainWindow::showRightContextMenu(const QPoint& pos) {
    if (!rightContextMenu_) rightContextMenu_ = new QMenu(this);
    rightContextMenu_->clear();

    // Selection state and ability to go up
    bool hasSel = false;
    if (auto sel = rightView_->selectionModel()) {
        hasSel = !sel->selectedRows(NAME_COL).isEmpty();
    }
    // Is there a parent directory?
    bool canGoUp = false;
    if (rightIsRemote_) {
        QString cur = rightRemoteModel_ ? rightRemoteModel_->rootPath() : QString();
        if (cur.endsWith('/')) cur.chop(1);
        canGoUp = (!cur.isEmpty() && cur != "/");
    } else {
        QDir d(rightPath_ ? rightPath_->text() : QString());
        canGoUp = d.cdUp();
    }

    if (rightIsRemote_) {
        // Up option (if applicable)
        if (canGoUp && actUpRight_) rightContextMenu_->addAction(actUpRight_);

        // Always show "Download" on remote, regardless of selection
        if (actDownloadF7_) rightContextMenu_->addAction(actDownloadF7_);

        if (!hasSel) {
            // No selection: creation and navigation
            if (rightRemoteWritable_) {
                if (actNewFileRight_) rightContextMenu_->addAction(actNewFileRight_);
                if (actNewDirRight_)  rightContextMenu_->addAction(actNewDirRight_);
            }
        } else {
            // With selection on remote
            if (actCopyRight_)   rightContextMenu_->addAction(actCopyRight_);
            if (rightRemoteWritable_) {
                rightContextMenu_->addSeparator();
                if (actUploadRight_) rightContextMenu_->addAction(actUploadRight_);
                if (actNewFileRight_) rightContextMenu_->addAction(actNewFileRight_);
                if (actNewDirRight_)  rightContextMenu_->addAction(actNewDirRight_);
                if (actRenameRight_)   rightContextMenu_->addAction(actRenameRight_);
                if (actDeleteRight_)   rightContextMenu_->addAction(actDeleteRight_);
                if (actMoveRight_)     rightContextMenu_->addAction(actMoveRight_);
                rightContextMenu_->addSeparator();
                rightContextMenu_->addAction(tr("Cambiar permisos…"), this, &MainWindow::changeRemotePermissions);
            }
        }
    } else {
        // Local: Up option if applicable
        if (canGoUp && actUpRight_) rightContextMenu_->addAction(actUpRight_);
        if (!hasSel) {
            // No selection: creation
            if (actNewFileRight_) rightContextMenu_->addAction(actNewFileRight_);
            if (actNewDirRight_)  rightContextMenu_->addAction(actNewDirRight_);
        } else {
            // With selection: local operations + copy/move from left
            if (actNewFileRight_) rightContextMenu_->addAction(actNewFileRight_);
            if (actNewDirRight_)  rightContextMenu_->addAction(actNewDirRight_);
            if (actRenameRight_)   rightContextMenu_->addAction(actRenameRight_);
            if (actDeleteRight_)   rightContextMenu_->addAction(actDeleteRight_);
            rightContextMenu_->addSeparator();
            // Copy/move the selection from the right panel to the left
            if (actCopyRight_)     rightContextMenu_->addAction(actCopyRight_);
            if (actMoveRight_)     rightContextMenu_->addAction(actMoveRight_);
        }
    }
    rightContextMenu_->popup(rightView_->viewport()->mapToGlobal(pos));
}

// Show context menu for the left (local) pane.
void MainWindow::showLeftContextMenu(const QPoint& pos) {
    if (!leftContextMenu_) leftContextMenu_ = new QMenu(this);
    leftContextMenu_->clear();
    // Selection and ability to go up
    bool hasSel = false;
    if (auto sel = leftView_->selectionModel()) {
        hasSel = !sel->selectedRows(NAME_COL).isEmpty();
    }
    QDir d(leftPath_ ? leftPath_->text() : QString());
    bool canGoUp = d.cdUp();

    // Local actions on the left panel
    if (canGoUp && actUpLeft_)   leftContextMenu_->addAction(actUpLeft_);
    if (!hasSel) {
        if (actNewFileLeft_) leftContextMenu_->addAction(actNewFileLeft_);
        if (actNewDirLeft_)  leftContextMenu_->addAction(actNewDirLeft_);
    } else {
        if (actNewFileLeft_) leftContextMenu_->addAction(actNewFileLeft_);
        if (actNewDirLeft_)  leftContextMenu_->addAction(actNewDirLeft_);
        if (actRenameLeft_) leftContextMenu_->addAction(actRenameLeft_);
        leftContextMenu_->addSeparator();
        // Directional labels in the menu, wired to existing actions
        leftContextMenu_->addAction(tr("Copiar al panel derecho"), this, &MainWindow::copyLeftToRight);
        leftContextMenu_->addAction(tr("Mover al panel derecho"), this, &MainWindow::moveLeftToRight);
        if (actDelete_)   leftContextMenu_->addAction(actDelete_);
    }
    leftContextMenu_->popup(leftView_->viewport()->mapToGlobal(pos));
}

// Change permissions of the selected remote entry.
void MainWindow::changeRemotePermissions() {
    if (!rightIsRemote_ || !sftp_ || !rightRemoteModel_) return;
    auto sel = rightView_->selectionModel();
    if (!sel) return;
    const auto rows = sel->selectedRows();
    if (rows.size() != 1) { QMessageBox::information(this, tr("Permisos"), tr("Selecciona un solo elemento.")); return; }
    const QModelIndex idx = rows.first();
    const QString name = rightRemoteModel_->nameAt(idx);
    const QString base = rightRemoteModel_->rootPath();
    const QString path = joinRemotePath(base, name);
    openscp::FileInfo st{};
    std::string err;
    if (!sftp_->stat(path.toStdString(), st, err)) { QMessageBox::warning(this, tr("Permisos"), QString::fromStdString(err)); return; }
    PermissionsDialog dlg(this);
    dlg.setMode(st.mode & 0777);
    if (dlg.exec() != QDialog::Accepted) return;
    unsigned int newMode = (st.mode & ~0777u) | (dlg.mode() & 0777u);
    auto applyOne = [&](const QString& p) -> bool {
        std::string cerrs;
        if (!sftp_->chmod(p.toStdString(), newMode, cerrs)) { QMessageBox::critical(this, tr("Permisos"), QString::fromStdString(cerrs)); return false; }
        return true;
    };
    bool ok = true;
    if (dlg.recursive() && st.is_dir) {
        QVector<QString> stack;
        stack.push_back(path);
        while (!stack.isEmpty() && ok) {
            const QString cur = stack.back();
            stack.pop_back();
            if (!applyOne(cur)) { ok = false; break; }
            std::vector<openscp::FileInfo> out;
            std::string lerr;
            if (!sftp_->list(cur.toStdString(), out, lerr)) continue;
            for (const auto& e : out) {
                const QString child = joinRemotePath(cur, QString::fromStdString(e.name));
                if (e.is_dir) stack.push_back(child);
                else {
                    if (!applyOne(child)) { ok = false; break; }
                }
            }
        }
    } else {
        ok = applyOne(path);
    }
    if (!ok) return;
    QString dummy;
    rightRemoteModel_->setRootPath(base, &dummy);
    updateRemoteWriteability();
    statusBar()->showMessage(tr("Permisos actualizados"), 3000);
}

// Ask the user to confirm an unknown host key (TOFU).
bool MainWindow::confirmHostKeyUI(const QString& host,
                                  quint16 port,
                                  const QString& algorithm,
                                  const QString& fingerprint,
                                  bool canSave) {
    m_tofuHost_ = host + ":" + QString::number(port);
    m_tofuAlg_  = algorithm;
    m_tofuFp_   = fingerprint;
    m_tofuCanSave_ = canSave;
    {
        std::unique_lock<std::mutex> lk(m_tofuMutex_);
        m_tofuDecided_ = false;
        m_tofuAccepted_ = false;
    }
    QMetaObject::invokeMethod(this, [this, host, algorithm, fingerprint]{
        showTOfuDialog(host, algorithm, fingerprint);
    }, Qt::QueuedConnection);
    std::unique_lock<std::mutex> lk(m_tofuMutex_);
    m_tofuCv_.wait(lk, [&]{ return m_tofuDecided_; });
    return m_tofuAccepted_;
}

// Explicit non‑modal TOFU dialog per spec: open() + finished -> onTofuFinished
void MainWindow::showTOfuDialog(const QString& host, const QString& alg, const QString& fp) {
    if (m_tofuBox) { m_tofuBox->raise(); m_tofuBox->activateWindow(); return; }
    // If a connection progress dialog is visible, disable it so it does not capture input
    if (m_connectProgress_ && m_connectProgress_->isVisible()) {
        m_connectProgress_->setEnabled(false);
        m_connectProgressDimmed_ = true;
        std::fprintf(stderr, "[OpenSCP] TOFU shown; progress paused=true\n");
    } else {
        std::fprintf(stderr, "[OpenSCP] TOFU shown; progress paused=false\n");
    }
    auto* box = new QMessageBox(this);
    m_tofuBox = box;
    box->setAttribute(Qt::WA_DeleteOnClose, true);
    box->setWindowModality(Qt::WindowModal);
    box->setIcon(QMessageBox::Question);
    box->setWindowTitle(tr("Confirmar huella SSH"));
    QString text = QString(tr("Conectar a %1\nAlgoritmo: %2\nHuella: %3\n\n¿Confiar y guardar en known_hosts?"))
                      .arg(host)
                      .arg(alg)
                      .arg(fp);
    if (!m_tofuCanSave_) {
        text = QString(tr("Conectar a %1\nAlgoritmo: %2\nHuella: %3\n\nNo se podrá guardar la huella. Conexión sólo por esta vez."))
                   .arg(host)
                   .arg(alg)
                   .arg(fp);
    }
    box->setText(text);
    box->addButton(m_tofuCanSave_ ? tr("Confiar") : tr("Conectar sin guardar"), QMessageBox::YesRole);
    box->addButton(tr("Cancelar"), QMessageBox::RejectRole);
    connect(box, &QMessageBox::finished, this, &MainWindow::onTofuFinished);
    QTimer::singleShot(0, box, [this, box]{
        box->open();
        box->raise();
        box->activateWindow();
        box->setFocus(Qt::ActiveWindowFocusReason);
    });
}

void MainWindow::onTofuFinished(int r) {
    bool accept = (r == QDialog::Accepted || r == QMessageBox::Yes);
    if (m_tofuBox) {
        const auto* clicked = m_tofuBox->clickedButton();
        if (clicked) {
            auto role = m_tofuBox->buttonRole((QAbstractButton*)clicked);
            accept = (role == QMessageBox::YesRole || role == QMessageBox::AcceptRole);
        }
        m_tofuBox->deleteLater();
        m_tofuBox.clear();
    }
    if (!m_tofuCanSave_ && accept) {
        statusBar()->showMessage(tr("No se pudo guardar la huella, conexión permitida solo esta vez"), 5000);
    } else if (!accept) {
        statusBar()->showMessage(tr("Conexión cancelada: huella no aceptada"), 5000);
    }
    // Re-enable progress if it was dimmed
    if (m_connectProgressDimmed_ && m_connectProgress_) {
        m_connectProgress_->setEnabled(true);
        m_connectProgressDimmed_ = false;
        std::fprintf(stderr, "[OpenSCP] TOFU closed; progress resumed=true\n");
    } else {
        std::fprintf(stderr, "[OpenSCP] TOFU closed; progress resumed=false\n");
    }
    {
        std::unique_lock<std::mutex> lk(m_tofuMutex_);
        m_tofuAccepted_ = accept;
        m_tofuDecided_ = true;
    }
    m_tofuCv_.notify_one();
}

// Secondary non‑modal dialog for one‑time connection without saving
void MainWindow::showOneTimeDialog(const QString& host, const QString& alg, const QString& fp) {
    if (m_tofuBox) { m_tofuBox->raise(); m_tofuBox->activateWindow(); return; }
    auto* box = new QMessageBox(this);
    m_tofuBox = box;
    box->setAttribute(Qt::WA_DeleteOnClose, true);
    box->setWindowModality(Qt::WindowModal);
    box->setIcon(QMessageBox::Warning);
    box->setWindowTitle(tr("Confirmación adicional"));
    box->setText(QString(tr("No se pudo guardar la huella. ¿Conectar solo esta vez sin guardar?\n\nHost: %1\nAlgoritmo: %2\nHuella: %3"))
                    .arg(host, alg, fp));
    box->addButton(tr("Conectar sin guardar"), QMessageBox::YesRole);
    box->addButton(tr("Cancelar"), QMessageBox::RejectRole);
    connect(box, &QMessageBox::finished, this, &MainWindow::onOneTimeFinished);
    QTimer::singleShot(0, box, [box]{ box->open(); });
}

void MainWindow::onOneTimeFinished(int r) {
    bool accept = (r == QDialog::Accepted || r == QMessageBox::Yes);
    if (m_tofuBox) {
        const auto* clicked = m_tofuBox->clickedButton();
        if (clicked) {
            auto role = m_tofuBox->buttonRole((QAbstractButton*)clicked);
            accept = (role == QMessageBox::YesRole || role == QMessageBox::AcceptRole);
        }
        m_tofuBox->deleteLater();
        m_tofuBox.clear();
    }
    if (accept) statusBar()->showMessage(tr("Conexión sin guardar confirmada por el usuario"), 5000);
    else statusBar()->showMessage(tr("Conexión cancelada tras fallo de guardado"), 5000);
    {
        std::unique_lock<std::mutex> lk(m_tofuMutex_);
        m_tofuAccepted_ = accept;
        m_tofuDecided_ = true;
    }
    m_tofuCv_.notify_one();
}

// Intercept drag-and-drop and global events for panes and dialogs.
bool MainWindow::eventFilter(QObject* obj, QEvent* ev) {
    // Center QDialog/QMessageBox on show relative to the main window
    if (ev->type() == QEvent::Show) {
        if (auto* dlg = qobject_cast<QDialog*>(obj)) {
            // Avoid interfering with native-backed dialogs on macOS (QMessageBox/NSAlert, QProgressDialog, QInputDialog),
            // which can crash if sized/moved during Show handling.
            if (qobject_cast<QMessageBox*>(dlg) || qobject_cast<QProgressDialog*>(dlg) || qobject_cast<QInputDialog*>(dlg)) {
                // Let Qt/macOS handle their own placement
            } else {
    // Only center dialogs that belong (directly or indirectly) to this window
                QWidget* p = dlg->parentWidget();
                bool belongsToThis = false;
                while (p) {
                    if (p == this) { belongsToThis = true; break; }
                    p = p->parentWidget();
                }
                if (belongsToThis) {
                    // Expand to sizeHint without shrinking an explicit size
                    dlg->resize(dlg->size().expandedTo(dlg->sizeHint()));
                    QRect base = this->geometry();
                    if (!base.isValid()) {
                        if (this->screen()) base = this->screen()->availableGeometry();
                        else if (auto ps = QGuiApplication::primaryScreen()) base = ps->availableGeometry();
                    }
                    if (base.isValid()) {
                        const QRect aligned = QStyle::alignedRect(Qt::LeftToRight, Qt::AlignCenter, dlg->size(), base);
                        dlg->move(aligned.topLeft());
                    }
                }
            }
        }
    }

    // Drag-and-drop over the right panel (local or remote)
    if (rightView_ && obj == rightView_->viewport()) {
        if (ev->type() == QEvent::DragEnter) {
            auto* de = static_cast<QDragEnterEvent*>(ev);
            if (rightIsRemote_ && !rightRemoteWritable_) { de->ignore(); return true; }
            de->acceptProposedAction();
            return true;
        } else if (ev->type() == QEvent::DragMove) {
            auto* dm = static_cast<QDragMoveEvent*>(ev);
            if (rightIsRemote_ && !rightRemoteWritable_) { dm->ignore(); return true; }
            dm->acceptProposedAction();
            return true;
        } else if (ev->type() == QEvent::Drop) {
            auto* dd = static_cast<QDropEvent*>(ev);
            const auto urls = dd->mimeData() ? dd->mimeData()->urls() : QList<QUrl>{};
            if (urls.isEmpty()) { dd->acceptProposedAction(); return true; }
            if (rightIsRemote_) {
                // Block upload if remote is read-only
                if (!rightRemoteWritable_) {
                    statusBar()->showMessage(tr("Directorio remoto en solo lectura; no se puede subir aquí"), 5000);
                    dd->ignore();
                    return true;
                }
                // Upload to remote
                if (!sftp_ || !rightRemoteModel_) { dd->acceptProposedAction(); return true; }
                const QString remoteBase = rightRemoteModel_->rootPath();
                int enq = 0;
                for (const QUrl& u : urls) {
                    const QString p = u.toLocalFile();
                    if (p.isEmpty()) continue;
                    QFileInfo fi(p);
                    if (fi.isDir()) {
                        QDirIterator it(p, QDir::NoDotAndDotDot | QDir::AllEntries, QDirIterator::Subdirectories);
                        while (it.hasNext()) {
                            it.next();
                            if (!it.fileInfo().isFile()) continue;
                            const QString rel = QDir(p).relativeFilePath(it.filePath());
                            const QString rTarget = joinRemotePath(remoteBase, rel);
                            transferMgr_->enqueueUpload(it.filePath(), rTarget);
                            ++enq;
                        }
                    } else if (fi.isFile()) {
                        const QString rTarget = joinRemotePath(remoteBase, fi.fileName());
                        transferMgr_->enqueueUpload(fi.absoluteFilePath(), rTarget);
                        ++enq;
                    }
                }
    if (enq > 0) {
        statusBar()->showMessage(QString(tr("Encolados: %1 subidas (DND)")).arg(enq), 4000);
        if (!transferDlg_) transferDlg_ = new TransferQueueDialog(transferMgr_, this);
        transferDlg_->show(); transferDlg_->raise(); transferDlg_->activateWindow();
    }
                dd->acceptProposedAction();
                return true;
            } else {
                // Local copy to the right panel directory
                QDir dst(rightPath_->text());
                if (!dst.exists()) { dd->acceptProposedAction(); return true; }
                int ok = 0, fail = 0;
                QString lastError;
                for (const QUrl& u : urls) {
                    const QString p = u.toLocalFile();
                    if (p.isEmpty()) continue;
                    QFileInfo fi(p);
                    const QString target = dst.filePath(fi.fileName());
                    // Avoid copying onto itself if same directory/file
                    if (fi.absoluteFilePath() == target) {
                        continue;
                    }
                    QString err;
                    if (copyEntryRecursively(fi.absoluteFilePath(), target, err)) ++ok;
                    else { ++fail; lastError = err; }
                }
                QString m = QString(tr("Copiados: %1  |  Fallidos: %2")).arg(ok).arg(fail);
                if (fail > 0 && !lastError.isEmpty()) m += "\n" + tr("Último error: ") + lastError;
                statusBar()->showMessage(m, 5000);
                dd->acceptProposedAction();
                return true;
            }
        }
    }
    // Drag-and-drop over the left panel (local): copy/download
    // Update delete shortcut enablement if selection changes due to DnD or click
    if (leftView_ && obj == leftView_->viewport()) {
        if (ev->type() == QEvent::DragEnter) {
            auto* de = static_cast<QDragEnterEvent*>(ev);
            de->acceptProposedAction();
            return true;
        } else if (ev->type() == QEvent::DragMove) {
            auto* dm = static_cast<QDragMoveEvent*>(ev);
            dm->acceptProposedAction();
            return true;
        } else if (ev->type() == QEvent::Drop) {
            auto* dd = static_cast<QDropEvent*>(ev);
            const auto urls = dd->mimeData() ? dd->mimeData()->urls() : QList<QUrl>{};
            if (!urls.isEmpty()) {
                // Local copy towards the left panel
                QDir dst(leftPath_->text());
                if (!dst.exists()) { dd->acceptProposedAction(); return true; }
                int ok = 0, fail = 0;
                QString lastError;
                for (const QUrl& u : urls) {
                    const QString p = u.toLocalFile();
                    if (p.isEmpty()) continue;
                    QFileInfo fi(p);
                    const QString target = dst.filePath(fi.fileName());
                    // Avoid self-drop: same file/folder and same destination
                    if (fi.absoluteFilePath() == target) {
                        continue;
                    }
                    QString err;
                    if (copyEntryRecursively(fi.absoluteFilePath(), target, err)) ++ok;
                    else { ++fail; lastError = err; }
                }
                QString m = QString(tr("Copiados: %1  |  Fallidos: %2")).arg(ok).arg(fail);
                if (fail > 0 && !lastError.isEmpty()) m += "\n" + tr("Último error: ") + lastError;
                statusBar()->showMessage(m, 5000);
                dd->acceptProposedAction();
                updateDeleteShortcutEnables();
                return true;
            }
            // Download from remote (based on right panel selection)
            if (rightIsRemote_ == true && rightView_ && rightRemoteModel_) {
                auto sel = rightView_->selectionModel();
                if (!sel || sel->selectedRows(NAME_COL).isEmpty()) { dd->acceptProposedAction(); return true; }
                const auto rows = sel->selectedRows(NAME_COL);
                int enq = 0;
                int bad = 0;
                const QString remoteBase = rightRemoteModel_->rootPath();
                QDir dst(leftPath_->text());
                for (const QModelIndex& idx : rows) {
                    const QString name = rightRemoteModel_->nameAt(idx);
                    { QString why; if (!isValidEntryName(name, &why)) { ++bad; continue; } }
                    QString rpath = remoteBase;
                    if (!rpath.endsWith('/')) rpath += '/';
                    rpath += name;
                    const QString lpath = dst.filePath(name);
                    if (rightRemoteModel_->isDir(idx)) {
                        QVector<QPair<QString, QString>> stack;
                        stack.push_back({ rpath, lpath });
                        while (!stack.isEmpty()) {
                            auto pair = stack.back();
                            stack.pop_back();
                            const QString curR = pair.first;
                            const QString curL = pair.second;
                            QDir().mkpath(curL);
                            std::vector<openscp::FileInfo> out;
                            std::string lerr;
                            if (!sftp_->list(curR.toStdString(), out, lerr)) continue;
                            for (const auto& e : out) {
                                const QString ename = QString::fromStdString(e.name);
                                QString why; if (!isValidEntryName(ename, &why)) { ++bad; continue; }
                                const QString childR = (curR.endsWith('/') ? curR + ename : curR + "/" + ename);
                                const QString childL = QDir(curL).filePath(ename);
                                if (e.is_dir) stack.push_back({ childR, childL });
                                else { transferMgr_->enqueueDownload(childR, childL); ++enq; }
                            }
                        }
                    } else {
                        transferMgr_->enqueueDownload(rpath, lpath);
                        ++enq;
                    }
                }
                if (enq > 0) {
                    QString msg = QString(tr("Encolados: %1 descargas (DND)")).arg(enq);
                    if (bad > 0) msg += QString("  |  ") + tr("Omitidos inválidos: %1").arg(bad);
                    statusBar()->showMessage(msg, 4000);
                    if (!transferDlg_) transferDlg_ = new TransferQueueDialog(transferMgr_, this);
                    transferDlg_->show(); transferDlg_->raise(); transferDlg_->activateWindow();
                }
                dd->acceptProposedAction();
                updateDeleteShortcutEnables();
                return true;
            }
        }
    }
    return QMainWindow::eventFilter(obj, ev);
}

// Establish an SFTP connection asynchronously and wire UI callbacks.
bool MainWindow::establishSftpAsync(openscp::SessionOptions opt, std::string& err) {
    // Inject host key confirmation (TOFU) via UI
    opt.hostkey_confirm_cb = [this](const std::string& h, std::uint16_t p, const std::string& alg, const std::string& fp, bool canSave) {
        return confirmHostKeyUI(QString::fromStdString(h), (quint16)p,
                                QString::fromStdString(alg), QString::fromStdString(fp),
                                canSave);
    };
    opt.hostkey_status_cb = [this](const std::string& msg){
        const QString q = QString::fromStdString(msg);
        QMetaObject::invokeMethod(this, [this, q]{ statusBar()->showMessage(q, 5000); }, Qt::QueuedConnection);
    };

    // Keyboard-interactive callback (OTP/2FA). Prefer auto-filling password/username; request OTP if needed.
    const std::string savedUser = opt.username;
    const std::string savedPass = opt.password ? *opt.password : std::string();
    opt.keyboard_interactive_cb = [this, savedUser, savedPass](const std::string& name,
                                                               const std::string& instruction,
                                                               const std::vector<std::string>& prompts,
                                                               std::vector<std::string>& responses) -> bool {
        (void)name;
        responses.clear();
        responses.reserve(prompts.size());
        // Resolve each prompt: auto-fill user/pass and ask for OTP/codes if present
        for (const std::string& p : prompts) {
            QString qprompt = QString::fromStdString(p);
            QString lower = qprompt.toLower();
            // Username
            if (lower.contains("user") || lower.contains("name:")) {
                responses.emplace_back(savedUser);
                continue;
            }
            // Password
            if (lower.contains("password") || lower.contains("passphrase") || lower.contains("passcode")) {
                if (!savedPass.empty()) { responses.emplace_back(savedPass); continue; }
                // Ask for password if we did not have it
                QString ans;
                bool ok = false;
                QMetaObject::invokeMethod(this, [&] {
                    ans = QInputDialog::getText(this, tr("Contraseña requerida"), qprompt,
                                                QLineEdit::Password, QString(), &ok);
                }, Qt::BlockingQueuedConnection);
                if (!ok) return false;
                {
                    QByteArray bytes = ans.toUtf8();
                    responses.emplace_back(bytes.constData(), (size_t)bytes.size());
                    secureClear(bytes);
                }
                secureClear(ans);
                continue;
            }
            // OTP / Verification code / Token
            if (lower.contains("verification") || lower.contains("verify") || lower.contains("otp") || lower.contains("code") || lower.contains("token")) {
                QString title = tr("Código de verificación requerido");
                if (!instruction.empty()) title += " — " + QString::fromStdString(instruction);
                QString ans;
                bool ok = false;
                QMetaObject::invokeMethod(this, [&] {
                    ans = QInputDialog::getText(this, title, qprompt, QLineEdit::Password, QString(), &ok);
                }, Qt::BlockingQueuedConnection);
                if (!ok) return false;
                {
                    QByteArray bytes = ans.toUtf8();
                    responses.emplace_back(bytes.constData(), (size_t)bytes.size());
                    secureClear(bytes);
                }
                secureClear(ans);
                continue;
            }
            // Generic case: ask for text (not hidden)
            QString title = tr("Información requerida");
            if (!instruction.empty()) title += " — " + QString::fromStdString(instruction);
            QString ans;
            bool ok = false;
            QMetaObject::invokeMethod(this, [&] {
                ans = QInputDialog::getText(this, title, qprompt, QLineEdit::Normal, QString(), &ok);
            }, Qt::BlockingQueuedConnection);
            if (!ok) return false;
            {
                QByteArray bytes = ans.toUtf8();
                responses.emplace_back(bytes.constData(), (size_t)bytes.size());
                secureClear(bytes);
            }
            secureClear(ans);
        }
        return responses.size() == prompts.size();
    };

    QProgressDialog progress(tr("Conectando…"), QString(), 0, 0, this);
    // Make progress non-modal to avoid blocking TOFU confirmation
    progress.setWindowModality(Qt::NonModal);
    progress.setCancelButton(nullptr);
    progress.setMinimumDuration(0);
    progress.setAutoClose(false);
    progress.setAutoReset(false);
    progress.show();
    progress.raise();
    m_connectProgress_ = &progress;
    m_connectProgressDimmed_ = false;

    std::atomic<bool> done{ false };
    bool okConn = false;
    std::thread th([&] {
        try {
            auto tmp = std::make_unique<openscp::Libssh2SftpClient>();
            okConn = tmp->connect(opt, err);
            if (okConn) {
                QMetaObject::invokeMethod(this, [this, t = tmp.release()] { sftp_.reset(t); }, Qt::BlockingQueuedConnection);
            }
        } catch (const std::exception& ex) {
            err = std::string("Excepción en conexión: ") + ex.what();
            okConn = false;
        } catch (...) {
            err = "Excepción desconocida en conexión";
            okConn = false;
        }
        done = true;
    });
    while (!done) { qApp->processEvents(QEventLoop::AllEvents, 50); std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
    th.join();
    progress.close();
    m_connectProgress_.clear();
    return okConn;
}

// Switch UI into remote mode and wire models/actions for the right pane.
void MainWindow::applyRemoteConnectedUI(const openscp::SessionOptions& opt) {
    delete rightRemoteModel_;
    rightRemoteModel_ = new RemoteModel(sftp_.get(), this);
    rightRemoteModel_->setShowHidden(prefShowHidden_);
    QString e;
    if (!rightRemoteModel_->setRootPath("/", &e)) {
        QMessageBox::critical(this, "Error listando remoto", e);
        sftp_.reset();
        delete rightRemoteModel_;
        rightRemoteModel_ = nullptr;
        return;
    }
    rightView_->setModel(rightRemoteModel_);
    if (rightView_->selectionModel()) {
        connect(rightView_->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this]{ updateDeleteShortcutEnables(); });
    }
    rightView_->header()->setStretchLastSection(false);
    rightView_->setColumnWidth(0, 300);
    rightView_->setColumnWidth(1, 120);
    rightView_->setColumnWidth(2, 180);
    rightView_->setColumnWidth(3, 120);
    rightView_->setSortingEnabled(true);
    rightView_->sortByColumn(0, Qt::AscendingOrder);
    rightPath_->setText("/");
    rightIsRemote_ = true;
    if (transferMgr_) { transferMgr_->setClient(sftp_.get()); transferMgr_->setSessionOptions(opt); }
    if (actConnect_) actConnect_->setEnabled(false);
    if (actDisconnect_) actDisconnect_->setEnabled(true);
    if (actDownloadF7_) actDownloadF7_->setEnabled(true);
    if (actUploadRight_) actUploadRight_->setEnabled(true);
    if (actNewDirRight_)  actNewDirRight_->setEnabled(true);
    if (actNewFileRight_) actNewFileRight_->setEnabled(true);
    if (actRenameRight_)  actRenameRight_->setEnabled(true);
    if (actDeleteRight_)  actDeleteRight_->setEnabled(true);
    if (actChooseRight_) {
        actChooseRight_->setIcon(QIcon(QLatin1String(":/assets/icons/action-open-folder-remote.svg")));
        // Opening the system file explorer on a remote host is not supported cross‑platform.
        // Disable this action in remote mode to avoid confusion.
        actChooseRight_->setEnabled(false);
        actChooseRight_->setToolTip(tr("No disponible en remoto"));
    }
    statusBar()->showMessage(tr("Conectado (SFTP) a ") + QString::fromStdString(opt.host), 4000);
    setWindowTitle(tr("OpenSCP — local/remoto (SFTP)"));
    updateRemoteWriteability();
    updateDeleteShortcutEnables();
}

// Apply persisted user preferences to the UI.
void MainWindow::applyPreferences() {
    QSettings s("OpenSCP", "OpenSCP");
    const bool showHidden = s.value("UI/showHidden", false).toBool();
    const bool singleClick = s.value("UI/singleClick", false).toBool();
    prefOpenRevealInFolder_ = s.value("UI/openRevealInFolder", false).toBool();
    // Keep Site Manager auto-open preference up to date
    m_openSiteManagerOnDisconnect = s.value("UI/openSiteManagerOnDisconnect", true).toBool();

    // Local: model filters (hidden on/off)
    auto applyLocalFilters = [&](QFileSystemModel* m) {
        if (!m) return;
        QDir::Filters f = QDir::AllEntries | QDir::NoDotAndDotDot | QDir::AllDirs;
        if (showHidden) f = f | QDir::Hidden | QDir::System;
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
        if (leftClickConn_)  { QObject::disconnect(leftClickConn_);  leftClickConn_  = QMetaObject::Connection(); }
        if (rightClickConn_) { QObject::disconnect(rightClickConn_); rightClickConn_ = QMetaObject::Connection(); }
        if (singleClick) {
            if (leftView_)  leftClickConn_  = connect(leftView_,  &QTreeView::clicked,   this, &MainWindow::leftItemActivated);
            if (rightView_) rightClickConn_ = connect(rightView_, &QTreeView::clicked,  this, &MainWindow::rightItemActivated);
        }
        prefSingleClick_ = singleClick;
    }
}

// Check if the current remote directory is writable and update enables.
void MainWindow::updateRemoteWriteability() {
    // Determine if the current remote directory is writable by attempting to create and delete
    // a temporary folder. Conservative: if it fails, consider read-only.
    if (!rightIsRemote_ || !sftp_ || !rightRemoteModel_) {
        rightRemoteWritable_ = false;
        return;
    }
    const QString base = rightRemoteModel_->rootPath();
    const QString testName = ".openscp-write-test-" + QString::number(QDateTime::currentMSecsSinceEpoch());
    const QString testPath = base.endsWith('/') ? base + testName : base + "/" + testName;
    std::string err;
    bool created = sftp_->mkdir(testPath.toStdString(), err, 0755);
    if (created) {
        std::string derr;
        sftp_->removeDir(testPath.toStdString(), derr);
        rightRemoteWritable_ = true;
    } else {
        rightRemoteWritable_ = false;
    }
    // Reflect in actions that require write access
    if (actUploadRight_) actUploadRight_->setEnabled(rightRemoteWritable_);
    if (actNewDirRight_)  actNewDirRight_->setEnabled(rightRemoteWritable_);
    if (actNewFileRight_) actNewFileRight_->setEnabled(rightRemoteWritable_);
    if (actRenameRight_)  actRenameRight_->setEnabled(rightRemoteWritable_);
    if (actDeleteRight_)  actDeleteRight_->setEnabled(rightRemoteWritable_);
    if (actMoveRight_)    actMoveRight_->setEnabled(rightRemoteWritable_);
    if (actMoveRightTb_)  actMoveRightTb_->setEnabled(rightRemoteWritable_);
    updateDeleteShortcutEnables();
}
    // Enablement rules for buttons/shortcuts on both sub‑toolbars.
// - General: require a selection.
// - Exceptions: Up (if parent exists), Upload… (remote RW), Download (remote).
// Enable Delete shortcuts only when a selection is present in the corresponding pane.
void MainWindow::updateDeleteShortcutEnables() {
    auto hasColSel = [&](QTreeView* v) -> bool {
        if (!v || !v->selectionModel()) return false;
        return !v->selectionModel()->selectedRows(NAME_COL).isEmpty();
    };
    const bool leftHasSel = hasColSel(leftView_);
    const bool rightHasSel = hasColSel(rightView_);
    const bool rightWrite = (!rightIsRemote_) || (rightIsRemote_ && rightRemoteWritable_);

    // Left: enable according to selection (exception: Up)
    if (actCopyF5_)    actCopyF5_->setEnabled(leftHasSel);
    if (actMoveF6_)    actMoveF6_->setEnabled(leftHasSel);
    if (actDelete_)    actDelete_->setEnabled(leftHasSel);
    if (actRenameLeft_)  actRenameLeft_->setEnabled(leftHasSel);
    if (actNewDirLeft_)  actNewDirLeft_->setEnabled(true); // always enabled on local
    if (actNewFileLeft_) actNewFileLeft_->setEnabled(true); // always enabled on local
    if (actUpLeft_) {
        QDir d(leftPath_ ? leftPath_->text() : QString());
        bool canUp = d.cdUp();
        actUpLeft_->setEnabled(canUp);
    }

    // Right: enable according to selection + permissions (exceptions: Up, Upload, Download)
    if (actCopyRightTb_) actCopyRightTb_->setEnabled(rightHasSel);
    if (actMoveRightTb_) actMoveRightTb_->setEnabled(rightHasSel && rightWrite);
    if (actDeleteRight_) actDeleteRight_->setEnabled(rightHasSel && rightWrite);
    if (actRenameRight_)  actRenameRight_->setEnabled(rightHasSel && rightWrite);
    if (actNewDirRight_)  actNewDirRight_->setEnabled(rightWrite); // enabled if local or remote is writable
    if (actNewFileRight_) actNewFileRight_->setEnabled(rightWrite);
    if (actUploadRight_) actUploadRight_->setEnabled(rightIsRemote_ && rightRemoteWritable_); // exception
    if (actDownloadF7_) actDownloadF7_->setEnabled(rightIsRemote_); // exception: enabled without selection
    if (actUpRight_) {
        QString cur = rightRemoteModel_ ? rightRemoteModel_->rootPath() : rightPath_->text();
        if (rightIsRemote_) {
            if (cur.endsWith('/')) cur.chop(1);
            actUpRight_->setEnabled(!cur.isEmpty() && cur != "/");
        } else {
            QDir d(cur);
            actUpRight_->setEnabled(d.cdUp());
        }
    }
}
