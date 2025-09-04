// Ventana principal de OpenSCP: gestor de archivos de dos paneles con soporte SFTP.
// Ofrece operaciones locales (copiar/mover/borrar) y remotas (navegar, subir, descargar,
// crear/renombrar/borrar), cola de transferencias con reanudación y validación de known_hosts.
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
#include <QDateTime>
#include "PermissionsDialog.hpp"
#include "SiteManagerDialog.hpp"
#include <QMimeData>
#include <QDropEvent>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include "TransferManager.hpp"
#include "TransferQueueDialog.hpp"
#include "SecretStore.hpp"
#include <QEventLoop>
#include <atomic>
#include <thread>
#include <chrono>

static constexpr int NAME_COL = 0;

MainWindow::~MainWindow() = default; // <- define el destructor aquí

// Copia recursivamente un archivo o carpeta.
// Devuelve true si todo salió bien; en caso contrario, false y escribe el error.
static bool copyEntryRecursively(const QString& srcPath, const QString& dstPath, QString& error) {
    QFileInfo srcInfo(srcPath);

    if (srcInfo.isFile()) {
        // Asegura carpeta destino
        QDir().mkpath(QFileInfo(dstPath).dir().absolutePath());
        if (QFile::exists(dstPath)) QFile::remove(dstPath);
        if (!QFile::copy(srcPath, dstPath)) {
            error = QString("No se pudo copiar archivo: %1").arg(srcPath);
            return false;
        }
        return true;
    }

    if (srcInfo.isDir()) {
        // Crea carpeta destino
        if (!QDir().mkpath(dstPath)) {
            error = QString("No se pudo crear carpeta destino: %1").arg(dstPath);
            return false;
        }
        // Itera recursivo
        QDirIterator it(srcPath, QDir::NoDotAndDotDot | QDir::AllEntries, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            const QFileInfo fi = it.fileInfo();
            const QString rel = QDir(srcPath).relativeFilePath(fi.absoluteFilePath());
            const QString target = QDir(dstPath).filePath(rel);

            if (fi.isDir()) {
                if (!QDir().mkpath(target)) {
                    error = QString("No se pudo crear subcarpeta destino: %1").arg(target);
                    return false;
                }
            } else {
                // Asegura carpeta contenedora
                QDir().mkpath(QFileInfo(target).dir().absolutePath());
                if (QFile::exists(target)) QFile::remove(target);
                if (!QFile::copy(fi.absoluteFilePath(), target)) {
                    error = QString("Falló al copiar: %1").arg(fi.absoluteFilePath());
                    return false;
                }
            }
        }
        return true;
    }

    error = "Entrada de origen ni archivo ni carpeta.";
    return false;
}

