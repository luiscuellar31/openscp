// Read-only, data-only model for remote entries.
#pragma once
#include "openscp/RemoteFileInfo.hpp"

#include <QAbstractTableModel>
#include <QCache>
#include <QIcon>
#include <QLocale>

#include <vector>

class RemoteModel : public QAbstractTableModel {
    Q_OBJECT
    public:
    explicit RemoteModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override {
        Q_UNUSED(parent);
        return 4;
    }
    QVariant data(const QModelIndex &index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    QStringList mimeTypes() const override;
    QMimeData *mimeData(const QModelIndexList &indexes) const override;
    Qt::DropActions supportedDragActions() const override {
        return Qt::CopyAction;
    }
    void sort(int column, Qt::SortOrder order) override;
    // Data-only update path used by RemoteOperationController.
    void setEntries(const QString &path,
                    const std::vector<openscp::FileInfo> &entries);
    void setLoading(const QString &path);
    void clearLoading();
    bool isLoading() const { return loading_; }

    QString rootPath() const { return currentPath_; }

    bool isDir(const QModelIndex &index) const;
    QString nameAt(const QModelIndex &index) const;
    bool hasSize(const QModelIndex &index) const;
    quint64 sizeAt(const QModelIndex &index) const;
    void setShowHidden(bool showHiddenEnabled) {
        showHidden_ = showHiddenEnabled;
    }
    bool showHidden() const { return showHidden_; }

    signals:
    void rootPathLoaded(const QString &path, bool loadOk, const QString &error);

    private:
    QString currentPath_;
    struct Item {
        QString name;
        bool isDir;
        bool isLink;
        quint64 size;
        bool hasSize;
        quint64 mtime;
        quint32 mode;
        quint32 uid;
        quint32 gid;
        QString permissions;
    };
    std::vector<Item> items_;
    QLocale locale_;
    mutable QCache<QString, QIcon> iconCache_;
    mutable QString cachedIconTheme_;
    bool showHidden_ = false; // hide names starting with '.' if false
    bool loading_ = false;
    int sortColumn_ = 0;
    Qt::SortOrder sortOrder_ = Qt::AscendingOrder;

    const Item *itemForIndex(const QModelIndex &index) const;
    QIcon iconForRemoteEntry(const Item &item) const;
    void sortItemsVector(std::vector<Item> &items, int column,
                         Qt::SortOrder order) const;
};
