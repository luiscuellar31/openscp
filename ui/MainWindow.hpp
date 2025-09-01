#pragma once
#include <QMainWindow>
#include <QFileSystemModel>
#include <QTreeView>
#include <QLineEdit>
#include <QAction>
#include <memory>

class RemoteModel;              // fwd
class QModelIndex;              // fwd para la firma del slot
namespace openscp { class SftpClient; } // fwd (usamos dtor en .cpp)

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow();

private slots:
    void chooseLeftDir();
    void chooseRightDir();
    void leftPathEntered();
    void rightPathEntered();
    void copyLeftToRight(); // F5
    void moveLeftToRight(); // F6
    void deleteFromLeft();  // Supr

    // Remoto (mock por ahora)
    void connectSftp();
    void disconnectSftp();
    void rightItemActivated(const QModelIndex& idx); // doble click en remoto

private:
    // Estado remoto
    std::unique_ptr<openscp::SftpClient> sftp_; // <-- SOLO UNA
    bool rightIsRemote_ = false;

    void setLeftRoot(const QString& path);
    void setRightRoot(const QString& path);       // local
    void setRightRemoteRoot(const QString& path); // remoto

    // Modelos
    QFileSystemModel* leftModel_        = nullptr;
    QFileSystemModel* rightLocalModel_  = nullptr;
    RemoteModel*      rightRemoteModel_ = nullptr;

    // Vistas/paths
    QTreeView* leftView_  = nullptr;
    QTreeView* rightView_ = nullptr;

    QLineEdit* leftPath_  = nullptr;
    QLineEdit* rightPath_ = nullptr;

    // Acciones
    QAction* actChooseLeft_  = nullptr;
    QAction* actChooseRight_ = nullptr;
    QAction* actCopyF5_      = nullptr;
    QAction* actMoveF6_      = nullptr;
    QAction* actDelete_      = nullptr;
    QAction* actConnect_     = nullptr;
    QAction* actDisconnect_  = nullptr;
};