// Calcula un path local temporal para previsualizar/abrir descargas puntuales.
static QString tempDownloadPathFor(const QString& remoteName) {
    QString base = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (base.isEmpty()) base = QDir::homePath() + "/Downloads";
    QDir().mkpath(base);
    return QDir(base).filePath(remoteName);
}

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    // Modelos
    leftModel_       = new QFileSystemModel(this);
    rightLocalModel_ = new QFileSystemModel(this);

    leftModel_->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::AllDirs);
    rightLocalModel_->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::AllDirs);

    // Rutas iniciales: HOME
    const QString home = QDir::homePath();
    leftModel_->setRootPath(home);
    rightLocalModel_->setRootPath(home);

    // Vistas
    leftView_  = new QTreeView(this);
    rightView_ = new QTreeView(this);

    leftView_->setModel(leftModel_);
    rightView_->setModel(rightLocalModel_);
    leftView_->setRootIndex(leftModel_->index(home));
    rightView_->setRootIndex(rightLocalModel_->index(home));

    // Ajustes visuales básicos
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
    rightView_->setDragEnabled(true); // permitir iniciar arrastres desde panel derecho

    // Aceptar drops en ambos paneles
    rightView_->setAcceptDrops(true);
    rightView_->setDragDropMode(QAbstractItemView::DragDrop);
    rightView_->viewport()->setAcceptDrops(true);
    rightView_->setDefaultDropAction(Qt::CopyAction);
    leftView_->setAcceptDrops(true);
    leftView_->setDragDropMode(QAbstractItemView::DragDrop);
    leftView_->viewport()->setAcceptDrops(true);
    leftView_->setDefaultDropAction(Qt::CopyAction);
    // Filtros en los viewports para recibir arrastre/soltar
    rightView_->viewport()->installEventFilter(this);
    leftView_->viewport()->installEventFilter(this);

    // Entradas de ruta (arriba)
    leftPath_  = new QLineEdit(home, this);
    rightPath_ = new QLineEdit(home, this);
    connect(leftPath_,  &QLineEdit::returnPressed, this, &MainWindow::leftPathEntered);
    connect(rightPath_, &QLineEdit::returnPressed, this, &MainWindow::rightPathEntered);

    // Splitter central con dos paneles
    auto* splitter = new QSplitter(this);
    auto* leftPane  = new QWidget(this);
    auto* rightPane = new QWidget(this);

    auto* leftLayout  = new QVBoxLayout(leftPane);
    auto* rightLayout = new QVBoxLayout(rightPane);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setContentsMargins(0, 0, 0, 0);

    // Sub-toolbar del panel izquierdo
    leftPaneBar_ = new QToolBar("LeftBar", leftPane);
    leftPaneBar_->setIconSize(QSize(16, 16));
    // Sub-toolbar izquierda: Arriba, Copiar, Mover, Borrar, Renombrar, Nueva carpeta
    actUpLeft_ = leftPaneBar_->addAction(tr("Arriba"), this, &MainWindow::goUpLeft);
    leftPaneBar_->addSeparator();
    actCopyF5_ = leftPaneBar_->addAction(tr("Copiar"), this, &MainWindow::copyLeftToRight);
    actMoveF6_ = leftPaneBar_->addAction(tr("Mover"), this, &MainWindow::moveLeftToRight);
    actDelete_ = leftPaneBar_->addAction(tr("Borrar"), this, &MainWindow::deleteFromLeft);
    actDelete_->setShortcut(QKeySequence(Qt::Key_Delete));
    actDelete_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    if (leftView_) leftView_->addAction(actDelete_);
    // Acción de copiar desde panel derecho a izquierdo (remoto/local -> izquierdo)
    actCopyRight_ = new QAction(tr("Copiar al panel izquierdo"), this);
    connect(actCopyRight_, &QAction::triggered, this, &MainWindow::copyRightToLeft);
    // Acción de mover desde panel derecho al izquierdo
    actMoveRight_ = new QAction(tr("Mover al panel izquierdo"), this);
    connect(actMoveRight_, &QAction::triggered, this, &MainWindow::moveRightToLeft);
    // Acciones locales adicionales (también en toolbar)
    actNewDirLeft_  = new QAction(tr("Nueva carpeta"), this);
    connect(actNewDirLeft_, &QAction::triggered, this, &MainWindow::newDirLeft);
    actRenameLeft_  = new QAction(tr("Renombrar"), this);
    connect(actRenameLeft_, &QAction::triggered, this, &MainWindow::renameLeftSelected);
    leftPaneBar_->addAction(actRenameLeft_);
    leftPaneBar_->addAction(actNewDirLeft_);
    leftLayout->addWidget(leftPaneBar_);

    // Panel izquierdo: toolbar -> path -> view
    leftLayout->addWidget(leftPath_);
    leftLayout->addWidget(leftView_);

    // Sub-toolbar del panel derecho
    rightPaneBar_ = new QToolBar("RightBar", rightPane);
    rightPaneBar_->setIconSize(QSize(16, 16));
    actUpRight_ = rightPaneBar_->addAction("Arriba", this, &MainWindow::goUpRight);

    // Acciones del panel derecho (crear primero, agregar luego en orden solicitado)
    actDownloadF7_ = new QAction(tr("Descargar"), this);
    connect(actDownloadF7_, &QAction::triggered, this, &MainWindow::downloadRightToLeft);
    actDownloadF7_->setEnabled(false);   // empieza deshabilitado en local

    actUploadRight_ = new QAction(tr("Subir…"), this);
    connect(actUploadRight_, &QAction::triggered, this, &MainWindow::uploadViaDialog);

    actNewDirRight_  = new QAction(tr("Nueva carpeta"), this);
    connect(actNewDirRight_,  &QAction::triggered, this, &MainWindow::newDirRight);
    actRenameRight_  = new QAction(tr("Renombrar"), this);
    connect(actRenameRight_,  &QAction::triggered, this, &MainWindow::renameRightSelected);
    actDeleteRight_  = new QAction(tr("Borrar"), this);
    connect(actDeleteRight_,  &QAction::triggered, this, &MainWindow::deleteRightSelected);

    // Orden: Copiar, Mover, Borrar, Renombrar, Nueva carpeta, luego Descargar/Subir
    rightPaneBar_->addSeparator();
    // Botones de toolbar con textos genéricos (Copiar/Mover)
    actCopyRightTb_ = new QAction(tr("Copiar"), this);
    connect(actCopyRightTb_, &QAction::triggered, this, &MainWindow::copyRightToLeft);
    actMoveRightTb_ = new QAction(tr("Mover"), this);
    connect(actMoveRightTb_, &QAction::triggered, this, &MainWindow::moveRightToLeft);
    rightPaneBar_->addAction(actCopyRightTb_);
    rightPaneBar_->addAction(actMoveRightTb_);
    rightPaneBar_->addAction(actDeleteRight_);
    rightPaneBar_->addAction(actRenameRight_);
    rightPaneBar_->addAction(actNewDirRight_);
    rightPaneBar_->addSeparator();
    rightPaneBar_->addAction(actDownloadF7_);
    rightPaneBar_->addAction(actUploadRight_);
    // Atajo Supr también en el panel derecho (limitado al widget del panel derecho)
    if (actDeleteRight_) {
        actDeleteRight_->setShortcut(QKeySequence(Qt::Key_Delete));
        actDeleteRight_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        if (rightView_) rightView_->addAction(actDeleteRight_);
    }
    // Deshabilitar solo acciones estrictamente remotas al inicio
    if (actDownloadF7_) actDownloadF7_->setEnabled(false);
    actUploadRight_->setEnabled(false);

    // Panel derecho: toolbar -> path -> view
    rightLayout->addWidget(rightPaneBar_);
    rightLayout->addWidget(rightPath_);
    rightLayout->addWidget(rightView_);

    // Montar paneles en el splitter
    splitter->addWidget(leftPane);
    splitter->addWidget(rightPane);
    setCentralWidget(splitter);

    // Toolbar principal (superior)
    auto* tb = addToolBar("Main");
    actChooseLeft_   = tb->addAction("Carpeta izquierda",  this, &MainWindow::chooseLeftDir);
    tb->addSeparator();
    actChooseRight_  = tb->addAction("Carpeta derecha",    this, &MainWindow::chooseRightDir);
    tb->addSeparator();
    // Acciones de copiar/mover/borrar ahora residen en la sub-toolbar izquierda
    actConnect_    = tb->addAction("Conectar (SFTP)", this, &MainWindow::connectSftp);
    tb->addSeparator();
    actDisconnect_ = tb->addAction("Desconectar",     this, &MainWindow::disconnectSftp);
    actDisconnect_->setEnabled(false);
    tb->addSeparator();
    actSites_ = tb->addAction("Sitios", [this] {
        SiteManagerDialog dlg(this);
        if (dlg.exec() == QDialog::Accepted) {
            openscp::SessionOptions opt{};
            if (dlg.selectedOptions(opt)) {
                std::string err;
                if (!establishSftpAsync(opt, err)) { QMessageBox::critical(this, "Error de conexión", QString::fromStdString(err)); return; }
                applyRemoteConnectedUI(opt);
            }
        }
    });
    tb->addSeparator();
    actShowQueue_ = tb->addAction("Cola", [this] {
        if (!transferDlg_) transferDlg_ = new TransferQueueDialog(transferMgr_, this);
        transferDlg_->show(); transferDlg_->raise(); transferDlg_->activateWindow();
    });
    // Cola siempre habilitada por defecto; sin toggle

    // Atajos globales ya agregados a las acciones correspondientes

    // Doble click en panel derecho: navegar en remoto o descargar/abrir archivo
    connect(rightView_, &QTreeView::activated, this, &MainWindow::rightItemActivated);

    // Menú contextual en panel derecho
    rightView_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(rightView_, &QWidget::customContextMenuRequested, this, &MainWindow::showRightContextMenu);
    if (rightView_->selectionModel()) {
        connect(rightView_->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this]{ updateDeleteShortcutEnables(); });
    }

    // Menú contextual en panel izquierdo (local)
    leftView_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(leftView_, &QWidget::customContextMenuRequested, this, &MainWindow::showLeftContextMenu);

    // Habilitar atajo de borrar solo cuando hay selección en panel izquierdo
    if (leftView_->selectionModel()) {
        connect(leftView_->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this]{ updateDeleteShortcutEnables(); });
    }

    downloadDir_ = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (downloadDir_.isEmpty())
        downloadDir_ = QDir::homePath() + "/Downloads";
    QDir().mkpath(downloadDir_);

    statusBar()->showMessage("Listo");
    setWindowTitle("OpenSCP (demo) — local/local (clic en Conectar para remoto)");
    resize(1100, 650);

    // Cola de transferencias
    transferMgr_ = new TransferManager(this);

    // Aviso si almacenamiento inseguro está activo (solo no-Apple cuando se habilita explícitamente)
    if (SecretStore::insecureFallbackActive()) {
        statusBar()->showMessage(tr("Advertencia: almacenamiento de secretos sin cifrar activado (fallback)"), 8000);
    }

    updateDeleteShortcutEnables();
}

void MainWindow::chooseLeftDir() {
    const QString dir = QFileDialog::getExistingDirectory(this, "Selecciona carpeta izquierda", leftPath_->text());
    if (!dir.isEmpty()) setLeftRoot(dir);
}

void MainWindow::chooseRightDir() {
    const QString dir = QFileDialog::getExistingDirectory(this, "Selecciona carpeta derecha", rightPath_->text());
    if (!dir.isEmpty()) setRightRoot(dir);
}

void MainWindow::leftPathEntered() {
    setLeftRoot(leftPath_->text());
}

void MainWindow::rightPathEntered() {
    if (rightIsRemote_) setRightRemoteRoot(rightPath_->text());
    else setRightRoot(rightPath_->text());
}

void MainWindow::setLeftRoot(const QString& path) {
    if (QDir(path).exists()) {
        leftPath_->setText(path);
        leftView_->setRootIndex(leftModel_->index(path));
        statusBar()->showMessage("Izquierda: " + path, 3000);
        updateDeleteShortcutEnables();
    } else {
        QMessageBox::warning(this, "Ruta inválida", "La carpeta no existe.");
    }
}

