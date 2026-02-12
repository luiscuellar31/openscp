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
#include <QUuid>
#include <QShortcut>
#include <QToolButton>
#include <QProcess>
#include <cstring>
#include "DragAwareTreeView.hpp"
#include <atomic>
#include <thread>
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

static QString shortRemotePermissionError(const std::string& raw, const QString& fallback) {
    QString msg = QString::fromStdString(raw).trimmed();
    if (msg.isEmpty()) return fallback;

    const QString lower = msg.toLower();
    if (lower.contains("permission denied") || lower.contains("permiso denegado")) {
        return QCoreApplication::translate("MainWindow", "Permiso denegado.");
    }
    if (lower.contains("read-only") || lower.contains("solo lectura")) {
        return QCoreApplication::translate("MainWindow", "Ubicación en modo solo lectura.");
    }

    const int nl = msg.indexOf('\n');
    if (nl > 0) msg = msg.left(nl);
    msg = msg.simplified();
    if (msg.size() > 96) msg = msg.left(93) + "...";
    return msg;
}

static QString newQuickSiteId() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

static QString normalizedIdentityHost(const std::string& host) {
    return QString::fromStdString(host).trimmed().toLower();
}

static QString normalizedIdentityUser(const std::string& user) {
    return QString::fromStdString(user).trimmed();
}

static QString normalizedIdentityKeyPath(const std::optional<std::string>& keyPath) {
    if (!keyPath || keyPath->empty()) return {};
    return QDir::cleanPath(QDir::fromNativeSeparators(QString::fromStdString(*keyPath).trimmed()));
}

static bool sameSavedSiteIdentity(const openscp::SessionOptions& a, const openscp::SessionOptions& b) {
    return normalizedIdentityHost(a.host) == normalizedIdentityHost(b.host) &&
           a.port == b.port &&
           normalizedIdentityUser(a.username) == normalizedIdentityUser(b.username) &&
           normalizedIdentityKeyPath(a.private_key_path) == normalizedIdentityKeyPath(b.private_key_path);
}

static QString quickSiteSecretKey(const SiteEntry& e, const QString& item) {
    if (!e.id.isEmpty()) return QString("site-id:%1:%2").arg(e.id, item);
    return QString("site:%1:%2").arg(e.name, item);
}

static QVector<SiteEntry> loadSavedSitesForQuickConnect(bool* needsSave) {
    QVector<SiteEntry> sites;
    bool shouldSave = false;
    QSettings s("OpenSCP", "OpenSCP");
    const int n = s.beginReadArray("sites");
    QSet<QString> usedIds;
    for (int i = 0; i < n; ++i) {
        s.setArrayIndex(i);
        SiteEntry e;
        e.id = s.value("id").toString().trimmed();
        if (e.id.isEmpty() || usedIds.contains(e.id)) {
            e.id = newQuickSiteId();
            shouldSave = true;
        }
        usedIds.insert(e.id);
        e.name = s.value("name").toString().trimmed();
        e.opt.host = s.value("host").toString().toStdString();
        e.opt.port = static_cast<std::uint16_t>(s.value("port", 22).toUInt());
        e.opt.username = s.value("user").toString().toStdString();
        const QString kp = s.value("keyPath").toString();
        if (!kp.isEmpty()) e.opt.private_key_path = kp.toStdString();
        const QString kh = s.value("knownHosts").toString();
        if (!kh.isEmpty()) e.opt.known_hosts_path = kh.toStdString();
        e.opt.known_hosts_policy = static_cast<openscp::KnownHostsPolicy>(
            s.value("khPolicy", static_cast<int>(openscp::KnownHostsPolicy::Strict)).toInt()
        );
        sites.push_back(e);
    }
    s.endArray();
    if (needsSave) *needsSave = shouldSave;
    return sites;
}

static void saveSavedSitesForQuickConnect(const QVector<SiteEntry>& sites) {
    QSettings s("OpenSCP", "OpenSCP");
    s.remove("sites");
    s.beginWriteArray("sites");
    for (int i = 0; i < sites.size(); ++i) {
        s.setArrayIndex(i);
        const SiteEntry& e = sites[i];
        s.setValue("id", e.id);
        s.setValue("name", e.name);
        s.setValue("host", QString::fromStdString(e.opt.host));
        s.setValue("port", static_cast<int>(e.opt.port));
        s.setValue("user", QString::fromStdString(e.opt.username));
        s.setValue("keyPath", e.opt.private_key_path ? QString::fromStdString(*e.opt.private_key_path) : QString());
        s.setValue("knownHosts", e.opt.known_hosts_path ? QString::fromStdString(*e.opt.known_hosts_path) : QString());
        s.setValue("khPolicy", static_cast<int>(e.opt.known_hosts_policy));
    }
    s.endArray();
    s.sync();
}

