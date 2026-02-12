// Site manager: list, add/edit/remove, and select to connect.
#pragma once
#include <QDialog>
#include <QVector>
#include <QString>
#include "openscp/SftpTypes.hpp"

class QTableWidget;
class QPushButton;

struct SiteEntry {
    QString id;
    QString name;
    openscp::SessionOptions opt;
};

class SiteManagerDialog : public QDialog {
    Q_OBJECT
public:
    explicit SiteManagerDialog(QWidget* parent = nullptr);
    bool selectedOptions(openscp::SessionOptions& out) const;

private slots:
    void onAdd();
    void onEdit();
    void onRemove();
    void onConnect();
    void updateButtons();

private:
    void loadSites();
    void saveSites();
    void refresh();

    QVector<SiteEntry> sites_;
    QTableWidget* table_ = nullptr;
    int selectedRow_ = -1;
    QPushButton* btAdd_ = nullptr;
    QPushButton* btEdit_ = nullptr;
    QPushButton* btDel_  = nullptr;
    QPushButton* btConn_ = nullptr;
    QPushButton* btClose_= nullptr;
};