void MainWindow::setRightRoot(const QString& path) {
    if (QDir(path).exists()) {
        rightPath_->setText(path);
        rightView_->setRootIndex(rightLocalModel_->index(path)); // <-- aquí
        statusBar()->showMessage("Derecha: " + path, 3000);
        updateDeleteShortcutEnables();
    } else {
        QMessageBox::warning(this, "Ruta inválida", "La carpeta no existe.");
    }
}

static QString joinRemotePath(const QString& base, const QString& name) {
    if (base == "/") return "/" + name;
    return base.endsWith('/') ? base + name : base + "/" + name;
}

void MainWindow::copyLeftToRight() {
    if (rightIsRemote_) {
        // ---- Rama REMOTA: subir archivos (PUT) al directorio remoto actual ----
        if (!sftp_ || !rightRemoteModel_) {
            QMessageBox::warning(this, "SFTP", "No hay sesión SFTP activa.");
            return;
        }

        // Selección en panel izquierdo (origen local)
        auto sel = leftView_->selectionModel();
        if (!sel) {
            QMessageBox::warning(this, "Copiar", "No hay selección disponible.");
            return;
        }
        const auto rows = sel->selectedRows(NAME_COL);
        if (rows.isEmpty()) {
            QMessageBox::information(this, "Copiar", "No hay entradas seleccionadas en el panel izquierdo.");
            return;
        }

        // Encolar siempre subidas
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
            statusBar()->showMessage(QString("Encolados: %1 subidas").arg(enq), 4000);
            if (!transferDlg_) transferDlg_ = new TransferQueueDialog(transferMgr_, this);
            transferDlg_->show(); transferDlg_->raise(); transferDlg_->activateWindow();
        }
        return;
    }

    // ---- Rama LOCAL→LOCAL: tu lógica existente tal cual ----
    const QString dstDirPath = rightPath_->text();
    QDir dstDir(dstDirPath);
    if (!dstDir.exists()) {
        QMessageBox::warning(this, "Destino inválido", "La carpeta de destino no existe.");
        return;
    }

    auto sel = leftView_->selectionModel();
    if (!sel) {
        QMessageBox::warning(this, "Copiar", "No hay selección disponible.");
        return;
    }
    const auto rows = sel->selectedRows(NAME_COL);
    if (rows.isEmpty()) {
        QMessageBox::information(this, "Copiar", "No hay entradas seleccionadas en el panel izquierdo.");
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
                    "Conflicto",
                    QString("«%1» ya existe en destino.\n¿Sobrescribir?").arg(fi.fileName()),
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

    QString msg = QString("Copiados: %1  |  Fallidos: %2  |  Saltados: %3")
                      .arg(ok)
                      .arg(fail)
                      .arg(skipped);
    if (fail > 0 && !lastError.isEmpty()) msg += "\nÚltimo error: " + lastError;
    statusBar()->showMessage(msg, 6000);
}

void MainWindow::moveLeftToRight() {
    if (rightIsRemote_) {
        if (!sftp_ || !rightRemoteModel_) { QMessageBox::warning(this, "SFTP", "No hay sesión SFTP activa."); return; }
        const auto rows = leftView_->selectionModel()->selectedRows(NAME_COL);
        if (rows.isEmpty()) { QMessageBox::information(this, "Mover", "No hay entradas seleccionadas en el panel izquierdo."); return; }
        if (QMessageBox::question(this, "Confirmar mover",
                                  "Esto subirá al servidor y eliminará el origen local.\n¿Deseas continuar?") != QMessageBox::Yes) return;

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
                                "Conflicto remoto",
                                QString("«%1» ya existe.\n¿Sobrescribir?").arg(rel),
                                QMessageBox::Yes | QMessageBox::No | QMessageBox::YesToAll | QMessageBox::NoToAll
                            );
                            if (ret == QMessageBox::YesToAll) policy = OverwritePolicy::OverwriteAll;
                            else if (ret == QMessageBox::NoToAll) policy = OverwritePolicy::SkipAll;
                            if (ret == QMessageBox::No || policy == OverwritePolicy::SkipAll) { ++skipped; continue; }
                        } else if (policy == OverwritePolicy::SkipAll) { ++skipped; continue; }
                    }
                    QProgressDialog dlg(QString("Subiendo %1").arg(rel), "Cancelar", 0, 100, this);
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
                    else { ++fail; lastError = "No se pudo borrar origen: " + fi.absoluteFilePath(); }
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
                            "Conflicto remoto",
                            QString("«%1» ya existe.\n¿Sobrescribir?").arg(fi.fileName()),
                            QMessageBox::Yes | QMessageBox::No | QMessageBox::YesToAll | QMessageBox::NoToAll
                        );
                        if (ret == QMessageBox::YesToAll) policy = OverwritePolicy::OverwriteAll;
                        else if (ret == QMessageBox::NoToAll) policy = OverwritePolicy::SkipAll;
                        if (ret == QMessageBox::No || policy == OverwritePolicy::SkipAll) { ++skipped; continue; }
                    } else if (policy == OverwritePolicy::SkipAll) { ++skipped; continue; }
                }
                QProgressDialog dlg(QString("Subiendo %1").arg(fi.fileName()), "Cancelar", 0, 100, this);
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
                    else { ++fail; lastError = "No se pudo borrar origen: " + fi.absoluteFilePath(); }
                } else {
                    ++fail;
                    lastError = QString::fromStdString(err);
                }
            }
        }
        QString m = QString("Movidos OK: %1  |  Fallidos: %2  |  Omitidos: %3").arg(ok).arg(fail).arg(skipped);
        if (fail > 0 && !lastError.isEmpty()) m += "\nÚltimo error: " + lastError;
        statusBar()->showMessage(m, 5000);
        QString dummy;
        rightRemoteModel_->setRootPath(remoteBase, &dummy);
        return;
    }

    // ---- Rama LOCAL→LOCAL existente ----
    const QString dstDirPath = rightPath_->text();
    QDir dstDir(dstDirPath);
    if (!dstDir.exists()) { QMessageBox::warning(this, "Destino inválido", "La carpeta de destino no existe."); return; }
    const auto rows = leftView_->selectionModel()->selectedRows(NAME_COL);
    if (rows.isEmpty()) { QMessageBox::information(this, "Mover", "No hay entradas seleccionadas en el panel izquierdo."); return; }
    if (QMessageBox::question(this, "Confirmar mover",
                              "Esto copiará y luego eliminará el origen.\n¿Deseas continuar?") != QMessageBox::Yes) return;
    int ok = 0, fail = 0;
    QString lastError;
    for (const QModelIndex& idx : rows) {
        const QFileInfo fi = leftModel_->fileInfo(idx);
        const QString target = dstDir.filePath(fi.fileName());
        QString err;
        if (copyEntryRecursively(fi.absoluteFilePath(), target, err)) {
            bool removed = fi.isDir() ? QDir(fi.absoluteFilePath()).removeRecursively() : QFile::remove(fi.absoluteFilePath());
            if (removed) ok++;
            else { fail++; lastError = "No se pudo borrar origen: " + fi.absoluteFilePath(); }
        } else {
            fail++;
            lastError = err;
        }
    }
    QString m = QString("Movidos OK: %1  |  Fallidos: %2").arg(ok).arg(fail);
    if (fail > 0 && !lastError.isEmpty()) m += "\nÚltimo error: " + lastError;
    statusBar()->showMessage(m, 5000);
}

