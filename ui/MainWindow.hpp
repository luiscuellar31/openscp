// Declaración de la ventana principal y su estado/acciones.
#pragma once
#include <QMainWindow>
#include <QFileSystemModel>
#include <QTreeView>
#include <QLineEdit>
#include <QAction>
#include <string>
#include <memory>

class RemoteModel;              // fwd
class QModelIndex;              // fwd para la firma del slot
class QToolBar;                 // fwd
class QMenu;                    // fwd
class QEvent;                   // fwd para eventFilter
class QDialog;                  // fwd
namespace openscp { class SftpClient; struct SessionOptions; } // fwd

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow();
protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;
    void showEvent(QShowEvent* e) override;

private slots:
    void chooseLeftDir();
    void chooseRightDir();
    void leftPathEntered();
    void rightPathEntered();
    void copyLeftToRight(); // F5
    void copyRightToLeft(); // remote -> left (sin diálogo)
    void moveRightToLeft(); // mueve selección del panel derecho al izquierdo
    void moveLeftToRight(); // F6
    void deleteFromLeft();  // Supr
    void goUpRight(); // Subir un nivel (derecha)
    void goUpLeft();  // Subir un nivel (izquierda)

    void connectSftp();
    void disconnectSftp();
    void rightItemActivated(const QModelIndex& idx); // doble click en remoto
    void leftItemActivated(const QModelIndex& idx);  // doble click en local (izquierda)
    void downloadRightToLeft(); // remoto -> local
    void uploadViaDialog();     // local -> remoto (diálogo: archivos o carpeta)
    void newDirRight();
    void newFileRight();
    void renameRightSelected();
    void deleteRightSelected();
    void showRightContextMenu(const QPoint& pos);
    void changeRemotePermissions();
    void showLeftContextMenu(const QPoint& pos);
    void newDirLeft();
    void newFileLeft();
    void renameLeftSelected();

    // Menú de aplicación
    void showAboutDialog();
    void showSettingsDialog();

private:
    void updateDeleteShortcutEnables();
    void applyPreferences();
    // Estado remoto (una sola sesión activa)
    std::unique_ptr<openscp::SftpClient> sftp_;
    bool rightIsRemote_ = false;

    void setLeftRoot(const QString& path);
    void setRightRoot(const QString& path);       // local
    void setRightRemoteRoot(const QString& path); // remoto

    // Modelos
    QFileSystemModel* leftModel_        = nullptr;
    QFileSystemModel* rightLocalModel_  = nullptr;
    RemoteModel*      rightRemoteModel_ = nullptr;

    // Vistas y rutas
    QTreeView* leftView_  = nullptr;
    QTreeView* rightView_ = nullptr;

    QLineEdit* leftPath_  = nullptr;
    QLineEdit* rightPath_ = nullptr;

    // Acciones
    QAction* actChooseLeft_  = nullptr;
    QAction* actChooseRight_ = nullptr;
    QAction* actCopyF5_      = nullptr;
    QAction* actCopyRight_   = nullptr; // Copiar desde panel derecho (remoto) al izquierdo
    QAction* actMoveRight_   = nullptr; // Mover desde panel derecho al izquierdo
    QAction* actMoveF6_      = nullptr;
    QAction* actDelete_      = nullptr;
    QAction* actConnect_     = nullptr;
    QAction* actDisconnect_  = nullptr;
    QAction* actDownloadF7_ = nullptr;
    QAction* actUploadRight_ = nullptr;
    QAction* actNewDirRight_  = nullptr;
    QAction* actNewFileRight_ = nullptr;
    QAction* actRenameRight_  = nullptr;
    QAction* actDeleteRight_  = nullptr; // remoto
    QAction* actNewDirLeft_   = nullptr; // local (izquierda)
    QAction* actNewFileLeft_  = nullptr; // local (izquierda)
    QAction* actRenameLeft_   = nullptr; // local (izquierda)
    QAction* actCopyRightTb_  = nullptr; // toolbar derecha: Copiar (texto genérico)
    QAction* actMoveRightTb_  = nullptr; // toolbar derecha: Mover (texto genérico)

    // Acciones de sub-toolbars
    QAction* actUpLeft_  = nullptr; // atras izquierda
    QAction* actUpRight_ = nullptr; // atras derecha

    // Sub-toolbars
    QToolBar* leftPaneBar_  = nullptr;
    QToolBar* rightPaneBar_ = nullptr;
    QMenu*     rightContextMenu_ = nullptr;
    QMenu*     leftContextMenu_  = nullptr;

    // Cola de transferencias
    class TransferManager* transferMgr_ = nullptr;
    class TransferQueueDialog* transferDlg_ = nullptr;
    QAction* actShowQueue_ = nullptr;
    QAction* actSites_     = nullptr; // gestor de sitios

    // Menú superior
    QMenu* appMenu_    = nullptr; // OpenSCP
    QMenu* fileMenu_   = nullptr; // Archivo
    QAction* actAbout_ = nullptr;
    QAction* actPrefs_ = nullptr;
    QAction* actQuit_  = nullptr;

    // descargas
    QString downloadDir_; // última carpeta local elegida para descargas
    QString uploadDir_;   // última carpeta local elegida para subidas

    // Confirmación de huella (TOFU)
    bool confirmHostKeyUI(const QString& host, quint16 port, const QString& algorithm, const QString& fingerprint);

    // Helpers de conexión y de armado de UI remota
    bool establishSftpAsync(openscp::SessionOptions opt, std::string& err);
    void applyRemoteConnectedUI(const openscp::SessionOptions& opt);

    // Estado de escritura en el directorio remoto actual
    bool rightRemoteWritable_ = false;
    // Recalcula si el directorio remoto actual es escribible (crea/borra una carpeta temporal)
    void updateRemoteWriteability();

    bool firstShow_ = true;

    // Preferencias de usuario
    bool prefShowHidden_ = false;
    bool prefSingleClick_ = false;
    QMetaObject::Connection leftClickConn_;
    QMetaObject::Connection rightClickConn_;
};
