// Modelo de solo-lectura para listar entradas remotas vía SftpClient.
#pragma once
#include <QAbstractTableModel>
#include <vector>
#include <memory>
#include "openscp/SftpClient.hpp"

class RemoteModel : public QAbstractTableModel {
    Q_OBJECT
public:
    explicit RemoteModel(openscp::SftpClient* client, QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override { Q_UNUSED(parent); return 4; }
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    QStringList mimeTypes() const override;
    QMimeData* mimeData(const QModelIndexList& indexes) const override;
    Qt::DropActions supportedDragActions() const override { return Qt::CopyAction; }
    void sort(int column, Qt::SortOrder order) override;

    // Fija el directorio remoto actual y actualiza filas.
    bool setRootPath(const QString& path, QString* errorOut = nullptr);
    QString rootPath() const { return currentPath_; }

    bool isDir(const QModelIndex& idx) const;
    QString nameAt(const QModelIndex& idx) const;

private:
    openscp::SftpClient* client_ = nullptr; // no owned
    QString currentPath_;
    struct Item {
        QString name;
        bool isDir;
        quint64 size;
        quint64 mtime;
        quint32 mode;
        quint32 uid;
        quint32 gid;
    };
    std::vector<Item> items_;
};