void MainWindow::deleteFromLeft() {
    const auto rows = leftView_->selectionModel()->selectedRows(NAME_COL);
    if (rows.isEmpty()) { QMessageBox::information(this, "Borrar", "No hay entradas seleccionadas en el panel izquierdo."); return; }
    if (QMessageBox::warning(this, "Confirmar borrado",
                              "Esto eliminará permanentemente los elementos seleccionados en el panel izquierdo.\n¿Deseas continuar?",
                              QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) return;
    int ok = 0, fail = 0;
    for (const QModelIndex& idx : rows) {
        const QFileInfo fi = leftModel_->fileInfo(idx);
        bool removed = fi.isDir() ? QDir(fi.absoluteFilePath()).removeRecursively() : QFile::remove(fi.absoluteFilePath());
        if (removed) ++ok; else ++fail;
    }
    statusBar()->showMessage(QString("Borrados: %1  |  Fallidos: %2").arg(ok).arg(fail), 5000);
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

void MainWindow::connectSftp() {
    ConnectionDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted) return;
    std::string err;
    auto opt = dlg.options();
    if (!establishSftpAsync(opt, err)) {
        QMessageBox::critical(this, "Error de conexión", QString::fromStdString(err));
        return;
    }
    applyRemoteConnectedUI(opt);
}

void MainWindow::disconnectSftp() {
    // Desacoplar cliente de la cola para evitar puntero colgante
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
    // Local: habilitar de nuevo acciones locales del panel derecho
    if (actNewDirRight_)   actNewDirRight_->setEnabled(true);
    if (actRenameRight_)   actRenameRight_->setEnabled(true);
    if (actDeleteRight_)   actDeleteRight_->setEnabled(true);
    if (actMoveRight_)     actMoveRight_->setEnabled(true);
    if (actMoveRightTb_)   actMoveRightTb_->setEnabled(true);
    if (actCopyRightTb_)   actCopyRightTb_->setEnabled(true);
    statusBar()->showMessage("Desconectado", 3000);
    setWindowTitle("OpenSCP (demo) — local/local");
    updateDeleteShortcutEnables();
}

void MainWindow::setRightRemoteRoot(const QString& path) {
    if (!rightIsRemote_ || !rightRemoteModel_) return;
    QString e;
    if (!rightRemoteModel_->setRootPath(path, &e)) {
        QMessageBox::warning(this, "Error remoto", e);
        return;
    }
    rightPath_->setText(path);
    updateRemoteWriteability();
    updateDeleteShortcutEnables();
}

void MainWindow::rightItemActivated(const QModelIndex& idx) {
    if (!rightIsRemote_ || !rightRemoteModel_) return;
    if (rightRemoteModel_->isDir(idx)) {
        const QString name = rightRemoteModel_->nameAt(idx);
        QString next = rightRemoteModel_->rootPath();
        if (!next.endsWith('/')) next += '/';
        next += name;
        setRightRemoteRoot(next);
        return;
    }
    const QString name = rightRemoteModel_->nameAt(idx);
    QString remotePath = rightRemoteModel_->rootPath();
    if (!remotePath.endsWith('/')) remotePath += '/';
    remotePath += name;
    const QString localPath = tempDownloadPathFor(name);
    if (!sftp_) { QMessageBox::warning(this, "Remoto", "No hay sesión SFTP activa."); return; }
    QProgressDialog dlg("Descargando " + name, "Cancelar", 0, 100, this);
    dlg.setWindowModality(Qt::ApplicationModal);
    dlg.setMinimumDuration(0);
    std::string err;
    bool ok = sftp_->get(
        remotePath.toStdString(),
        localPath.toStdString(),
        err,
        [&](std::size_t done, std::size_t total) { int pct = (total > 0) ? int((done * 100) / total) : 0; dlg.setValue(pct); qApp->processEvents(); },
        [&]() { return dlg.wasCanceled(); },
        false
    );
    dlg.setValue(100);
    if (!ok) { QMessageBox::critical(this, "Descarga fallida", QString::fromStdString(err)); return; }
    QDesktopServices::openUrl(QUrl::fromLocalFile(localPath));
    statusBar()->showMessage("Descargado: " + localPath, 5000);
}

void MainWindow::downloadRightToLeft() {
    if (!rightIsRemote_) { QMessageBox::information(this, "Descargar", "El panel derecho no es remoto."); return; }
    if (!sftp_ || !rightRemoteModel_) { QMessageBox::warning(this, "SFTP", "No hay sesión SFTP activa."); return; }
    const QString picked = QFileDialog::getExistingDirectory(this, "Selecciona carpeta de destino (local)", downloadDir_.isEmpty() ? QDir::homePath() : downloadDir_);
    if (picked.isEmpty()) return;
    downloadDir_ = picked;
    QDir dst(downloadDir_);
    if (!dst.exists()) { QMessageBox::warning(this, "Destino inválido", "La carpeta de destino no existe."); return; }
    auto sel = rightView_->selectionModel();
    QModelIndexList rows;
    if (sel) rows = sel->selectedRows(NAME_COL);
    if (rows.isEmpty()) {
        // Descargar todo lo visible (primer nivel) si no hay selección
        int rc = rightRemoteModel_ ? rightRemoteModel_->rowCount() : 0;
        for (int r = 0; r < rc; ++r) rows << rightRemoteModel_->index(r, NAME_COL);
        if (rows.isEmpty()) { QMessageBox::information(this, "Descargar", "Nada para descargar."); return; }
    }
    int enq = 0;
    const QString remoteBase = rightRemoteModel_->rootPath();
    for (const QModelIndex& idx : rows) {
        const QString name = rightRemoteModel_->nameAt(idx);
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
                    const QString childR = (curR.endsWith('/') ? curR + QString::fromStdString(e.name) : curR + "/" + QString::fromStdString(e.name));
                    const QString childL = QDir(curL).filePath(QString::fromStdString(e.name));
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
        statusBar()->showMessage(QString("Encolados: %1 descargas").arg(enq), 4000);
        if (!transferDlg_) transferDlg_ = new TransferQueueDialog(transferMgr_, this);
        transferDlg_->show(); transferDlg_->raise(); transferDlg_->activateWindow();
    }
}

// Copia selección del panel derecho al izquierdo.
// - Remoto -> encola descargas (no bloquea).
// - Local  -> copia local a local (con política de sobrescritura).
void MainWindow::copyRightToLeft() {
    QDir dst(leftPath_->text());
    if (!dst.exists()) { QMessageBox::warning(this, tr("Destino inválido"), tr("La carpeta de destino (panel izquierdo) no existe.")); return; }
    auto sel = rightView_->selectionModel();
    if (!sel) { QMessageBox::warning(this, tr("Copiar"), tr("No hay selección.")); return; }
    const auto rows = sel->selectedRows(NAME_COL);
    if (rows.isEmpty()) { QMessageBox::information(this, tr("Copiar"), tr("Nada seleccionado.")); return; }

    if (!rightIsRemote_) {
        // Copia local -> local (derecha a izquierda)
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

    // Remoto -> local: encolar descargas
    if (!sftp_ || !rightRemoteModel_) { QMessageBox::warning(this, tr("SFTP"), tr("No hay sesión SFTP activa.")); return; }
    int enq = 0;
    const QString remoteBase = rightRemoteModel_->rootPath();
    for (const QModelIndex& idx : rows) {
        const QString name = rightRemoteModel_->nameAt(idx);
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
                    const QString childR = (curR.endsWith('/') ? curR + QString::fromStdString(e.name) : curR + "/" + QString::fromStdString(e.name));
                    const QString childL = QDir(curL).filePath(QString::fromStdString(e.name));
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
        statusBar()->showMessage(QString(tr("Encolados: %1 descargas")).arg(enq), 4000);
        if (!transferDlg_) transferDlg_ = new TransferQueueDialog(transferMgr_, this);
        transferDlg_->show(); transferDlg_->raise(); transferDlg_->activateWindow();
    }
}

// Mueve selección del panel derecho al izquierdo.
// - Remoto -> descarga con progreso y borra en remoto si tuvo éxito.
// - Local  -> copia local y borra el origen.
void MainWindow::moveRightToLeft() {
    auto sel = rightView_->selectionModel();
    if (!sel || sel->selectedRows(NAME_COL).isEmpty()) { QMessageBox::information(this, tr("Mover"), tr("Nada seleccionado.")); return; }
    QDir dst(leftPath_->text());
    if (!dst.exists()) { QMessageBox::warning(this, tr("Destino inválido"), tr("La carpeta de destino (panel izquierdo) no existe.")); return; }

    if (!rightIsRemote_) {
        // Local -> Local: mover (copiar y borrar)
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

    // Remoto -> Local: descargar con progreso y borrar remoto
    if (!sftp_ || !rightRemoteModel_) { QMessageBox::warning(this, tr("SFTP"), tr("No hay sesión SFTP activa.")); return; }
    const auto rows = sel->selectedRows(NAME_COL);
    int ok = 0, fail = 0;
    QString lastErr;
    const QString remoteBase = rightRemoteModel_->rootPath();
    for (const QModelIndex& idx : rows) {
        const QString name = rightRemoteModel_->nameAt(idx);
        QString rpath = remoteBase; if (!rpath.endsWith('/')) rpath += '/'; rpath += name;
        const QString lpath = dst.filePath(name);
        if (rightRemoteModel_->isDir(idx)) {
            QVector<QPair<QString, QString>> stack; stack.push_back({ rpath, lpath }); bool allOk = true;
            while (!stack.isEmpty()) {
                auto pair = stack.back(); stack.pop_back();
                const QString curR = pair.first; const QString curL = pair.second; QDir().mkpath(curL);
                std::vector<openscp::FileInfo> out; std::string lerr;
                if (!sftp_->list(curR.toStdString(), out, lerr)) { allOk = false; lastErr = QString::fromStdString(lerr); break; }
                for (const auto& e : out) {
                    const QString childR = (curR.endsWith('/') ? curR + QString::fromStdString(e.name) : curR + "/" + QString::fromStdString(e.name));
                    const QString childL = QDir(curL).filePath(QString::fromStdString(e.name));
                    if (e.is_dir) { stack.push_back({ childR, childL }); continue; }
                    QProgressDialog dlg(QString(tr("Descargando %1")).arg(QString::fromStdString(e.name)), tr("Cancelar"), 0, 100, this);
                    dlg.setWindowModality(Qt::ApplicationModal); dlg.setMinimumDuration(0);
                    std::string gerr;
                    bool ok1 = sftp_->get(childR.toStdString(), childL.toStdString(), gerr,
                        [&](std::size_t done, std::size_t total) { int pct = (total>0)? int((done*100)/total):0; dlg.setValue(pct); qApp->processEvents(); },
                        [&]() { return dlg.wasCanceled(); }, false);
                    dlg.setValue(100);
                    if (!ok1) { allOk = false; lastErr = QString::fromStdString(gerr); break; }
                }
                if (!allOk) break;
            }
            if (allOk) {
                std::function<bool(const QString&)> delRec = [&](const QString& p) {
                    std::vector<openscp::FileInfo> out; std::string lerr;
                    if (!sftp_->list(p.toStdString(), out, lerr)) { std::string ferr; return sftp_->removeFile(p.toStdString(), ferr); }
                    for (const auto& e : out) {
                        const QString child = joinRemotePath(p, QString::fromStdString(e.name));
                        if (e.is_dir) { if (!delRec(child)) return false; }
                        else { std::string ferr; if (!sftp_->removeFile(child.toStdString(), ferr)) return false; }
                    }
                    std::string derr; return sftp_->removeDir(p.toStdString(), derr);
                };
                if (delRec(rpath)) ++ok; else { ++fail; }
            } else { ++fail; }
        } else {
            QProgressDialog dlg(QString(tr("Descargando %1")).arg(name), tr("Cancelar"), 0, 100, this);
            dlg.setWindowModality(Qt::ApplicationModal); dlg.setMinimumDuration(0);
            std::string gerr;
            bool ok1 = sftp_->get(rpath.toStdString(), lpath.toStdString(), gerr,
                [&](std::size_t done, std::size_t total) { int pct = (total>0)? int((done*100)/total):0; dlg.setValue(pct); qApp->processEvents(); },
                [&]() { return dlg.wasCanceled(); }, false);
            dlg.setValue(100);
            if (ok1) { std::string ferr; if (sftp_->removeFile(rpath.toStdString(), ferr)) ++ok; else { ++fail; lastErr = QString::fromStdString(ferr); } }
            else { ++fail; lastErr = QString::fromStdString(gerr); }
        }
    }
    QString m = QString(tr("Movidos OK: %1  |  Fallidos: %2")).arg(ok).arg(fail);
    if (fail > 0 && !lastErr.isEmpty()) m += "\n" + tr("Último error: ") + lastErr;
    statusBar()->showMessage(m, 5000);
    QString dummy; rightRemoteModel_->setRootPath(remoteBase, &dummy);
    updateRemoteWriteability();
}

void MainWindow::uploadViaDialog() {
    if (!rightIsRemote_ || !sftp_ || !rightRemoteModel_) { QMessageBox::information(this, "Subir", "El panel derecho no es remoto o no hay sesión activa."); return; }
    const QString startDir = uploadDir_.isEmpty() ? QDir::homePath() : uploadDir_;
    QFileDialog dlg(this, "Selecciona archivos o carpetas a subir", startDir);
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
    if (files.isEmpty()) { statusBar()->showMessage("Nada para subir.", 4000); return; }
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
        statusBar()->showMessage(QString("Encolados: %1 subidas").arg(enq), 4000);
        if (!transferDlg_) transferDlg_ = new TransferQueueDialog(transferMgr_, this);
        transferDlg_->show(); transferDlg_->raise(); transferDlg_->activateWindow();
    }
}



void MainWindow::newDirRight() {
    bool ok = false;
    const QString name = QInputDialog::getText(this, "Nueva carpeta", "Nombre:", QLineEdit::Normal, {}, &ok);
    if (!ok || name.isEmpty()) return;
    if (rightIsRemote_) {
        if (!sftp_ || !rightRemoteModel_) return;
        const QString path = joinRemotePath(rightRemoteModel_->rootPath(), name);
        std::string err;
        if (!sftp_->mkdir(path.toStdString(), err, 0755)) { QMessageBox::critical(this, "SFTP", QString::fromStdString(err)); return; }
        QString dummy;
        rightRemoteModel_->setRootPath(rightRemoteModel_->rootPath(), &dummy);
        updateRemoteWriteability();
    } else {
        QDir base(rightPath_->text());
        if (!base.mkpath(base.filePath(name))) { QMessageBox::critical(this, "Local", "No se pudo crear carpeta."); return; }
        setRightRoot(base.absolutePath());
    }
}

void MainWindow::renameRightSelected() {
    auto sel = rightView_->selectionModel();
    if (!sel) return;
    const auto rows = sel->selectedRows();
    if (rows.size() != 1) { QMessageBox::information(this, "Renombrar", "Selecciona exactamente un elemento."); return; }
    if (rightIsRemote_) {
        if (!sftp_ || !rightRemoteModel_) return;
        const QModelIndex idx = rows.first();
        const QString oldName = rightRemoteModel_->nameAt(idx);
        bool ok = false;
        const QString newName = QInputDialog::getText(this, "Renombrar", "Nuevo nombre:", QLineEdit::Normal, oldName, &ok);
        if (!ok || newName.isEmpty() || newName == oldName) return;
        const QString base = rightRemoteModel_->rootPath();
        const QString from = joinRemotePath(base, oldName);
        const QString to   = joinRemotePath(base, newName);
        std::string err;
        if (!sftp_->rename(from.toStdString(), to.toStdString(), err, false)) { QMessageBox::critical(this, "SFTP", QString::fromStdString(err)); return; }
        QString dummy;
        rightRemoteModel_->setRootPath(base, &dummy);
        updateRemoteWriteability();
    } else {
        const QModelIndex idx = rows.first();
        const QFileInfo fi = rightLocalModel_->fileInfo(idx);
        bool ok = false;
        const QString newName = QInputDialog::getText(this, "Renombrar", "Nuevo nombre:", QLineEdit::Normal, fi.fileName(), &ok);
        if (!ok || newName.isEmpty() || newName == fi.fileName()) return;
        const QString newPath = QDir(fi.absolutePath()).filePath(newName);
        bool renamed = QFile::rename(fi.absoluteFilePath(), newPath);
        if (!renamed) renamed = QDir(fi.absolutePath()).rename(fi.absoluteFilePath(), newPath);
        if (!renamed) { QMessageBox::critical(this, "Local", "No se pudo renombrar."); return; }
        setRightRoot(rightPath_->text());
    }
}

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

void MainWindow::newDirLeft() {
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Nueva carpeta"), tr("Nombre:"), QLineEdit::Normal, {}, &ok);
    if (!ok || name.isEmpty()) return;
    QDir base(leftPath_->text());
    if (!base.mkpath(base.filePath(name))) { QMessageBox::critical(this, tr("Local"), tr("No se pudo crear carpeta.")); return; }
    setLeftRoot(base.absolutePath());
}

void MainWindow::deleteRightSelected() {
    auto sel = rightView_->selectionModel();
    if (!sel) return;
    const auto rows = sel->selectedRows();
    if (rows.isEmpty()) { QMessageBox::information(this, "Borrar", "Nada seleccionado."); return; }
    if (rightIsRemote_) {
        if (!sftp_ || !rightRemoteModel_) return;
        if (QMessageBox::warning(this, "Confirmar borrado", "Esto eliminará permanentemente en el servidor remoto.\n¿Continuar?", QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) return;
        int ok = 0, fail = 0;
        QString lastErr;
        const QString base = rightRemoteModel_->rootPath();
        std::function<bool(const QString&)> delRec = [&](const QString& p) {
            std::vector<openscp::FileInfo> out;
            std::string lerr;
            if (!sftp_->list(p.toStdString(), out, lerr)) {
                std::string ferr;
                if (!sftp_->removeFile(p.toStdString(), ferr)) { lastErr = QString::fromStdString(ferr); return false; }
                return true;
            }
            for (const auto& e : out) {
                const QString child = joinRemotePath(p, QString::fromStdString(e.name));
                if (e.is_dir) {
                    if (!delRec(child)) return false;
                } else {
                    std::string ferr;
                    if (!sftp_->removeFile(child.toStdString(), ferr)) { lastErr = QString::fromStdString(ferr); return false; }
                }
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
        QString msg = QString("Borrados OK: %1  |  Fallidos: %2").arg(ok).arg(fail);
        if (fail > 0 && !lastErr.isEmpty()) msg += "\nÚltimo error: " + lastErr;
        statusBar()->showMessage(msg, 6000);
        QString dummy;
        rightRemoteModel_->setRootPath(base, &dummy);
        updateRemoteWriteability();
    } else {
        if (QMessageBox::warning(this, "Confirmar borrado", "Esto eliminará permanentemente en el disco local.\n¿Continuar?", QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) return;
        int ok = 0, fail = 0;
        for (const QModelIndex& idx : rows) {
            const QFileInfo fi = rightLocalModel_->fileInfo(idx);
            bool removed = fi.isDir() ? QDir(fi.absoluteFilePath()).removeRecursively() : QFile::remove(fi.absoluteFilePath());
            if (removed) ++ok; else ++fail;
        }
        statusBar()->showMessage(QString("Borrados: %1  |  Fallidos: %2").arg(ok).arg(fail), 5000);
        setRightRoot(rightPath_->text());
    }
}

void MainWindow::showRightContextMenu(const QPoint& pos) {
    if (!rightContextMenu_) rightContextMenu_ = new QMenu(this);
    rightContextMenu_->clear();

    // Estado de selección y posibilidad de subir nivel
    bool hasSel = false;
    if (auto sel = rightView_->selectionModel()) {
        hasSel = !sel->selectedRows(NAME_COL).isEmpty();
    }
    // ¿Hay directorio padre?
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
        // Opción Arriba (si aplica)
        if (canGoUp && actUpRight_) rightContextMenu_->addAction(actUpRight_);

        // Mostrar siempre "Descargar" en remoto, haya o no selección
        if (actDownloadF7_) rightContextMenu_->addAction(actDownloadF7_);

        if (!hasSel) {
            // Sin selección: solo Arriba y (si escribible) Nueva carpeta
            if (rightRemoteWritable_ && actNewDirRight_) rightContextMenu_->addAction(actNewDirRight_);
        } else {
            // Con selección en remoto
            if (actCopyRight_)   rightContextMenu_->addAction(actCopyRight_);
            if (rightRemoteWritable_) {
                rightContextMenu_->addSeparator();
                if (actUploadRight_) rightContextMenu_->addAction(actUploadRight_);
                if (actNewDirRight_)   rightContextMenu_->addAction(actNewDirRight_);
                if (actRenameRight_)   rightContextMenu_->addAction(actRenameRight_);
                if (actDeleteRight_)   rightContextMenu_->addAction(actDeleteRight_);
                if (actMoveRight_)     rightContextMenu_->addAction(actMoveRight_);
                rightContextMenu_->addSeparator();
                rightContextMenu_->addAction(tr("Cambiar permisos…"), this, &MainWindow::changeRemotePermissions);
            }
        }
    } else {
        // Local: Opción Arriba si aplica
        if (canGoUp && actUpRight_) rightContextMenu_->addAction(actUpRight_);
        if (!hasSel) {
            // Sin selección: solo Arriba + Nueva carpeta
            if (actNewDirRight_) rightContextMenu_->addAction(actNewDirRight_);
        } else {
            // Con selección: operaciones locales + copiar/mover desde izquierda
            if (actNewDirRight_)   rightContextMenu_->addAction(actNewDirRight_);
            if (actRenameRight_)   rightContextMenu_->addAction(actRenameRight_);
            if (actDeleteRight_)   rightContextMenu_->addAction(actDeleteRight_);
            rightContextMenu_->addSeparator();
            // Copiar/mover la selección del panel derecho hacia el izquierdo
            if (actCopyRight_)     rightContextMenu_->addAction(actCopyRight_);
            if (actMoveRight_)     rightContextMenu_->addAction(actMoveRight_);
        }
    }
    rightContextMenu_->popup(rightView_->viewport()->mapToGlobal(pos));
}

void MainWindow::showLeftContextMenu(const QPoint& pos) {
    if (!leftContextMenu_) leftContextMenu_ = new QMenu(this);
    leftContextMenu_->clear();
    // Selección y posibilidad de subir nivel
    bool hasSel = false;
    if (auto sel = leftView_->selectionModel()) {
        hasSel = !sel->selectedRows(NAME_COL).isEmpty();
    }
    QDir d(leftPath_ ? leftPath_->text() : QString());
    bool canGoUp = d.cdUp();

    // Acciones locales del panel izquierdo
    if (canGoUp && actUpLeft_)   leftContextMenu_->addAction(actUpLeft_);
    if (!hasSel) {
        if (actNewDirLeft_) leftContextMenu_->addAction(actNewDirLeft_);
    } else {
        if (actNewDirLeft_) leftContextMenu_->addAction(actNewDirLeft_);
        if (actRenameLeft_) leftContextMenu_->addAction(actRenameLeft_);
        leftContextMenu_->addSeparator();
        // Etiquetas direccionales en el menú, conectadas a las acciones existentes
        leftContextMenu_->addAction(tr("Copiar al panel derecho"), this, &MainWindow::copyLeftToRight);
        leftContextMenu_->addAction(tr("Mover al panel derecho"), this, &MainWindow::moveLeftToRight);
        if (actDelete_)   leftContextMenu_->addAction(actDelete_);
    }
    leftContextMenu_->popup(leftView_->viewport()->mapToGlobal(pos));
}

void MainWindow::changeRemotePermissions() {
    if (!rightIsRemote_ || !sftp_ || !rightRemoteModel_) return;
    auto sel = rightView_->selectionModel();
    if (!sel) return;
    const auto rows = sel->selectedRows();
    if (rows.size() != 1) { QMessageBox::information(this, "Permisos", "Selecciona un solo elemento."); return; }
    const QModelIndex idx = rows.first();
    const QString name = rightRemoteModel_->nameAt(idx);
    const QString base = rightRemoteModel_->rootPath();
    const QString path = joinRemotePath(base, name);
    openscp::FileInfo st{};
    std::string err;
    if (!sftp_->stat(path.toStdString(), st, err)) { QMessageBox::warning(this, "Permisos", QString::fromStdString(err)); return; }
    PermissionsDialog dlg(this);
    dlg.setMode(st.mode & 0777);
    if (dlg.exec() != QDialog::Accepted) return;
    unsigned int newMode = (st.mode & ~0777u) | (dlg.mode() & 0777u);
    auto applyOne = [&](const QString& p) -> bool {
        std::string cerrs;
        if (!sftp_->chmod(p.toStdString(), newMode, cerrs)) { QMessageBox::critical(this, "Permisos", QString::fromStdString(cerrs)); return false; }
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
    statusBar()->showMessage("Permisos actualizados", 3000);
}

bool MainWindow::confirmHostKeyUI(const QString& host, quint16 port, const QString& algorithm, const QString& fingerprint) {
    QMessageBox box(this);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle("Confirmar huella SSH");
    box.setText(QString("Conectar a %1:%2\nAlgoritmo: %3\nHuella: %4\n\n¿Confiar y guardar en known_hosts?")
                    .arg(host)
                    .arg(port)
                    .arg(algorithm, fingerprint));
    QPushButton* btYes = box.addButton("Confiar", QMessageBox::AcceptRole);
    QPushButton* btNo  = box.addButton("Cancelar", QMessageBox::RejectRole);
    box.exec();
    return box.clickedButton() == btYes;
}

bool MainWindow::eventFilter(QObject* obj, QEvent* ev) {
    // DnD sobre el panel derecho (local o remoto)
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
                // Bloquear subida si el remoto es solo lectura
                if (!rightRemoteWritable_) {
                    statusBar()->showMessage("Directorio remoto en solo lectura; no se puede subir aquí", 5000);
                    dd->ignore();
                    return true;
                }
                // Subida a remoto
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
                    statusBar()->showMessage(QString("Encolados: %1 subidas (DND)").arg(enq), 4000);
                    if (!transferDlg_) transferDlg_ = new TransferQueueDialog(transferMgr_, this);
                    transferDlg_->show(); transferDlg_->raise(); transferDlg_->activateWindow();
                }
                dd->acceptProposedAction();
                return true;
            } else {
                // Copia local al directorio del panel derecho
                QDir dst(rightPath_->text());
                if (!dst.exists()) { dd->acceptProposedAction(); return true; }
                int ok = 0, fail = 0;
                QString lastError;
                for (const QUrl& u : urls) {
                    const QString p = u.toLocalFile();
                    if (p.isEmpty()) continue;
                    QFileInfo fi(p);
                    const QString target = dst.filePath(fi.fileName());
                    QString err;
                    if (copyEntryRecursively(fi.absoluteFilePath(), target, err)) ++ok;
                    else { ++fail; lastError = err; }
                }
                QString m = QString("Copiados: %1  |  Fallidos: %2").arg(ok).arg(fail);
                if (fail > 0 && !lastError.isEmpty()) m += "\nÚltimo error: " + lastError;
                statusBar()->showMessage(m, 5000);
                dd->acceptProposedAction();
                return true;
            }
        }
    }
    // DnD sobre el panel izquierdo (local): copiar/descargar
    // Actualiza estado del atajo de borrar si la selección cambia por DnD o click
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
                // Copia local hacia panel izquierdo
                QDir dst(leftPath_->text());
                if (!dst.exists()) { dd->acceptProposedAction(); return true; }
                int ok = 0, fail = 0;
                QString lastError;
                for (const QUrl& u : urls) {
                    const QString p = u.toLocalFile();
                    if (p.isEmpty()) continue;
                    QFileInfo fi(p);
                    const QString target = dst.filePath(fi.fileName());
                    QString err;
                    if (copyEntryRecursively(fi.absoluteFilePath(), target, err)) ++ok;
                    else { ++fail; lastError = err; }
                }
                QString m = QString("Copiados: %1  |  Fallidos: %2").arg(ok).arg(fail);
                if (fail > 0 && !lastError.isEmpty()) m += "\nÚltimo error: " + lastError;
                statusBar()->showMessage(m, 5000);
                dd->acceptProposedAction();
                updateDeleteShortcutEnables();
                return true;
            }
            // Descargar desde remoto (según selección del panel derecho)
            if (rightIsRemote_ == true && rightView_ && rightRemoteModel_) {
                auto sel = rightView_->selectionModel();
                if (!sel || sel->selectedRows(NAME_COL).isEmpty()) { dd->acceptProposedAction(); return true; }
                const auto rows = sel->selectedRows(NAME_COL);
                int enq = 0;
                const QString remoteBase = rightRemoteModel_->rootPath();
                QDir dst(leftPath_->text());
                for (const QModelIndex& idx : rows) {
                    const QString name = rightRemoteModel_->nameAt(idx);
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
                                const QString childR = (curR.endsWith('/') ? curR + QString::fromStdString(e.name) : curR + "/" + QString::fromStdString(e.name));
                                const QString childL = QDir(curL).filePath(QString::fromStdString(e.name));
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
                    statusBar()->showMessage(QString("Encolados: %1 descargas (DND)").arg(enq), 4000);
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

bool MainWindow::establishSftpAsync(openscp::SessionOptions opt, std::string& err) {
    // Inyectar confirmación de huella (TOFU) desde UI
    opt.hostkey_confirm_cb = [this](const std::string& h, std::uint16_t p, const std::string& alg, const std::string& fp) {
        bool accepted = false;
        QMetaObject::invokeMethod(this, [&, h, p, alg, fp] {
            accepted = confirmHostKeyUI(QString::fromStdString(h), (quint16)p, QString::fromStdString(alg), QString::fromStdString(fp));
        }, Qt::BlockingQueuedConnection);
        return accepted;
    };

    // Callback de keyboard-interactive (OTP/2FA). Prefiere autocompletar password/usuario; pide OTP si hace falta.
    const std::string savedUser = opt.username;
    const std::string savedPass = opt.password ? *opt.password : std::string();
    opt.keyboard_interactive_cb = [this, savedUser, savedPass](const std::string& name,
                                                               const std::string& instruction,
                                                               const std::vector<std::string>& prompts,
                                                               std::vector<std::string>& responses) -> bool {
        (void)name;
        responses.clear();
        responses.reserve(prompts.size());
        // Resolver cada prompt: autollenar user/pass y pedir OTP/códigos si aparecen
        for (const std::string& p : prompts) {
            QString qprompt = QString::fromStdString(p);
            QString lower = qprompt.toLower();
            // Usuario
            if (lower.contains("user") || lower.contains("name:")) {
                responses.emplace_back(savedUser);
                continue;
            }
            // Contraseña
            if (lower.contains("password") || lower.contains("passphrase") || lower.contains("passcode")) {
                if (!savedPass.empty()) { responses.emplace_back(savedPass); continue; }
                // Pedir contraseña si no la teníamos
                QString ans;
                bool ok = false;
                QMetaObject::invokeMethod(this, [&] {
                    ans = QInputDialog::getText(this, tr("Contraseña requerida"), qprompt,
                                                QLineEdit::Password, QString(), &ok);
                }, Qt::BlockingQueuedConnection);
                if (!ok) return false;
                responses.emplace_back(ans.toUtf8().toStdString());
                continue;
            }
            // OTP / Código de verificación / Token
            if (lower.contains("verification") || lower.contains("verify") || lower.contains("otp") || lower.contains("code") || lower.contains("token")) {
                QString title = tr("Código de verificación requerido");
                if (!instruction.empty()) title += " — " + QString::fromStdString(instruction);
                QString ans;
                bool ok = false;
                QMetaObject::invokeMethod(this, [&] {
                    ans = QInputDialog::getText(this, title, qprompt, QLineEdit::Password, QString(), &ok);
                }, Qt::BlockingQueuedConnection);
                if (!ok) return false;
                responses.emplace_back(ans.toUtf8().toStdString());
                continue;
            }
            // Caso genérico: pedir texto (sin ocultar)
            QString title = tr("Información requerida");
            if (!instruction.empty()) title += " — " + QString::fromStdString(instruction);
            QString ans;
            bool ok = false;
            QMetaObject::invokeMethod(this, [&] {
                ans = QInputDialog::getText(this, title, qprompt, QLineEdit::Normal, QString(), &ok);
            }, Qt::BlockingQueuedConnection);
            if (!ok) return false;
            responses.emplace_back(ans.toUtf8().toStdString());
        }
        return responses.size() == prompts.size();
    };

    QProgressDialog progress(tr("Conectando…"), QString(), 0, 0, this);
    progress.setWindowModality(Qt::ApplicationModal);
    progress.setCancelButton(nullptr);
    progress.show();

    std::atomic<bool> done{ false };
    bool okConn = false;
    std::thread th([&] {
        auto tmp = std::make_unique<openscp::Libssh2SftpClient>();
        okConn = tmp->connect(opt, err);
        if (okConn) {
            QMetaObject::invokeMethod(this, [this, t = tmp.release()] { sftp_.reset(t); }, Qt::BlockingQueuedConnection);
        }
        done = true;
    });
    while (!done) { qApp->processEvents(QEventLoop::AllEvents, 50); std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
    th.join();
    progress.close();
    return okConn;
}

void MainWindow::applyRemoteConnectedUI(const openscp::SessionOptions& opt) {
    delete rightRemoteModel_;
    rightRemoteModel_ = new RemoteModel(sftp_.get(), this);
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
    if (actRenameRight_)  actRenameRight_->setEnabled(true);
    if (actDeleteRight_)  actDeleteRight_->setEnabled(true);
    statusBar()->showMessage("Conectado (SFTP) a " + QString::fromStdString(opt.host), 4000);
    setWindowTitle("OpenSCP (demo) — local/remoto (SFTP)");
    updateRemoteWriteability();
    updateDeleteShortcutEnables();
}

void MainWindow::updateRemoteWriteability() {
    // Determina si el directorio remoto actual es escribible intentando crear y borrar
    // una carpeta temporal. Conservador: si falla, consideramos solo lectura.
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
    // Reflejar en acciones que requieren escritura
    if (actUploadRight_) actUploadRight_->setEnabled(rightRemoteWritable_);
    if (actNewDirRight_)  actNewDirRight_->setEnabled(rightRemoteWritable_);
    if (actRenameRight_)  actRenameRight_->setEnabled(rightRemoteWritable_);
    if (actDeleteRight_)  actDeleteRight_->setEnabled(rightRemoteWritable_);
    if (actMoveRight_)    actMoveRight_->setEnabled(rightRemoteWritable_);
    if (actMoveRightTb_)  actMoveRightTb_->setEnabled(rightRemoteWritable_);
    updateDeleteShortcutEnables();
}
// Reglas de habilitación de botones/atajos en ambas sub‑toolbars.
// - General: requieren selección.
// - Excepciones: Arriba (si hay padre), Renombrar (siempre), Subir… (remoto RW), Descargar (remoto).
void MainWindow::updateDeleteShortcutEnables() {
    auto hasColSel = [&](QTreeView* v) -> bool {
        if (!v || !v->selectionModel()) return false;
        return !v->selectionModel()->selectedRows(NAME_COL).isEmpty();
    };
    const bool leftHasSel = hasColSel(leftView_);
    const bool rightHasSel = hasColSel(rightView_);
    const bool rightWrite = (!rightIsRemote_) || (rightIsRemote_ && rightRemoteWritable_);

    // Izquierda: habilitar según selección (excepciones: Arriba, Renombrar)
    if (actCopyF5_)    actCopyF5_->setEnabled(leftHasSel);
    if (actMoveF6_)    actMoveF6_->setEnabled(leftHasSel);
    if (actDelete_)    actDelete_->setEnabled(leftHasSel);
    if (actRenameLeft_) actRenameLeft_->setEnabled(true); // excepción: siempre habilitado
    if (actNewDirLeft_) actNewDirLeft_->setEnabled(true); // siempre habilitado en local
    if (actUpLeft_) {
        QDir d(leftPath_ ? leftPath_->text() : QString());
        bool canUp = d.cdUp();
        actUpLeft_->setEnabled(canUp);
    }

    // Derecha: habilitar según selección + permisos (excepciones: Arriba, Renombrar, Subir, Descargar)
    if (actCopyRightTb_) actCopyRightTb_->setEnabled(rightHasSel);
    if (actMoveRightTb_) actMoveRightTb_->setEnabled(rightHasSel && rightWrite);
    if (actDeleteRight_) actDeleteRight_->setEnabled(rightHasSel && rightWrite);
    if (actRenameRight_) actRenameRight_->setEnabled(rightWrite); // excepción: no depende de selección
    if (actNewDirRight_) actNewDirRight_->setEnabled(rightWrite); // habilitado si local o remoto con escritura
    if (actUploadRight_) actUploadRight_->setEnabled(rightIsRemote_ && rightRemoteWritable_); // excepción
    if (actDownloadF7_) actDownloadF7_->setEnabled(rightIsRemote_); // excepción: habilitado sin selección
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