static QString defaultQuickSiteName(const openscp::SessionOptions& opt) {
    const QString user = normalizedIdentityUser(opt.username);
    const QString host = normalizedIdentityHost(opt.host);
    QString out;
    if (!user.isEmpty() && !host.isEmpty()) out = QString("%1@%2").arg(user, host);
    else if (!host.isEmpty()) out = host;
    else if (!user.isEmpty()) out = user;
    else out = QObject::tr("Nuevo sitio");
    if (!host.isEmpty() && opt.port != 22) out += QString(":%1").arg(opt.port);
    return out;
}

static QString ensureUniqueQuickSiteName(const QVector<SiteEntry>& sites, const QString& preferred) {
    QString base = preferred.trimmed();
    if (base.isEmpty()) base = QObject::tr("Nuevo sitio");
    auto exists = [&](const QString& candidate) {
        for (const auto& s : sites) {
            if (s.name.compare(candidate, Qt::CaseInsensitive) == 0) return true;
        }
        return false;
    };
    if (!exists(base)) return base;
    for (int i = 2; i < 10000; ++i) {
        const QString candidate = QString("%1 (%2)").arg(base).arg(i);
        if (!exists(candidate)) return candidate;
    }
    return base + QString(" (%1)").arg(QUuid::createUuid().toString(QUuid::WithoutBraces).left(6));
}

static QString quickPersistStatusShort(SecretStore::PersistStatus st) {
    switch (st) {
        case SecretStore::PersistStatus::Stored: return QObject::tr("guardado");
        case SecretStore::PersistStatus::Unavailable: return QObject::tr("no disponible");
        case SecretStore::PersistStatus::PermissionDenied: return QObject::tr("permiso denegado");
        case SecretStore::PersistStatus::BackendError: return QObject::tr("error del backend");
    }
    return QObject::tr("error");
}

