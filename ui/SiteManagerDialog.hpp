// Site manager: list, add/edit/remove, and select to connect.
#pragma once
#include "SiteEntry.hpp"
#include <QDialog>
#include <QString>
#include <QVector>

class QLineEdit;
class QPushButton;
class QSortFilterProxyModel;
class QTableView;
class SiteListModel;

class SiteManagerDialog : public QDialog {
    Q_OBJECT
    public:
    explicit SiteManagerDialog(QWidget *parent = nullptr);
    bool selectedOptions(openscp::SessionOptions &out) const;
    bool selectedSite(SiteEntry &out) const;
    void reloadFromSettings();

    private slots:
    void onAdd();
    void onEdit();
    void onDuplicate();
    void onRemove();
    void onConnect();
    void updateButtons();

    private:
    void loadSites();
    bool saveSites();
    void refresh();
    int selectedSiteIndex() const;
    void selectSiteIndex(int siteIndex);

    QVector<SiteEntry> sites_;
    SiteListModel *model_ = nullptr;
    QSortFilterProxyModel *proxy_ = nullptr;
    QTableView *table_ = nullptr;
    QLineEdit *search_ = nullptr;
    QPushButton *btAdd_ = nullptr;
    QPushButton *btEdit_ = nullptr;
    QPushButton *btDuplicate_ = nullptr;
    QPushButton *btDel_ = nullptr;
    QPushButton *btConn_ = nullptr;
    QPushButton *btClose_ = nullptr;
    bool legacySecretMigrationBlocked_ = false;
};
