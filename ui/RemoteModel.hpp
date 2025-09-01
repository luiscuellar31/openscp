#pragma once
#include <QAbstractListModel>
#include <vector>
#include <memory>
#include "openscp/SftpClient.hpp"

class RemoteModel : public QAbstractListModel {
  Q_OBJECT
public:
  explicit RemoteModel(openscp::SftpClient* client, QObject* parent = nullptr);

  int rowCount(const QModelIndex& parent = QModelIndex()) const override;
  QVariant data(const QModelIndex& index, int role) const override;
  Qt::ItemFlags flags(const QModelIndex& index) const override;

  bool setRootPath(const QString& path, QString* errorOut = nullptr);
  QString rootPath() const { return currentPath_; }

  bool isDir(const QModelIndex& idx) const;
  QString nameAt(const QModelIndex& idx) const;

private:
  openscp::SftpClient* client_ = nullptr; // no owned
  QString currentPath_;
  struct Item { QString name; bool isDir; quint64 size; quint64 mtime; };
  std::vector<Item> items_;
};