static void refreshOpenSiteManagerWidget(QPointer<QWidget> siteManager) {
    if (!siteManager) return;
    auto* dlg = qobject_cast<SiteManagerDialog*>(siteManager.data());
    if (!dlg) return;
    dlg->reloadFromSettings();
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
                startSftpConnect(opt);
            }
        }
    });
    actSites_->setIcon(resIcon("action-open-saved-sites.svg"));
    actSites_->setToolTip(actSites_->text());
    tb->addSeparator();
    actShowQueue_ = tb->addAction(tr("Transferencias"), [this] {
        showTransferQueue();
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

    {
        QSettings s("OpenSCP", "OpenSCP");
        downloadDir_ = defaultDownloadDirFromSettings(s);
    }
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
            maybeShowTransferQueue();
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
        const QString remoteBase = rightRemoteModel_->rootPath();
        struct UploadPair {
            QString localPath;
            QString remotePath;
            QString topLocalDir; // non-empty only for files that belong to a moved directory
        };
        QVector<UploadPair> pairs;
        int skippedPrep = 0;
        int movedEmptyDirs = 0;
        QString prepError;

        auto ensureRemoteDir = [&](const QString& dir) -> bool {
            if (dir.isEmpty()) return true;
            QString cur = "/";
            const QStringList parts = dir.split('/', Qt::SkipEmptyParts);
            for (const QString& part : parts) {
                const QString next = (cur == "/") ? ("/" + part) : (cur + "/" + part);
                bool isD = false;
                std::string e;
                const bool ex = sftp_->exists(next.toStdString(), isD, e);
                if (!e.empty()) {
                    prepError = QString::fromStdString(e);
                    return false;
                }
                if (!ex) {
                    std::string me;
                    if (!sftp_->mkdir(next.toStdString(), me, 0755)) {
                        prepError = QString::fromStdString(me);
                        return false;
                    }
                }
                cur = next;
            }
            return true;
        };

        for (const QModelIndex& idx : rows) {
            const QFileInfo fi = leftModel_->fileInfo(idx);
            if (fi.isDir()) {
                const QString topLocalDir = fi.absoluteFilePath();
                const QString baseRemoteDir = joinRemotePath(remoteBase, fi.fileName());
                if (!ensureRemoteDir(baseRemoteDir)) { ++skippedPrep; continue; }
                const int pairStart = pairs.size();
                bool dirPrepFailed = false;
                int filesInDir = 0;
                QDirIterator it(topLocalDir, QDir::NoDotAndDotDot | QDir::AllEntries, QDirIterator::Subdirectories);
                while (it.hasNext()) {
                    it.next();
                    const QFileInfo sfi = it.fileInfo();
                    const QString rel = QDir(topLocalDir).relativeFilePath(sfi.absoluteFilePath());
                    const QString rTarget = joinRemotePath(baseRemoteDir, rel);
                    if (sfi.isDir()) {
                        if (!ensureRemoteDir(rTarget)) { dirPrepFailed = true; break; }
                        continue;
                    }
                    if (!sfi.isFile()) continue;
                    pairs.push_back({ sfi.absoluteFilePath(), rTarget, topLocalDir });
                    ++filesInDir;
                }
                if (dirPrepFailed) {
                    pairs.resize(pairStart);
                    ++skippedPrep;
                    continue;
                }
                if (filesInDir == 0) {
                    if (QDir(topLocalDir).removeRecursively()) ++movedEmptyDirs;
                    else { ++skippedPrep; prepError = tr("No se pudo borrar origen: ") + topLocalDir; }
                }
            } else if (fi.isFile()) {
                const QString rTarget = joinRemotePath(remoteBase, fi.fileName());
                pairs.push_back({ fi.absoluteFilePath(), rTarget, QString() });
            }
        }

        for (const auto& p : pairs) transferMgr_->enqueueUpload(p.localPath, p.remotePath);
        if (!pairs.isEmpty()) {
            statusBar()->showMessage(QString(tr("Encolados: %1 subidas (mover)")).arg(pairs.size()), 4000);
            maybeShowTransferQueue();
        } else if (movedEmptyDirs > 0) {
            statusBar()->showMessage(QString(tr("Movidos OK: %1 (carpetas vacías)")).arg(movedEmptyDirs), 4000);
        } else if (skippedPrep > 0) {
            QString msg = QString(tr("No se pudieron preparar elementos para mover: %1")).arg(skippedPrep);
            if (!prepError.isEmpty()) msg += "\n" + tr("Último error: ") + prepError;
            statusBar()->showMessage(msg, 5000);
        }

        // Local cleanup on successful upload, without blocking UI.
        if (!pairs.isEmpty()) {
            struct MoveUploadState {
                QSet<QString> pendingLocalFiles;
                QSet<QString> processedLocalFiles;
                QHash<QString, QString> fileToTopDir;
                QHash<QString, int> remainingInTopDir;
                QSet<QString> failedTopDirs;
                int movedOk = 0;
                int failed = 0;
                int skipped = 0;
                QString lastError;
            };
            auto state = std::make_shared<MoveUploadState>();
            state->movedOk = movedEmptyDirs;
            state->skipped = skippedPrep;
            state->lastError = prepError;

            for (const auto& p : pairs) {
                state->pendingLocalFiles.insert(p.localPath);
                if (!p.topLocalDir.isEmpty()) {
                    state->fileToTopDir.insert(p.localPath, p.topLocalDir);
                    state->remainingInTopDir[p.topLocalDir] = state->remainingInTopDir.value(p.topLocalDir) + 1;
                }
            }

            auto connPtr = std::make_shared<QMetaObject::Connection>();
            *connPtr = connect(transferMgr_, &TransferManager::tasksChanged, this, [this, state, remoteBase, connPtr, pairs]() {
                const auto tasks = transferMgr_->tasksSnapshot();

                for (const auto& t : tasks) {
                    if (t.type != TransferTask::Type::Upload) continue;
                    const QString local = t.src;
                    if (!state->pendingLocalFiles.contains(local)) continue;
                    if (state->processedLocalFiles.contains(local)) continue;
                    if (t.status != TransferTask::Status::Done &&
                        t.status != TransferTask::Status::Error &&
                        t.status != TransferTask::Status::Canceled) {
                        continue;
                    }

                    state->processedLocalFiles.insert(local);
                    const QString topDir = state->fileToTopDir.value(local);
                    const bool uploadDone = (t.status == TransferTask::Status::Done);
                    if (uploadDone) {
                        const bool removed = !QFileInfo::exists(local) || QFile::remove(local);
                        if (removed) {
                            ++state->movedOk;
                        } else {
                            ++state->failed;
                            state->lastError = tr("No se pudo borrar origen: ") + local;
                            if (!topDir.isEmpty()) state->failedTopDirs.insert(topDir);
                        }
                    } else {
                        ++state->failed;
                        if (!t.error.isEmpty()) state->lastError = t.error;
                        if (!topDir.isEmpty()) state->failedTopDirs.insert(topDir);
                    }

                    if (!topDir.isEmpty()) {
                        const int rem = qMax(0, state->remainingInTopDir.value(topDir) - 1);
                        state->remainingInTopDir[topDir] = rem;
                        if (rem == 0 && !state->failedTopDirs.contains(topDir) && QDir(topDir).exists()) {
                            if (!QDir(topDir).removeRecursively()) {
                                ++state->failed;
                                state->lastError = tr("No se pudo borrar origen: ") + topDir;
                            }
                        }
                    }
                }

                bool allFinal = true;
                for (const auto& p : pairs) {
                    bool found = false;
                    bool final = false;
                    for (const auto& t : tasks) {
                        if (t.type == TransferTask::Type::Upload && t.src == p.localPath && t.dst == p.remotePath) {
                            found = true;
                            if (t.status == TransferTask::Status::Done ||
                                t.status == TransferTask::Status::Error ||
                                t.status == TransferTask::Status::Canceled) {
                                final = true;
                            }
                            break;
                        }
                    }
                    if (!found || !final) { allFinal = false; break; }
                }

                if (allFinal) {
                    QString msg = QString(tr("Movidos OK: %1  |  Fallidos: %2  |  Omitidos: %3"))
                                      .arg(state->movedOk)
                                      .arg(state->failed)
                                      .arg(state->skipped);
                    if (state->failed > 0 && !state->lastError.isEmpty()) {
                        msg += "\n" + tr("Último error: ") + state->lastError;
                    }
                    statusBar()->showMessage(msg, 6000);
                    setLeftRoot(leftPath_->text());
                    QString dummy;
                    if (rightRemoteModel_) rightRemoteModel_->setRootPath(remoteBase, &dummy);
                    updateRemoteWriteability();
                    QObject::disconnect(*connPtr);
                }
            });
        }
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
    dlg.setQuickConnectSaveOptionsVisible(true);
    if (dlg.exec() != QDialog::Accepted) return;
    auto opt = dlg.options();
    std::optional<PendingSiteSaveRequest> saveRequest = std::nullopt;
    if (dlg.saveSiteRequested()) {
        PendingSiteSaveRequest req;
        req.siteName = dlg.siteName();
        req.saveCredentials = dlg.saveCredentialsRequested();
        saveRequest = req;
    }
    // Apply global security preferences also for ad‑hoc connections (Advanced settings)
    {
        QSettings s("OpenSCP", "OpenSCP");
        opt.known_hosts_hash_names = s.value("Security/knownHostsHashed", true).toBool();
        opt.show_fp_hex = s.value("Security/fpHex", false).toBool();
    }
    if (saveRequest.has_value()) {
        maybePersistQuickConnectSite(opt, *saveRequest, false);
        // Already persisted on request; connection lifecycle no longer needs to do it.
        saveRequest.reset();
    }
    startSftpConnect(opt, saveRequest);
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
    m_sessionNoHostVerification_ = false;
    updateHostPolicyRiskBanner();
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
                startSftpConnect(opt);
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

bool MainWindow::confirmInsecureHostPolicyForSession(const openscp::SessionOptions& opt) {
    if (opt.known_hosts_policy != openscp::KnownHostsPolicy::Off) return true;

    const QString hostKey = QString::fromStdString(opt.host).trimmed().toLower();
    const QString allowKey = QString("Security/noHostVerificationConfirmedUntilUtc/%1:%2")
                                 .arg(hostKey)
                                 .arg((int)opt.port);
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    QSettings s("OpenSCP", "OpenSCP");
    const qint64 allowedUntil = s.value(allowKey, 0).toLongLong();
    if (allowedUntil > now) return true;

    const auto first = QMessageBox::warning(
        this,
        tr("Riesgo crítico de seguridad"),
        tr("Estás a punto de conectar con la política \"Sin verificación\".\n"
           "Esto permite ataques MITM y suplantación del servidor.\n\n"
           "¿Deseas continuar bajo tu responsabilidad?"),
        QMessageBox::Yes | QMessageBox::Cancel,
        QMessageBox::Cancel
    );
    if (first != QMessageBox::Yes) return false;

    const QString token = QStringLiteral("UNSAFE");
    bool ok = false;
    const QString entered = QInputDialog::getText(
        this,
        tr("Confirmación adicional requerida"),
        tr("Para confirmar, escribe exactamente %1").arg(token),
        QLineEdit::Normal,
        QString(),
        &ok
    ).trimmed();
    if (!ok || entered != token) {
        QMessageBox::information(this, tr("Conexión cancelada"), tr("No se confirmó el riesgo de forma válida."));
        return false;
    }

    // Temporary exception per host:port to avoid persistent bypasses.
    const int ttlMin = qBound(1, prefNoHostVerificationTtlMin_, 120);
    const qint64 newUntil = now + qint64(ttlMin) * 60;
    s.setValue(allowKey, newUntil);
    s.sync();
    const QDateTime expLocal = QDateTime::fromSecsSinceEpoch(newUntil).toLocalTime();
    statusBar()->showMessage(
        tr("Excepción temporal de \"sin verificación\" activa hasta %1")
            .arg(QLocale().toString(expLocal, QLocale::ShortFormat)),
        8000
    );
    return true;
}

void MainWindow::updateHostPolicyRiskBanner() {
    const bool show = rightIsRemote_ && m_sessionNoHostVerification_;
    if (!show) {
        if (m_hostPolicyRiskLabel_) m_hostPolicyRiskLabel_->hide();
        return;
    }
    if (!m_hostPolicyRiskLabel_) {
        m_hostPolicyRiskLabel_ = new QLabel(this);
        m_hostPolicyRiskLabel_->setStyleSheet("QLabel { color: #B00020; font-weight: 600; }");
        statusBar()->addPermanentWidget(m_hostPolicyRiskLabel_);
    }
    m_hostPolicyRiskLabel_->setText(tr("Riesgo: host key sin verificación en esta sesión"));
    m_hostPolicyRiskLabel_->setToolTip(tr("La sesión actual no valida host key; existe riesgo de MITM."));
    m_hostPolicyRiskLabel_->show();
}

QString MainWindow::defaultDownloadDirFromSettings(const QSettings& s) {
    QString fallback = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (fallback.isEmpty()) fallback = QDir::homePath() + "/Downloads";
    QString configured = QDir::cleanPath(s.value("UI/defaultDownloadDir", fallback).toString().trimmed());
    if (configured.isEmpty()) configured = fallback;
    return configured;
}

void MainWindow::showTransferQueue() {
    if (!transferDlg_) transferDlg_ = new TransferQueueDialog(transferMgr_, this);
    transferDlg_->show();
    transferDlg_->raise();
    transferDlg_->activateWindow();
}

void MainWindow::maybeShowTransferQueue() {
    if (prefShowQueueOnEnqueue_) showTransferQueue();
}

void MainWindow::openLocalPathWithPreference(const QString& localPath) {
    const QString mode = prefOpenBehaviorMode_.trimmed().toLower();
    if (mode == QStringLiteral("reveal")) {
        revealInFolder(localPath);
        return;
    }
    if (mode == QStringLiteral("open")) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(localPath));
        return;
    }

    QMessageBox box(this);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(tr("Preferencia de apertura"));
    box.setText(tr("¿Cómo deseas abrir este archivo?"));
    QPushButton* btnOpen = box.addButton(tr("Abrir archivo"), QMessageBox::NoRole);
    QPushButton* btnReveal = box.addButton(tr("Mostrar carpeta"), QMessageBox::AcceptRole);
    box.setDefaultButton(btnReveal);
    box.exec();
    if (box.clickedButton() == btnOpen) QDesktopServices::openUrl(QUrl::fromLocalFile(localPath));
    else revealInFolder(localPath);
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
            openLocalPathWithPreference(fi.absoluteFilePath());
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
        const auto tasks = transferMgr_->tasksSnapshot();
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
        maybeShowTransferQueue();
    } else {
        // There was already an identical task in the queue; optionally show it
        maybeShowTransferQueue();
        statusBar()->showMessage(tr("Descarga ya encolada"), 2000);
    }
    // Open the file when the corresponding task finishes (avoid duplicate listeners)
    static QSet<QString> sOpenListeners;
    const QString key = remotePath + "->" + localPath;
    if (!sOpenListeners.contains(key)) {
        sOpenListeners.insert(key);
        auto connPtr = std::make_shared<QMetaObject::Connection>();
        *connPtr = connect(transferMgr_, &TransferManager::tasksChanged, this, [this, remotePath, localPath, key, connPtr]() {
            const auto tasks = transferMgr_->tasksSnapshot();
            for (const auto& t : tasks) {
                if (t.type == TransferTask::Type::Download && t.src == remotePath && t.dst == localPath) {
                    if (t.status == TransferTask::Status::Done) {
                        openLocalPathWithPreference(localPath);
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
        openLocalPathWithPreference(fi.absoluteFilePath());
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
        maybeShowTransferQueue();
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
        maybeShowTransferQueue();
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
        maybeShowTransferQueue();
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
            const auto tasks = transferMgr_->tasksSnapshot();
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
        maybeShowTransferQueue();
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
    if (rows.size() != 1) { QMessageBox::information(this, tr("Permisos"), tr("Selecciona solo un elemento.")); return; }
    const QModelIndex idx = rows.first();
    const QString name = rightRemoteModel_->nameAt(idx);
    const QString base = rightRemoteModel_->rootPath();
    const QString path = joinRemotePath(base, name);
    openscp::FileInfo st{};
    std::string err;
    if (!sftp_->stat(path.toStdString(), st, err)) {
        QMessageBox::warning(
            this,
            tr("Permisos"),
            tr("No se pudieron leer los permisos.\n%1")
                .arg(shortRemotePermissionError(err, tr("Error al leer información remota.")))
        );
        return;
    }
    PermissionsDialog dlg(this);
    dlg.setMode(st.mode & 0777);
    if (dlg.exec() != QDialog::Accepted) return;
    unsigned int newMode = (st.mode & ~0777u) | (dlg.mode() & 0777u);
    auto applyOne = [&](const QString& p) -> bool {
        std::string cerrs;
        if (!sftp_->chmod(p.toStdString(), newMode, cerrs)) {
            const QString item = QFileInfo(p).fileName().isEmpty() ? p : QFileInfo(p).fileName();
            QMessageBox::critical(
                this,
                tr("Permisos"),
                tr("No se pudieron aplicar permisos en \"%1\".\n%2")
                    .arg(item, shortRemotePermissionError(cerrs, tr("Error al aplicar cambios.")))
            );
            return false;
        }
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
        maybeShowTransferQueue();
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
                    maybeShowTransferQueue();
                }
                dd->acceptProposedAction();
                updateDeleteShortcutEnables();
                return true;
            }
        }
    }
    return QMainWindow::eventFilter(obj, ev);
}

// Start an SFTP connection in a background thread and finalize in the UI thread.
void MainWindow::startSftpConnect(openscp::SessionOptions opt,
                                  std::optional<PendingSiteSaveRequest> saveRequest) {
    if (m_connectInProgress_) {
        statusBar()->showMessage(tr("Ya hay una conexión en progreso"), 3000);
        return;
    }
    if (rightIsRemote_ || sftp_) {
        statusBar()->showMessage(tr("Ya existe una sesión SFTP activa"), 3000);
        return;
    }
    if (!confirmInsecureHostPolicyForSession(opt)) {
        statusBar()->showMessage(tr("Conexión cancelada: política sin verificación no confirmada"), 5000);
        return;
    }

    const openscp::SessionOptions uiOpt = opt;
    QPointer<MainWindow> self(this);
    auto cancelFlag = std::make_shared<std::atomic<bool>>(false);
    m_connectCancelRequested_ = cancelFlag;
    m_connectInProgress_ = true;

    if (actConnect_) actConnect_->setEnabled(false);
    if (actSites_) actSites_->setEnabled(false);

    auto* progress = new QProgressDialog(tr("Conectando…"), tr("Cancelar"), 0, 0, this);
    progress->setWindowModality(Qt::NonModal);
    progress->setMinimumDuration(0);
    progress->setAutoClose(false);
    progress->setAutoReset(false);
    connect(progress, &QProgressDialog::canceled, this, [this] {
        if (m_connectCancelRequested_) {
            m_connectCancelRequested_->store(true);
            statusBar()->showMessage(tr("Cancelando conexión…"), 3000);
        }
    });
    progress->show();
    progress->raise();
    m_connectProgress_ = progress;
    m_connectProgressDimmed_ = false;

    // Inject host key confirmation (TOFU) via UI
    opt.hostkey_confirm_cb = [self](const std::string& h, std::uint16_t p, const std::string& alg, const std::string& fp, bool canSave) {
        if (!self) return false;
        return self->confirmHostKeyUI(QString::fromStdString(h), (quint16)p,
                                      QString::fromStdString(alg), QString::fromStdString(fp),
                                      canSave);
    };
    opt.hostkey_status_cb = [self](const std::string& msg){
        if (!self) return;
        const QString q = QString::fromStdString(msg);
        QMetaObject::invokeMethod(self, [self, q]{
            if (self) self->statusBar()->showMessage(q, 5000);
        }, Qt::QueuedConnection);
    };

    // Keyboard-interactive callback (OTP/2FA). Prefer auto-filling password/username; request OTP if needed.
    const std::string savedUser = opt.username;
    const std::string savedPass = opt.password ? *opt.password : std::string();
    opt.keyboard_interactive_cb = [self, savedUser, savedPass](const std::string& name,
                                                               const std::string& instruction,
                                                               const std::vector<std::string>& prompts,
                                                               std::vector<std::string>& responses) -> openscp::KbdIntPromptResult {
        (void)name;
        if (!self) return openscp::KbdIntPromptResult::Cancelled;
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
                QMetaObject::invokeMethod(self, [&] {
                    if (!self) return;
                    ans = QInputDialog::getText(self, tr("Contraseña requerida"), qprompt,
                                                QLineEdit::Password, QString(), &ok);
                }, Qt::BlockingQueuedConnection);
                if (!ok) return openscp::KbdIntPromptResult::Cancelled;
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
                QMetaObject::invokeMethod(self, [&] {
                    if (!self) return;
                    ans = QInputDialog::getText(self, title, qprompt, QLineEdit::Password, QString(), &ok);
                }, Qt::BlockingQueuedConnection);
                if (!ok) return openscp::KbdIntPromptResult::Cancelled;
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
            QMetaObject::invokeMethod(self, [&] {
                if (!self) return;
                ans = QInputDialog::getText(self, title, qprompt, QLineEdit::Normal, QString(), &ok);
            }, Qt::BlockingQueuedConnection);
            if (!ok) return openscp::KbdIntPromptResult::Cancelled;
            {
                QByteArray bytes = ans.toUtf8();
                responses.emplace_back(bytes.constData(), (size_t)bytes.size());
                secureClear(bytes);
            }
            secureClear(ans);
        }
        return (responses.size() == prompts.size())
            ? openscp::KbdIntPromptResult::Handled
            : openscp::KbdIntPromptResult::Unhandled;
    };

    std::thread([self, opt = std::move(opt), uiOpt, saveRequest, cancelFlag]() mutable {
        bool okConn = false;
        bool canceledByUser = false;
        std::string err;
        openscp::SftpClient* connectedClient = nullptr;
        try {
            if (cancelFlag && cancelFlag->load()) {
                canceledByUser = true;
                err = "Conexión cancelada por el usuario";
            } else {
                auto tmp = std::make_unique<openscp::Libssh2SftpClient>();
                okConn = tmp->connect(opt, err);
                if (cancelFlag && cancelFlag->load()) {
                    canceledByUser = true;
                    if (okConn) tmp->disconnect();
                    okConn = false;
                    if (err.empty()) err = "Conexión cancelada por el usuario";
                }
                if (okConn) connectedClient = tmp.release();
            }
        } catch (const std::exception& ex) {
            err = std::string("Excepción en conexión: ") + ex.what();
            okConn = false;
        } catch (...) {
            err = "Excepción desconocida en conexión";
            okConn = false;
        }

        const QString qerr = QString::fromStdString(err);
        const bool queued = QMetaObject::invokeMethod(
            qApp,
            [self, okConn, qerr, connectedClient, uiOpt, saveRequest, canceledByUser]() {
                if (!self) {
                    if (connectedClient) {
                        connectedClient->disconnect();
                        delete connectedClient;
                    }
                    return;
                }
                self->finalizeSftpConnect(okConn, qerr, connectedClient, uiOpt, saveRequest, canceledByUser);
            },
            Qt::QueuedConnection
        );
        if (!queued && connectedClient) {
            connectedClient->disconnect();
            delete connectedClient;
        }
    }).detach();
}

void MainWindow::finalizeSftpConnect(bool okConn,
                                     const QString& err,
                                     openscp::SftpClient* connectedClient,
                                     const openscp::SessionOptions& uiOpt,
                                     std::optional<PendingSiteSaveRequest> saveRequest,
                                     bool canceledByUser) {
    std::unique_ptr<openscp::SftpClient> guard(connectedClient);
    if (m_connectProgress_) {
        m_connectProgress_->close();
        m_connectProgress_.clear();
    }
    m_connectProgressDimmed_ = false;
    m_connectCancelRequested_.reset();
    m_connectInProgress_ = false;
    if (actSites_) actSites_->setEnabled(true);

    if (!okConn) {
        if (actConnect_ && !rightIsRemote_) actConnect_->setEnabled(true);
        if (canceledByUser) statusBar()->showMessage(tr("Conexión cancelada"), 4000);
        else QMessageBox::critical(this, tr("Error de conexión"), err);
        return;
    }

    m_sessionNoHostVerification_ = (uiOpt.known_hosts_policy == openscp::KnownHostsPolicy::Off);
    sftp_ = std::move(guard);
    applyRemoteConnectedUI(uiOpt);
    if (rightIsRemote_ && saveRequest.has_value()) {
        maybePersistQuickConnectSite(uiOpt, *saveRequest, true);
    }
}

void MainWindow::maybePersistQuickConnectSite(const openscp::SessionOptions& opt,
                                              const PendingSiteSaveRequest& req,
                                              bool connectionEstablished) {
    bool regeneratedIds = false;
    QVector<SiteEntry> sites = loadSavedSitesForQuickConnect(&regeneratedIds);

    int matchIndex = -1;
    for (int i = 0; i < sites.size(); ++i) {
        if (sameSavedSiteIdentity(sites[i].opt, opt)) {
            matchIndex = i;
            break;
        }
    }

    bool created = false;
    if (matchIndex < 0) {
        SiteEntry e;
        e.id = newQuickSiteId();
        e.name = ensureUniqueQuickSiteName(
            sites,
            req.siteName.trimmed().isEmpty() ? defaultQuickSiteName(opt) : req.siteName.trimmed()
        );
        e.opt = opt;
        e.opt.password.reset();
        e.opt.private_key_passphrase.reset();
        sites.push_back(e);
        matchIndex = sites.size() - 1;
        created = true;
    }

    if (created || regeneratedIds) {
        saveSavedSitesForQuickConnect(sites);
        refreshOpenSiteManagerWidget(m_siteManager);
    }

    if (!req.saveCredentials) {
        if (connectionEstablished) {
            if (created) statusBar()->showMessage(tr("Conectado. Sitio guardado."), 5000);
            else statusBar()->showMessage(tr("Conectado. Sitio ya existente."), 5000);
        } else {
            if (created) statusBar()->showMessage(tr("Sitio guardado."), 5000);
            else statusBar()->showMessage(tr("Sitio ya existente."), 5000);
        }
        return;
    }

    SecretStore store;
    QStringList issues;
    bool anyCredentialStored = false;
    const SiteEntry& target = sites[matchIndex];

    if (opt.password && !opt.password->empty()) {
        const auto r = store.setSecret(quickSiteSecretKey(target, QStringLiteral("password")),
                                       QString::fromStdString(*opt.password));
        if (r.ok()) anyCredentialStored = true;
        else issues << tr("Contraseña: %1").arg(quickPersistStatusShort(r.status));
    }
    if (opt.private_key_passphrase && !opt.private_key_passphrase->empty()) {
        const auto r = store.setSecret(quickSiteSecretKey(target, QStringLiteral("keypass")),
                                       QString::fromStdString(*opt.private_key_passphrase));
        if (r.ok()) anyCredentialStored = true;
        else issues << tr("Passphrase: %1").arg(quickPersistStatusShort(r.status));
    }

    if (!issues.isEmpty()) {
        QMessageBox::warning(
            this,
            tr("Sitios guardados"),
            tr("El sitio se guardó, pero no fue posible guardar algunas credenciales:\n%1")
                .arg(issues.join("\n"))
        );
    }

    if (connectionEstablished) {
        if (created && anyCredentialStored) statusBar()->showMessage(tr("Conectado. Sitio y credenciales guardados."), 5000);
        else if (created) statusBar()->showMessage(tr("Conectado. Sitio guardado."), 5000);
        else if (anyCredentialStored) statusBar()->showMessage(tr("Conectado. Credenciales actualizadas."), 5000);
        else statusBar()->showMessage(tr("Conectado. Sitio ya existente."), 5000);
    } else {
        if (created && anyCredentialStored) statusBar()->showMessage(tr("Sitio y credenciales guardados."), 5000);
        else if (created) statusBar()->showMessage(tr("Sitio guardado."), 5000);
        else if (anyCredentialStored) statusBar()->showMessage(tr("Credenciales actualizadas."), 5000);
        else statusBar()->showMessage(tr("Sitio ya existente."), 5000);
    }
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
        m_sessionNoHostVerification_ = false;
        updateHostPolicyRiskBanner();
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
    updateHostPolicyRiskBanner();
    updateRemoteWriteability();
    updateDeleteShortcutEnables();
}

// Apply persisted user preferences to the UI.
void MainWindow::applyPreferences() {
    QSettings s("OpenSCP", "OpenSCP");
    const bool showHidden = s.value("UI/showHidden", false).toBool();
    const bool singleClick = s.value("UI/singleClick", false).toBool();
    QString openBehaviorMode = s.value("UI/openBehaviorMode").toString().trimmed().toLower();
    if (openBehaviorMode.isEmpty()) {
        const bool revealLegacy = s.value("UI/openRevealInFolder", false).toBool();
        openBehaviorMode = revealLegacy ? QStringLiteral("reveal") : QStringLiteral("ask");
    }
    if (openBehaviorMode != QStringLiteral("ask") &&
        openBehaviorMode != QStringLiteral("reveal") &&
        openBehaviorMode != QStringLiteral("open")) {
        openBehaviorMode = QStringLiteral("ask");
    }
    prefOpenBehaviorMode_ = openBehaviorMode;
    prefShowQueueOnEnqueue_ = s.value("UI/showQueueOnEnqueue", true).toBool();
    prefNoHostVerificationTtlMin_ = qBound(1, s.value("Security/noHostVerificationTtlMin", 15).toInt(), 120);
    downloadDir_ = defaultDownloadDirFromSettings(s);
    QDir().mkpath(downloadDir_);
    // Keep Site Manager auto-open preference up to date
    m_openSiteManagerOnDisconnect = s.value("UI/openSiteManagerOnDisconnect", true).toBool();
    applyTransferPreferences();

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

void MainWindow::applyTransferPreferences() {
    if (!transferMgr_) return;
    QSettings s("OpenSCP", "OpenSCP");
    const int maxConcurrent = qBound(1, s.value("Transfer/maxConcurrent", 2).toInt(), 8);
    const int globalSpeed = qMax(0, s.value("Transfer/globalSpeedKBps", 0).toInt());
    transferMgr_->setMaxConcurrent(maxConcurrent);
    transferMgr_->setGlobalSpeedLimitKBps(globalSpeed);
    if (transferDlg_) QMetaObject::invokeMethod(transferDlg_, "refresh", Qt::QueuedConnection);
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
