#include "MainWindow.hpp"
#include "openscp/SftpClient.hpp"
#include "ConnectionDialog.hpp"
#include "RemoteModel.hpp"
#include "openscp/MockSftpClient.hpp"
#include <QApplication>
#include <QHBoxLayout>
#include <QSplitter>
#include <QToolBar>
#include <QFileDialog>
#include <QStatusBar>
#include <QHeaderView>
#include <QMessageBox>
#include <QFileInfo>
#include <QDir>
#include <QFile>
#include <QKeySequence>
#include <QDirIterator>

static constexpr int NAME_COL = 0;

MainWindow::~MainWindow() = default; // <- define el destructor aquí

#include <QDirIterator>

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
    auto tuneView = [](QTreeView* v){
        v->setSelectionMode(QAbstractItemView::ExtendedSelection);
        v->setSortingEnabled(true);
        v->sortByColumn(0, Qt::AscendingOrder);
        v->header()->setStretchLastSection(true);
        v->setColumnWidth(0, 280);
    };
    tuneView(leftView_);
    tuneView(rightView_);

    // Entradas de ruta (arriba)
    leftPath_  = new QLineEdit(home, this);
    rightPath_ = new QLineEdit(home, this);
    connect(leftPath_,  &QLineEdit::returnPressed, this, &MainWindow::leftPathEntered);
    connect(rightPath_, &QLineEdit::returnPressed, this, &MainWindow::rightPathEntered);

    // UI (layout) Splitter central con dos paneles
    auto* splitter = new QSplitter(this);
    auto* leftPane  = new QWidget(this);
    auto* rightPane = new QWidget(this);

    auto* leftLayout  = new QVBoxLayout(leftPane);
    auto* rightLayout = new QVBoxLayout(rightPane);
    leftLayout->setContentsMargins(0,0,0,0);
    rightLayout->setContentsMargins(0,0,0,0);

    leftLayout->addWidget(leftPath_);
    leftLayout->addWidget(leftView_);
    rightLayout->addWidget(rightPath_);
    rightLayout->addWidget(rightView_);

    splitter->addWidget(leftPane);
    splitter->addWidget(rightPane);
    setCentralWidget(splitter);

    // Barra de herramientas //
    auto* tb = addToolBar("Main");
    actChooseLeft_   = tb->addAction("Carpeta izquierda",  this, &MainWindow::chooseLeftDir);
    actChooseRight_  = tb->addAction("Carpeta derecha",    this, &MainWindow::chooseRightDir);
    tb->addSeparator();
    actCopyF5_ = tb->addAction("Copiar (F5)", this, &MainWindow::copyLeftToRight);
    actCopyF5_->setShortcut(QKeySequence(Qt::Key_F5));
    tb->addSeparator();
    actMoveF6_ = tb->addAction("Mover (F6)", this, &MainWindow::moveLeftToRight);
    actMoveF6_->setShortcut(QKeySequence(Qt::Key_F6));
    actDelete_ = tb->addAction("Borrar (Supr)", this, &MainWindow::deleteFromLeft);
    actDelete_->setShortcut(QKeySequence(Qt::Key_Delete));
    tb->addSeparator();
    actConnect_    = tb->addAction("Conectar (SFTP)", this, &MainWindow::connectSftp);
    actDisconnect_ = tb->addAction("Desconectar",     this, &MainWindow::disconnectSftp);
    actDisconnect_->setEnabled(false);

    this->addAction(actMoveF6_);
    this->addAction(actDelete_);

    // Doble click en panel derecho para navegar remoto
    connect(rightView_, &QTreeView::activated, this, &MainWindow::rightItemActivated);

    statusBar()->showMessage("Listo");
    setWindowTitle("OpenSCP (demo) — local/local (clic en Conectar para remoto)");
    resize(1100, 650);
}

void MainWindow::chooseLeftDir() {
    const QString dir = QFileDialog::getExistingDirectory(this, "Selecciona carpeta izquierda", leftPath_->text());
    if (!dir.isEmpty()) setLeftRoot(dir);
}

