// Read-only model to list remote entries via SftpClient.
#pragma once
#include "openscp/SftpClient.hpp"
#include <QAbstractTableModel>
#include <atomic>
#include <memory>
#include <optional>
#include <vector>

class RemoteModel : public QAbstractTableModel {
    Q_OBJECT
    public:
    explicit RemoteModel(openscp::SftpClient *client,
                         QObject *parent = nullptr);

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

    // Set the current remote directory and refresh rows.
    // By default this is asynchronous and emits rootPathLoaded on completion.
    bool setRootPath(const QString &path, QString *errorOut = nullptr,
                     bool async = true);
    QString rootPath() const { return currentPath_; }
    void setSessionOptions(const openscp::SessionOptions &opt) {
        sessionOpt_ = opt;
    }

    bool isDir(const QModelIndex &idx) const;
    QString nameAt(const QModelIndex &idx) const;
    bool hasSize(const QModelIndex &idx) const;
    quint64 sizeAt(const QModelIndex &idx) const;
    void setShowHidden(bool v) { showHidden_ = v; }
    bool showHidden() const { return showHidden_; }

    // Enumeration support for staging folders
    struct EnumeratedFile {
        QString remotePath;   // full remote path ("/base/sub/file")
        QString relativePath; // relative to the enumerated base ("sub/file")
        quint64 size = 0;     // bytes
        bool hasSize = false; // true if size is known
    };
    struct EnumOptions {
        bool skipSymlinks = true;           // skip symlinks by default
        std::atomic_bool *cancel = nullptr; // cooperative cancel flag
        int maxDepth = 32;                  // maximum recursion depth
    };
    // Recursively enumerate files under `baseRemote` (directories only).
    // Returns true if finished without fatal error. partialErrorOut is set to
    // true if some branches failed.
    bool enumerateFilesUnderEx(const QString &baseRemote,
                               std::vector<EnumeratedFile> &out,
                               const EnumOptions &opt, bool *partialErrorOut,
                               bool *someSizeUnknownOut,
                               quint64 *dirCountOut = nullptr,
                               quint64 *symlinkSkippedOut = nullptr,
                               quint64 *deniedCountOut = nullptr,
                               quint64 *unknownSizeCountOut = nullptr) const;
    // Backward-compatible simple enumeration (no cancel/skip control)
    bool enumerateFilesUnder(const QString &baseRemote,
                             std::vector<EnumeratedFile> &out,
                             QString *errorOut = nullptr) const;

    signals:
    void rootPathLoaded(const QString &path, bool ok, const QString &error);

    private:
    openscp::SftpClient *client_ = nullptr; // no owned
    QString currentPath_;
    struct Item {
        QString name;
        bool isDir;
        quint64 size;
        bool hasSize;
        quint64 mtime;
        quint32 mode;
        quint32 uid;
        quint32 gid;
    };
    std::vector<Item> items_;
    bool showHidden_ = false; // hide names starting with '.' if false
    int sortColumn_ = 0;
    Qt::SortOrder sortOrder_ = Qt::AscendingOrder;
    std::optional<openscp::SessionOptions> sessionOpt_;
    std::atomic<quint64> listRequestSeq_{0};

    void replaceItems(std::vector<Item> &&nextItems, const QString &path);
    void sortItemsVector(std::vector<Item> &items, int column,
                         Qt::SortOrder order) const;
};
