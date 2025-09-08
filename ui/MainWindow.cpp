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
#include <QMenuBar>
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
            error = QString(QCoreApplication::translate("MainWindow", "No se pudo copiar archivo: %1")).arg(srcPath);
            return false;
        }
        return true;
    }

    if (srcInfo.isDir()) {
        // Crea carpeta destino
        if (!QDir().mkpath(dstPath)) {
            error = QString(QCoreApplication::translate("MainWindow", "No se pudo crear carpeta destino: %1")).arg(dstPath);
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
                    error = QString(QCoreApplication::translate("MainWindow", "No se pudo crear subcarpeta destino: %1")).arg(target);
                    return false;
                }
            } else {
                // Asegura carpeta contenedora
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

// Calcula un path local temporal para previsualizar/abrir descargas puntuales.
static QString tempDownloadPathFor(const QString& remoteName) {
    QString base = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (base.isEmpty()) base = QDir::homePath() + "/Downloads";
    QDir().mkpath(base);
    return QDir(base).filePath(remoteName);
}

// Valida nombres simples de archivo/carpeta (sin rutas)
static bool isValidEntryName(const QString& name, QString* why = nullptr) {
    if (name == "." || name == "..") {
        if (why) *why = QCoreApplication::translate("MainWindow", "Nombre inválido: no puede ser '.' ni '..'.");
        return false;
    }
    if (name.contains('/') || name.contains('\\')) {
        if (why) *why = QCoreApplication::translate("MainWindow", "Nombre inválido: no puede contener separadores ('/' o '\\').");
        return false;
    }
    return true;
}

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    // Centrado global de cuadros de diálogo respecto a la ventana principal
    qApp->installEventFilter(this);
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
    // Evitar expandir subárbol con doble clic; usaremos navegación por cambio de raíz
    leftView_->setExpandsOnDoubleClick(false);
    rightView_->setExpandsOnDoubleClick(false);
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
    leftPaneBar_->setIconSize(QSize(18, 18));
    leftPaneBar_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    // Helper de iconos desde recursos locales
    auto resIcon = [](const char* fname) -> QIcon {
        return QIcon(QStringLiteral(":/icons/using/") + QLatin1String(fname));
    };
    // Sub-toolbar izquierda: Arriba, Copiar, Mover, Borrar, Renombrar, Nueva carpeta
    actUpLeft_ = leftPaneBar_->addAction(tr("Arriba"), this, &MainWindow::goUpLeft);
    actUpLeft_->setIcon(resIcon("action-go-up.svg"));
    actUpLeft_->setToolTip(actUpLeft_->text());
    // Botón "Carpeta izquierda" junto a Arriba
    actChooseLeft_ = leftPaneBar_->addAction(tr("Carpeta izquierda"), this, &MainWindow::chooseLeftDir);
    actChooseLeft_->setIcon(resIcon("action-open-folder.svg"));
    actChooseLeft_->setToolTip(actChooseLeft_->text());
    leftPaneBar_->addSeparator();
    actCopyF5_ = leftPaneBar_->addAction(tr("Copiar"), this, &MainWindow::copyLeftToRight);
    actCopyF5_->setIcon(resIcon("action-copy.svg"));
    actCopyF5_->setToolTip(actCopyF5_->text());
    actMoveF6_ = leftPaneBar_->addAction(tr("Mover"), this, &MainWindow::moveLeftToRight);
    actMoveF6_->setIcon(resIcon("action-move-to-right.svg"));
    actMoveF6_->setToolTip(actMoveF6_->text());
    actDelete_ = leftPaneBar_->addAction(tr("Borrar"), this, &MainWindow::deleteFromLeft);
    actDelete_->setIcon(resIcon("action-delete.svg"));
    actDelete_->setToolTip(actDelete_->text());
    actDelete_->setShortcut(QKeySequence(Qt::Key_Delete));
    actDelete_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    if (leftView_) leftView_->addAction(actDelete_);
    // Acción de copiar desde panel derecho a izquierdo (remoto/local -> izquierdo)
    actCopyRight_ = new QAction(tr("Copiar al panel izquierdo"), this);
    connect(actCopyRight_, &QAction::triggered, this, &MainWindow::copyRightToLeft);
    actCopyRight_->setIcon(QIcon(QLatin1String(":/icons/using/action-copy.svg")));
    // Acción de mover desde panel derecho al izquierdo
    actMoveRight_ = new QAction(tr("Mover al panel izquierdo"), this);
    connect(actMoveRight_, &QAction::triggered, this, &MainWindow::moveRightToLeft);
    actMoveRight_->setIcon(QIcon(QLatin1String(":/icons/using/action-move-to-left.svg")));
    // Acciones locales adicionales (también en toolbar)
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
    leftPaneBar_->addAction(actRenameLeft_);
    leftPaneBar_->addAction(actNewFileLeft_);
    leftPaneBar_->addAction(actNewDirLeft_);
    leftLayout->addWidget(leftPaneBar_);

    // Panel izquierdo: toolbar -> path -> view
    leftLayout->addWidget(leftPath_);
    leftLayout->addWidget(leftView_);

    // Sub-toolbar del panel derecho
    rightPaneBar_ = new QToolBar("RightBar", rightPane);
    rightPaneBar_->setIconSize(QSize(18, 18));
    rightPaneBar_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    actUpRight_ = rightPaneBar_->addAction(tr("Arriba"), this, &MainWindow::goUpRight);
    actUpRight_->setIcon(resIcon("action-go-up.svg"));
    actUpRight_->setToolTip(actUpRight_->text());
    // Botón "Carpeta derecha" junto a Arriba
    actChooseRight_  = rightPaneBar_->addAction(tr("Carpeta derecha"),    this, &MainWindow::chooseRightDir);
    actChooseRight_->setIcon(resIcon("action-open-folder.svg"));
    actChooseRight_->setToolTip(actChooseRight_->text());

    // Acciones del panel derecho (crear primero, agregar luego en orden solicitado)
    actDownloadF7_ = new QAction(tr("Descargar"), this);
    connect(actDownloadF7_, &QAction::triggered, this, &MainWindow::downloadRightToLeft);
    actDownloadF7_->setEnabled(false);   // empieza deshabilitado en local
    actDownloadF7_->setIcon(resIcon("action-download.svg"));
    actDownloadF7_->setToolTip(actDownloadF7_->text());

    actUploadRight_ = new QAction(tr("Subir…"), this);
    connect(actUploadRight_, &QAction::triggered, this, &MainWindow::uploadViaDialog);
    actUploadRight_->setIcon(resIcon("action-upload.svg"));
    actUploadRight_->setToolTip(actUploadRight_->text());

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

    // Orden: Copiar, Mover, Borrar, Renombrar, Nueva carpeta, luego Descargar/Subir
    rightPaneBar_->addSeparator();
    // Botones de toolbar con textos genéricos (Copiar/Mover)
    actCopyRightTb_ = new QAction(tr("Copiar"), this);
    connect(actCopyRightTb_, &QAction::triggered, this, &MainWindow::copyRightToLeft);
    actMoveRightTb_ = new QAction(tr("Mover"), this);
    connect(actMoveRightTb_, &QAction::triggered, this, &MainWindow::moveRightToLeft);
    actCopyRightTb_->setIcon(resIcon("action-copy.svg"));
    actCopyRightTb_->setToolTip(actCopyRightTb_->text());
    actMoveRightTb_->setIcon(resIcon("action-move-to-left.svg"));
    actMoveRightTb_->setToolTip(actMoveRightTb_->text());
    rightPaneBar_->addAction(actCopyRightTb_);
    rightPaneBar_->addAction(actMoveRightTb_);
    rightPaneBar_->addAction(actDeleteRight_);
    rightPaneBar_->addAction(actRenameRight_);
    rightPaneBar_->addAction(actNewFileRight_);
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
    if (actNewFileRight_) actNewFileRight_->setEnabled(false);

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
    tb->setToolButtonStyle(Qt::ToolButtonIconOnly);
    tb->setMovable(false);
    // Mantener tamaño predeterminado del sistema para la principal y ajustar sub‑toolbars un poco más pequeñas
    const int mainIconPx = tb->style()->pixelMetric(QStyle::PM_ToolBarIconSize, nullptr, tb);
    const int subIconPx  = qMax(16, mainIconPx - 4); // subtoolbars ligeramente menores
    leftPaneBar_->setIconSize(QSize(subIconPx, subIconPx));
    rightPaneBar_->setIconSize(QSize(subIconPx, subIconPx));
    // Acciones de copiar/mover/borrar ahora residen en la sub-toolbar izquierda
    actConnect_    = tb->addAction(tr("Conectar (SFTP)"), this, &MainWindow::connectSftp);
    actConnect_->setIcon(resIcon("action-connect.svg"));
    actConnect_->setToolTip(actConnect_->text());
    tb->addSeparator();
    actDisconnect_ = tb->addAction(tr("Desconectar"),     this, &MainWindow::disconnectSftp);
    actDisconnect_->setIcon(resIcon("action-disconnect.svg"));
    actDisconnect_->setToolTip(actDisconnect_->text());
    actDisconnect_->setEnabled(false);
    tb->addSeparator();
    actSites_ = tb->addAction(tr("Sitios"), [this] {
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
    actShowQueue_ = tb->addAction(tr("Cola"), [this] {
        if (!transferDlg_) transferDlg_ = new TransferQueueDialog(transferMgr_, this);
        transferDlg_->show(); transferDlg_->raise(); transferDlg_->activateWindow();
    });
    actShowQueue_->setIcon(resIcon("action-open-transfer-queue.svg"));
    actShowQueue_->setToolTip(actShowQueue_->text());
    // Cola siempre habilitada por defecto; sin toggle

    // Atajos globales ya agregados a las acciones correspondientes

    // Barra de menús (nativa en macOS)
    // Se agregan entradas duplicando acciones existentes para quien prefiera menú clásico.
    appMenu_  = menuBar()->addMenu(tr("OpenSCP"));
    actAbout_ = appMenu_->addAction(tr("Acerca de OpenSCP"), this, &MainWindow::showAboutDialog);
    actAbout_->setMenuRole(QAction::AboutRole);
    actPrefs_ = appMenu_->addAction(tr("Configuración…"), this, &MainWindow::showSettingsDialog);
    actPrefs_->setMenuRole(QAction::PreferencesRole);
    // Atajo estándar multiplataforma (Cmd+, en macOS; Ctrl+, en Linux/Windows)
    actPrefs_->setShortcut(QKeySequence::Preferences);
    appMenu_->addSeparator();
    actQuit_  = appMenu_->addAction(tr("Salir"), qApp, &QApplication::quit);
    actQuit_->setMenuRole(QAction::QuitRole);
    // Atajo estándar para salir (Cmd+Q / Ctrl+Q)
    actQuit_->setShortcut(QKeySequence::Quit);

    fileMenu_ = menuBar()->addMenu(tr("Archivo"));
    fileMenu_->addAction(actChooseLeft_);
    fileMenu_->addAction(actChooseRight_);
    fileMenu_->addSeparator();
    fileMenu_->addAction(actConnect_);
    fileMenu_->addAction(actDisconnect_);
    fileMenu_->addAction(actSites_);
    fileMenu_->addAction(actShowQueue_);
    // En plataformas no-macOS, también mostrar Configuración y Salir bajo "Archivo"
    // para una UX familiar en Linux/Windows, manteniendo a la vez el menú "OpenSCP".
#ifndef Q_OS_MAC
    fileMenu_->addSeparator();
    fileMenu_->addAction(actPrefs_);
    fileMenu_->addAction(actQuit_);
#endif

    // Ayuda (evitamos el menú de ayuda nativo para no mostrar el buscador)
    auto* helpMenu = menuBar()->addMenu(tr("Ayuda"));
    // En macOS, un menú titulado exactamente "Help" activa la barra de búsqueda nativa.
    // Mantenemos la etiqueta visible "Help" pero evitamos la detección insertando un espacio de ancho cero.
#ifdef Q_OS_MAC
    {
        const QString t = helpMenu->title();
        if (t.compare(QStringLiteral("Help"), Qt::CaseInsensitive) == 0) {
            helpMenu->setTitle(QStringLiteral("Hel") + QChar(0x200B) + QStringLiteral("p"));
        }
    }
#endif
    helpMenu->menuAction()->setMenuRole(QAction::NoRole);
    // Evitar que macOS mueva acciones al menú de la app: forzar NoRole
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

    // Doble clic/Enter: navegación en ambos paneles
    connect(rightView_, &QTreeView::activated, this, &MainWindow::rightItemActivated);
    connect(leftView_,  &QTreeView::activated, this, &MainWindow::leftItemActivated);

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

    statusBar()->showMessage(tr("Listo"));
    setWindowTitle(tr("OpenSCP (demo) — local/local (clic en Conectar para remoto)"));
    resize(1100, 650);

    // Cola de transferencias
    transferMgr_ = new TransferManager(this);

    // Aviso si almacenamiento inseguro está activo (solo no-Apple cuando se habilita explícitamente)
    if (SecretStore::insecureFallbackActive()) {
        statusBar()->showMessage(tr("Advertencia: almacenamiento de secretos sin cifrar activado (fallback)"), 8000);
    }

    // Aplicar preferencias de usuario (ocultos, modo de clic, etc.)
    applyPreferences();
    updateDeleteShortcutEnables();
}

void MainWindow::showAboutDialog() {
    AboutDialog dlg(this);
    dlg.exec();
}

void MainWindow::showSettingsDialog() {
    SettingsDialog dlg(this);
    if (dlg.exec() == QDialog::Accepted) {
        applyPreferences();
    }
}

void MainWindow::chooseLeftDir() {
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Selecciona carpeta izquierda"), leftPath_->text());
    if (!dir.isEmpty()) setLeftRoot(dir);
}

void MainWindow::chooseRightDir() {
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Selecciona carpeta derecha"), rightPath_->text());
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
        statusBar()->showMessage(tr("Izquierda: ") + path, 3000);
        updateDeleteShortcutEnables();
    } else {
        QMessageBox::warning(this, tr("Ruta inválida"), tr("La carpeta no existe."));
    }
}

void MainWindow::setRightRoot(const QString& path) {
    if (QDir(path).exists()) {
        rightPath_->setText(path);
        rightView_->setRootIndex(rightLocalModel_->index(path)); // <-- aquí
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
        // ---- Rama REMOTA: subir archivos (PUT) al directorio remoto actual ----
        if (!sftp_ || !rightRemoteModel_) {
            QMessageBox::warning(this, tr("SFTP"), tr("No hay sesión SFTP activa."));
            return;
        }

        // Selección en panel izquierdo (origen local)
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
            statusBar()->showMessage(QString(tr("Encolados: %1 subidas")).arg(enq), 4000);
            if (!transferDlg_) transferDlg_ = new TransferQueueDialog(transferMgr_, this);
            transferDlg_->show(); transferDlg_->raise(); transferDlg_->activateWindow();
        }
        return;
    }

    // ---- Rama LOCAL→LOCAL: tu lógica existente tal cual ----
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

    // ---- Rama LOCAL→LOCAL existente ----
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

void MainWindow::connectSftp() {
    ConnectionDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted) return;
    std::string err;
    auto opt = dlg.options();
    if (!establishSftpAsync(opt, err)) {
        QMessageBox::critical(this, tr("Error de conexión"), QString::fromStdString(err));
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
    if (actNewFileRight_)  actNewFileRight_->setEnabled(true);
    if (actRenameRight_)   actRenameRight_->setEnabled(true);
    if (actDeleteRight_)   actDeleteRight_->setEnabled(true);
    if (actMoveRight_)     actMoveRight_->setEnabled(true);
    if (actMoveRightTb_)   actMoveRightTb_->setEnabled(true);
    if (actCopyRightTb_)   actCopyRightTb_->setEnabled(true);
    if (actChooseRight_)   actChooseRight_->setIcon(QIcon(QLatin1String(":/icons/using/action-open-folder.svg")));
    statusBar()->showMessage(tr("Desconectado"), 3000);
    setWindowTitle(tr("OpenSCP (demo) — local/local"));
    updateDeleteShortcutEnables();
}

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

void MainWindow::rightItemActivated(const QModelIndex& idx) {
    // Modo local (panel derecho local): navegar entrando en carpetas
    if (!rightIsRemote_) {
        if (!rightLocalModel_) return;
        const QFileInfo fi = rightLocalModel_->fileInfo(idx);
        if (fi.isDir()) {
            setRightRoot(fi.absoluteFilePath());
        }
        return;
    }
    // Modo remoto: navegar o descargar/abrir archivo
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
    QString remotePath = rightRemoteModel_->rootPath();
    if (!remotePath.endsWith('/')) remotePath += '/';
    remotePath += name;
    const QString localPath = tempDownloadPathFor(name);
    if (!sftp_) { QMessageBox::warning(this, tr("Remoto"), tr("No hay sesión SFTP activa.")); return; }
    QProgressDialog dlg(tr("Descargando ") + name, tr("Cancelar"), 0, 100, this);
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
    if (!ok) { QMessageBox::critical(this, tr("Descarga fallida"), QString::fromStdString(err)); return; }
    QDesktopServices::openUrl(QUrl::fromLocalFile(localPath));
    statusBar()->showMessage(tr("Descargado: ") + localPath, 5000);
}

// Doble clic en panel izquierdo: si es carpeta, entrar y reemplazar raíz
void MainWindow::leftItemActivated(const QModelIndex& idx) {
    if (!leftModel_) return;
    const QFileInfo fi = leftModel_->fileInfo(idx);
    if (fi.isDir()) {
        setLeftRoot(fi.absoluteFilePath());
    }
}

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
        // Descargar todo lo visible (primer nivel) si no hay selección
        int rc = rightRemoteModel_ ? rightRemoteModel_->rowCount() : 0;
        for (int r = 0; r < rc; ++r) rows << rightRemoteModel_->index(r, NAME_COL);
        if (rows.isEmpty()) { QMessageBox::information(this, tr("Descargar"), tr("Nada para descargar.")); return; }
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
        statusBar()->showMessage(QString(tr("Encolados: %1 descargas")).arg(enq), 4000);
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
    QString why; if (!isValidEntryName(name, &why)) { QMessageBox::warning(this, tr("Nombre inválido"), why); return; }
    QDir base(leftPath_->text());
    if (!base.mkpath(base.filePath(name))) { QMessageBox::critical(this, tr("Local"), tr("No se pudo crear carpeta.")); return; }
    setLeftRoot(base.absolutePath());
}

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
            // Sin selección: creación y navegación
            if (rightRemoteWritable_) {
                if (actNewFileRight_) rightContextMenu_->addAction(actNewFileRight_);
                if (actNewDirRight_)  rightContextMenu_->addAction(actNewDirRight_);
            }
        } else {
            // Con selección en remoto
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
        // Local: Opción Arriba si aplica
        if (canGoUp && actUpRight_) rightContextMenu_->addAction(actUpRight_);
        if (!hasSel) {
            // Sin selección: creación
            if (actNewFileRight_) rightContextMenu_->addAction(actNewFileRight_);
            if (actNewDirRight_)  rightContextMenu_->addAction(actNewDirRight_);
        } else {
            // Con selección: operaciones locales + copiar/mover desde izquierda
            if (actNewFileRight_) rightContextMenu_->addAction(actNewFileRight_);
            if (actNewDirRight_)  rightContextMenu_->addAction(actNewDirRight_);
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
        if (actNewFileLeft_) leftContextMenu_->addAction(actNewFileLeft_);
        if (actNewDirLeft_)  leftContextMenu_->addAction(actNewDirLeft_);
    } else {
        if (actNewFileLeft_) leftContextMenu_->addAction(actNewFileLeft_);
        if (actNewDirLeft_)  leftContextMenu_->addAction(actNewDirLeft_);
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

bool MainWindow::confirmHostKeyUI(const QString& host, quint16 port, const QString& algorithm, const QString& fingerprint) {
    QMessageBox box(this);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(tr("Confirmar huella SSH"));
    box.setText(QString(tr("Conectar a %1:%2\nAlgoritmo: %3\nHuella: %4\n\n¿Confiar y guardar en known_hosts?"))
                    .arg(host)
                    .arg(port)
                    .arg(algorithm, fingerprint));
    QPushButton* btYes = box.addButton(tr("Confiar"), QMessageBox::AcceptRole);
    QPushButton* btNo  = box.addButton(tr("Cancelar"), QMessageBox::RejectRole);
    box.exec();
    return box.clickedButton() == btYes;
}

bool MainWindow::eventFilter(QObject* obj, QEvent* ev) {
    // Centrar QDialog/QMessageBox al mostrarse respecto a la ventana principal
    if (ev->type() == QEvent::Show) {
        if (auto* dlg = qobject_cast<QDialog*>(obj)) {
            // Solo centrar diálogos que pertenecen (directa o indirectamente) a esta ventana
            QWidget* p = dlg->parentWidget();
            bool belongsToThis = false;
            while (p) {
                if (p == this) { belongsToThis = true; break; }
                p = p->parentWidget();
            }
            if (belongsToThis) {
                dlg->adjustSize();
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
                    statusBar()->showMessage(tr("Directorio remoto en solo lectura; no se puede subir aquí"), 5000);
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
        statusBar()->showMessage(QString(tr("Encolados: %1 subidas (DND)")).arg(enq), 4000);
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
                    // Evitar copiar sobre sí mismo si es el mismo directorio/archivo
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
                    // Evitar self-drop: mismo archivo/carpeta y mismo destino
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
                    statusBar()->showMessage(QString(tr("Encolados: %1 descargas (DND)")).arg(enq), 4000);
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
    if (actChooseRight_)   actChooseRight_->setIcon(QIcon(QLatin1String(":/icons/using/action-open-folder-remote.svg")));
    statusBar()->showMessage(tr("Conectado (SFTP) a ") + QString::fromStdString(opt.host), 4000);
    setWindowTitle(tr("OpenSCP (demo) — local/remoto (SFTP)"));
    updateRemoteWriteability();
    updateDeleteShortcutEnables();
}

void MainWindow::applyPreferences() {
    QSettings s("OpenSCP", "OpenSCP");
    const bool showHidden = s.value("UI/showHidden", false).toBool();
    const bool singleClick = s.value("UI/singleClick", false).toBool();

    // Local: filtros de modelo (ocultos on/off)
    auto applyLocalFilters = [&](QFileSystemModel* m) {
        if (!m) return;
        QDir::Filters f = QDir::AllEntries | QDir::NoDotAndDotDot | QDir::AllDirs;
        if (showHidden) f = f | QDir::Hidden | QDir::System;
        m->setFilter(f);
    };
    applyLocalFilters(leftModel_);
    applyLocalFilters(rightLocalModel_);

    // Remoto: re-listar con filtro de ocultos
    prefShowHidden_ = showHidden;
    if (rightRemoteModel_) {
        rightRemoteModel_->setShowHidden(showHidden);
        QString dummy;
        rightRemoteModel_->setRootPath(rightRemoteModel_->rootPath(), &dummy);
    }

    // Activación por un clic (añadir/quitar conexión a clicked())
    if (prefSingleClick_ != singleClick) {
        // Desconectar conexiones previas si existían
        if (leftClickConn_)  { QObject::disconnect(leftClickConn_);  leftClickConn_  = QMetaObject::Connection(); }
        if (rightClickConn_) { QObject::disconnect(rightClickConn_); rightClickConn_ = QMetaObject::Connection(); }
        if (singleClick) {
            if (leftView_)  leftClickConn_  = connect(leftView_,  &QTreeView::clicked,   this, &MainWindow::leftItemActivated);
            if (rightView_) rightClickConn_ = connect(rightView_, &QTreeView::clicked,  this, &MainWindow::rightItemActivated);
        }
        prefSingleClick_ = singleClick;
    }
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
    if (actNewFileRight_) actNewFileRight_->setEnabled(rightRemoteWritable_);
    if (actRenameRight_)  actRenameRight_->setEnabled(rightRemoteWritable_);
    if (actDeleteRight_)  actDeleteRight_->setEnabled(rightRemoteWritable_);
    if (actMoveRight_)    actMoveRight_->setEnabled(rightRemoteWritable_);
    if (actMoveRightTb_)  actMoveRightTb_->setEnabled(rightRemoteWritable_);
    updateDeleteShortcutEnables();
}
// Reglas de habilitación de botones/atajos en ambas sub‑toolbars.
// - General: requieren selección.
// - Excepciones: Arriba (si hay padre), Subir… (remoto RW), Descargar (remoto).
void MainWindow::updateDeleteShortcutEnables() {
    auto hasColSel = [&](QTreeView* v) -> bool {
        if (!v || !v->selectionModel()) return false;
        return !v->selectionModel()->selectedRows(NAME_COL).isEmpty();
    };
    const bool leftHasSel = hasColSel(leftView_);
    const bool rightHasSel = hasColSel(rightView_);
    const bool rightWrite = (!rightIsRemote_) || (rightIsRemote_ && rightRemoteWritable_);

    // Izquierda: habilitar según selección (excepción: Arriba)
    if (actCopyF5_)    actCopyF5_->setEnabled(leftHasSel);
    if (actMoveF6_)    actMoveF6_->setEnabled(leftHasSel);
    if (actDelete_)    actDelete_->setEnabled(leftHasSel);
    if (actRenameLeft_)  actRenameLeft_->setEnabled(leftHasSel);
    if (actNewDirLeft_)  actNewDirLeft_->setEnabled(true); // siempre habilitado en local
    if (actNewFileLeft_) actNewFileLeft_->setEnabled(true); // siempre habilitado en local
    if (actUpLeft_) {
        QDir d(leftPath_ ? leftPath_->text() : QString());
        bool canUp = d.cdUp();
        actUpLeft_->setEnabled(canUp);
    }

    // Derecha: habilitar según selección + permisos (excepciones: Arriba, Subir, Descargar)
    if (actCopyRightTb_) actCopyRightTb_->setEnabled(rightHasSel);
    if (actMoveRightTb_) actMoveRightTb_->setEnabled(rightHasSel && rightWrite);
    if (actDeleteRight_) actDeleteRight_->setEnabled(rightHasSel && rightWrite);
    if (actRenameRight_)  actRenameRight_->setEnabled(rightHasSel && rightWrite);
    if (actNewDirRight_)  actNewDirRight_->setEnabled(rightWrite); // habilitado si local o remoto con escritura
    if (actNewFileRight_) actNewFileRight_->setEnabled(rightWrite);
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