void MainWindow::chooseRightDir() {
    const QString dir = QFileDialog::getExistingDirectory(this, "Selecciona carpeta derecha", rightPath_->text());
    if (!dir.isEmpty()) setRightRoot(dir);
}

void MainWindow::leftPathEntered()  { setLeftRoot(leftPath_->text()); }

void MainWindow::rightPathEntered() {
    if (rightIsRemote_) setRightRemoteRoot(rightPath_->text());
    else setRightRoot(rightPath_->text());
}


void MainWindow::setLeftRoot(const QString& path) {
    if (QDir(path).exists()) {
        leftPath_->setText(path);
        leftView_->setRootIndex(leftModel_->index(path));
        statusBar()->showMessage("Izquierda: " + path, 3000);
    } else {
        QMessageBox::warning(this, "Ruta inválida", "La carpeta no existe.");
    }
}

void MainWindow::setRightRoot(const QString& path) {
    if (QDir(path).exists()) {
        rightPath_->setText(path);
        rightView_->setRootIndex(rightLocalModel_->index(path)); // <-- aquí
        statusBar()->showMessage("Derecha: " + path, 3000);
    } else {
        QMessageBox::warning(this, "Ruta inválida", "La carpeta no existe.");
    }
}


void MainWindow::copyLeftToRight() {
    if (rightIsRemote_) {
        QMessageBox::information(this, "Copiar",
            "Copiar a remoto (SFTP) se implementará en el siguiente paso.\n"
            "Por ahora el remoto es solo lectura (mock).");
        return;
    }

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

    // Política simple para colisiones
    enum class OverwritePolicy { Ask, OverwriteAll, SkipAll };
    OverwritePolicy policy = OverwritePolicy::Ask;

    int ok = 0, fail = 0, skipped = 0;
    QString lastError;

    for (const QModelIndex& idx : rows) {
        const QFileInfo fi = leftModel_->fileInfo(idx);
        const QString target = dstDir.filePath(fi.fileName());

        // Resolver colisión si el destino existe
        if (QFileInfo::exists(target)) {
            if (policy == OverwritePolicy::Ask) {
                auto ret = QMessageBox::question(this, "Conflicto",
                    QString("«%1» ya existe en destino.\n¿Sobrescribir?").arg(fi.fileName()),
                    QMessageBox::Yes | QMessageBox::No | QMessageBox::YesToAll | QMessageBox::NoToAll);
                if (ret == QMessageBox::YesToAll) policy = OverwritePolicy::OverwriteAll;
                else if (ret == QMessageBox::NoToAll) policy = OverwritePolicy::SkipAll;

                if (ret == QMessageBox::No || policy == OverwritePolicy::SkipAll) {
                    ++skipped;
                    continue;
                }
            }
            // OverwriteAll o Yes: eliminar antes de copiar
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
                    .arg(ok).arg(fail).arg(skipped);
    if (fail > 0 && !lastError.isEmpty()) msg += "\nÚltimo error: " + lastError;
    statusBar()->showMessage(msg, 6000);
}

void MainWindow::moveLeftToRight() {
    const QString dstDirPath = rightPath_->text();
    QDir dstDir(dstDirPath);
    if (!dstDir.exists()) {
        QMessageBox::warning(this, "Destino inválido", "La carpeta de destino no existe.");
        return;
    }

    const auto rows = leftView_->selectionModel()->selectedRows(NAME_COL);
    if (rows.isEmpty()) {
        QMessageBox::information(this, "Mover", "No hay entradas seleccionadas en el panel izquierdo.");
        return;
    }

    if (QMessageBox::question(this, "Confirmar mover",
        "Esto copiará y luego eliminará el origen.\n¿Deseas continuar?")
        != QMessageBox::Yes) {
        return;
    }

    int ok = 0, fail = 0;
    QString lastError;

    for (const QModelIndex& idx : rows) {
        const QFileInfo fi = leftModel_->fileInfo(idx);
        const QString target = dstDir.filePath(fi.fileName());

        QString err;
        if (copyEntryRecursively(fi.absoluteFilePath(), target, err)) {
            // Elimina origen (archivo o carpeta) tras copiar
            bool removed = fi.isDir() ? QDir(fi.absoluteFilePath()).removeRecursively()
                                      : QFile::remove(fi.absoluteFilePath());
            if (removed) ok++;
            else { fail++; lastError = "No se pudo borrar origen: " + fi.absoluteFilePath(); }
        } else {
            fail++; lastError = err;
        }
    }

    QString msg = QString("Movidos OK: %1  |  Fallidos: %2").arg(ok).arg(fail);
    if (fail > 0 && !lastError.isEmpty()) msg += "\nÚltimo error: " + lastError;
    statusBar()->showMessage(msg, 5000);
}

void MainWindow::deleteFromLeft() {
    const auto rows = leftView_->selectionModel()->selectedRows(NAME_COL);
    if (rows.isEmpty()) {
        QMessageBox::information(this, "Borrar", "No hay entradas seleccionadas en el panel izquierdo.");
        return;
    }

    if (QMessageBox::warning(this, "Confirmar borrado",
        "Esto eliminará permanentemente los elementos seleccionados en el panel izquierdo.\n¿Deseas continuar?",
        QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) {
        return;
    }

    int ok = 0, fail = 0;
    for (const QModelIndex& idx : rows) {
        const QFileInfo fi = leftModel_->fileInfo(idx);
        bool removed = fi.isDir() ? QDir(fi.absoluteFilePath()).removeRecursively()
                                  : QFile::remove(fi.absoluteFilePath());
        if (removed) ok++; else fail++;
    }

    statusBar()->showMessage(QString("Borrados: %1  |  Fallidos: %2").arg(ok).arg(fail), 5000);
}

void MainWindow::connectSftp() {
    // Pide datos de conexión
    ConnectionDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted) return;

    // Crea cliente (mock por ahora)
    sftp_ = std::make_unique<openscp::MockSftpClient>();
    std::string err;
    const auto opt = dlg.options();
    if (!sftp_->connect(opt, err)) {
        QMessageBox::critical(this, "Error de conexión", QString::fromStdString(err));
        sftp_.reset();
        return;
    }

    // Crea modelo remoto y cámbialo en la vista derecha
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
    rightPath_->setText("/");
    rightIsRemote_ = true;
    actConnect_->setEnabled(false);
    actDisconnect_->setEnabled(true);
    statusBar()->showMessage("Conectado (mock) a " + QString::fromStdString(opt.host), 4000);
    setWindowTitle("OpenSCP (demo) — local/remoto (mock)");
}

void MainWindow::disconnectSftp() {
    if (sftp_) sftp_->disconnect();
    sftp_.reset();
    if (rightRemoteModel_) {
        rightView_->setModel(rightLocalModel_);
        delete rightRemoteModel_;
        rightRemoteModel_ = nullptr;
    }
    rightIsRemote_ = false;
    actConnect_->setEnabled(true);
    actDisconnect_->setEnabled(false);
    statusBar()->showMessage("Desconectado", 3000);
    setWindowTitle("OpenSCP (demo) — local/local");
}

void MainWindow::setRightRemoteRoot(const QString& path) {
    if (!rightIsRemote_ || !rightRemoteModel_) return;
    QString e;
    if (!rightRemoteModel_->setRootPath(path, &e)) {
        QMessageBox::warning(this, "Error remoto", e);
        return;
    }
    rightPath_->setText(path);
}

void MainWindow::rightItemActivated(const QModelIndex& idx) {
    if (!rightIsRemote_ || !rightRemoteModel_) return;
    if (!rightRemoteModel_->isDir(idx)) return;
    const QString name = rightRemoteModel_->nameAt(idx);
    QString next = rightRemoteModel_->rootPath();
    if (!next.endsWith('/')) next += '/';
    next += name;
    setRightRemoteRoot(next);
}

