// Remote model implementation (table: Name, Size, Date, Permissions).
#include "RemoteModel.hpp"
#include "MainWindowSharedUtils.hpp"
#include "RemoteWalker.hpp"
#include "TimeUtils.hpp"
#include "openscp/RuntimeLogging.hpp"
#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QIcon>
#include <QLoggingCategory>
#include <QMetaObject>
#include <QMimeDatabase>
#include <QMimeType>
#include <QPointer>
#include <QSet>
#include <QStyle>
#include <QVariant>
#include <algorithm>
#include <functional>
#include <thread>

Q_LOGGING_CATEGORY(ocEnum, "openscp.enum")
#include <QDir>
#include <QLocale>
#include <QMimeData>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>

RemoteModel::RemoteModel(openscp::SftpClient *client, QObject *parent)
    : QAbstractTableModel(parent), client_(client) {}

int RemoteModel::rowCount(const QModelIndex &parent) const {
    if (parent.isValid())
        return 0;
    return static_cast<int>(items_.size());
}

static QIcon remoteFolderIcon() {
    static const QIcon icon = [] {
        QIcon themed = QIcon::fromTheme(QStringLiteral("folder"));
        if (!themed.isNull())
            return themed;
        return QApplication::style()
                   ? QApplication::style()->standardIcon(QStyle::SP_DirIcon)
                   : QIcon();
    }();
    return icon;
}

static QIcon remoteFileIcon() {
    static const QIcon icon = [] {
        QIcon themed = QIcon::fromTheme(QStringLiteral("text-x-generic"));
        if (!themed.isNull())
            return themed;
        return QApplication::style()
                   ? QApplication::style()->standardIcon(QStyle::SP_FileIcon)
                   : QIcon();
    }();
    return icon;
}

static QIcon remoteLinkIcon() {
    static const QIcon icon = [] {
        QIcon themed = QIcon::fromTheme(QStringLiteral("emblem-symbolic-link"));
        if (!themed.isNull())
            return themed;
        return QApplication::style()
                   ? QApplication::style()->standardIcon(
                         QStyle::SP_FileLinkIcon)
                   : QIcon();
    }();
    return icon;
}

static QIcon iconFromMimeTheme(const QString &name) {
    static QMimeDatabase mimeDb;
    const QMimeType mt =
        mimeDb.mimeTypeForFile(name, QMimeDatabase::MatchExtension);
    if (!mt.isValid())
        return QIcon();

    QIcon icon = QIcon::fromTheme(mt.iconName());
    if (icon.isNull() && !mt.genericIconName().isEmpty()) {
        icon = QIcon::fromTheme(mt.genericIconName());
    }
    return icon;
}

static QIcon iconForRemoteEntry(const QString &name, bool isDir, bool isLink) {
    QString key;
    if (isLink) {
        key = QStringLiteral("__link");
    } else if (isDir) {
        key = QStringLiteral("__dir");
    } else {
        const QString ext = QFileInfo(name).completeSuffix().toLower();
        key = ext.isEmpty() ? QStringLiteral("__file")
                            : QStringLiteral("ext:") + ext;
    }

    static QHash<QString, QIcon> cache;
    if (cache.contains(key))
        return cache.value(key);

    QIcon icon;
#ifdef Q_OS_MAC
    static QFileIconProvider provider;
    if (isDir) {
        icon = provider.icon(QFileIconProvider::Folder);
    } else if (!isLink) {
        const QString ext = QFileInfo(name).completeSuffix().toLower();
        QString probeName = QStringLiteral("remote-entry");
        if (!ext.isEmpty())
            probeName += QStringLiteral(".") + ext;
        icon = provider.icon(QFileInfo(probeName));
    }
#endif
    if (icon.isNull() && isLink) {
        icon = remoteLinkIcon();
    }
    if (icon.isNull() && isDir) {
        icon = remoteFolderIcon();
    }
    if (icon.isNull() && !isDir && !isLink) {
        icon = iconFromMimeTheme(name);
    }
    if (icon.isNull() && !isDir && !isLink) {
        icon = remoteFileIcon();
    }
    if (icon.isNull()) {
        icon = remoteFileIcon();
    }

    cache.insert(key, icon);
    return icon;
}

QVariant RemoteModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 ||
        index.row() >= (int)items_.size())
        return {};
    const auto &it = items_[index.row()];
    const bool isLnk = (it.mode & 0120000u) == 0120000u; // S_IFLNK
    if (role == Qt::DecorationRole && index.column() == 0) {
        return iconForRemoteEntry(it.name, it.isDir, isLnk);
    }
    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case 0: {
            QString suffix;
            if (isLnk)
                suffix = "@";
            else if (it.isDir)
                suffix = "/";
            return it.name + suffix;
        }
        case 1:
            if (it.isDir)
                return QVariant();
            if (!it.hasSize)
                return QStringLiteral("—");
            return QLocale().formattedDataSize((qint64)it.size, 1,
                                               QLocale::DataSizeIecFormat);
        case 2:
            if (it.mtime > 0)
                return openscpui::localShortTime(it.mtime);
            else
                return QVariant();
        case 3: {
            // Permissions in rwxr-xr-x style
            QString s(10, '-');
            const quint32 m = it.mode;
            // file type
            bool isLnk = (m & 0120000u) == 0120000u;
            s[0] = isLnk ? 'l' : (it.isDir ? 'd' : '-');
            auto bit = [&](int pos, quint32 mask, QChar ch) {
                if (m & mask)
                    s[pos] = ch;
            };
            bit(1, 0400, 'r');
            bit(2, 0200, 'w');
            bit(3, 0100, 'x');
            bit(4, 0040, 'r');
            bit(5, 0020, 'w');
            bit(6, 0010, 'x');
            bit(7, 0004, 'r');
            bit(8, 0002, 'w');
            bit(9, 0001, 'x');
            return s;
        }
        }
    }
    if (role == Qt::ToolTipRole) {
        const auto &it = items_[index.row()];
        if (it.isDir)
            return tr("Folder");
        if (!it.hasSize) {
            return tr("Size: unknown (not provided by the server)");
        }
        QString tip = tr("File");
        const QString human = QLocale().formattedDataSize(
            (qint64)it.size, 1, QLocale::DataSizeIecFormat);
        const QString bytes = QLocale().toString((qulonglong)it.size);
        tip += QString(" • %1 (%2 bytes)").arg(human, bytes);
        if (it.mtime > 0)
            tip += " • " + openscpui::localShortTime(it.mtime);
        return tip;
    }
    return {};
}

Qt::ItemFlags RemoteModel::flags(const QModelIndex &index) const {
    if (!index.isValid())
        return Qt::NoItemFlags;
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled;
}

bool RemoteModel::setRootPath(const QString &path, QString *errorOut,
                              bool async) {
    if (!client_) {
        if (errorOut)
            *errorOut = tr("No remote client available");
        return false;
    }

    const QString normalized = normalizeRemotePath(path);

    const quint64 reqId = ++listRequestSeq_;
    const bool showHiddenNow = showHidden_;
    const int sortColNow = sortColumn_;
    const Qt::SortOrder sortOrdNow = sortOrder_;

    if (!async) {
        std::vector<openscp::FileInfo> out;
        std::string err;
        if (!client_->list(normalized.toStdString(), out, err)) {
            const QString qerr = QString::fromStdString(err);
            if (errorOut)
                *errorOut = qerr;
            return false;
        }

        std::vector<Item> nextItems;
        nextItems.reserve(out.size());
        for (const auto &f : out) {
            const QString name = QString::fromStdString(f.name);
            if (!showHiddenNow && name.startsWith('.'))
                continue;
            nextItems.push_back({name, f.is_dir, f.size, f.has_size, f.mtime,
                                 f.mode, f.uid, f.gid});
        }
        sortItemsVector(nextItems, sortColNow, sortOrdNow);
        replaceItems(std::move(nextItems), normalized);
        return true;
    }

    if (!sessionOpt_.has_value()) {
        if (errorOut)
            *errorOut = tr("Missing session options for remote listing");
        return false;
    }

    openscp::SftpClient *baseClient = client_;
    const openscp::SessionOptions optNow = *sessionOpt_;
    QPointer<RemoteModel> self(this);
    std::thread([self, reqId, normalized, showHiddenNow, sortColNow,
                 sortOrdNow, baseClient, optNow]() mutable {
        std::vector<openscp::FileInfo> out;
        std::string err;
        bool ok = false;
        QString qerr;

        std::unique_ptr<openscp::SftpClient> listClient;
        std::string connErr;
        if (!self || !baseClient) {
            qerr = QStringLiteral("Remote model is no longer available");
        } else {
            listClient = baseClient->newConnectionLike(optNow, connErr);
            if (!listClient) {
                qerr = connErr.empty()
                           ? QStringLiteral("Could not start remote listing")
                           : QString::fromStdString(connErr);
            } else {
                ok = listClient->list(normalized.toStdString(), out, err);
                if (!ok)
                    qerr = QString::fromStdString(err);
            }
        }
        if (listClient)
            listClient->disconnect();

        if (!self)
            return;
        QObject *app = QCoreApplication::instance();
        if (!app)
            return;
        QMetaObject::invokeMethod(
            app,
            [self, reqId, normalized, ok, qerr, out = std::move(out),
             showHiddenNow, sortColNow, sortOrdNow]() mutable {
                if (!self)
                    return;
                if (reqId != self->listRequestSeq_.load())
                    return;
                if (!ok) {
                    emit self->rootPathLoaded(normalized, false, qerr);
                    return;
                }
                std::vector<Item> nextItems;
                nextItems.reserve(out.size());
                for (const auto &f : out) {
                    const QString name = QString::fromStdString(f.name);
                    if (!showHiddenNow && name.startsWith('.'))
                        continue;
                    nextItems.push_back(
                        {name, f.is_dir, f.size, f.has_size, f.mtime, f.mode,
                         f.uid, f.gid});
                }
                self->sortItemsVector(nextItems, sortColNow, sortOrdNow);
                self->replaceItems(std::move(nextItems), normalized);
                emit self->rootPathLoaded(normalized, true, QString());
            },
            Qt::QueuedConnection);
    }).detach();
    return true;
}

void RemoteModel::replaceItems(std::vector<Item> &&nextItems,
                               const QString &path) {
    const int oldCount = static_cast<int>(items_.size());
    if (oldCount > 0) {
        beginRemoveRows(QModelIndex(), 0, oldCount - 1);
        items_.clear();
        endRemoveRows();
    } else {
        items_.clear();
    }
    const int newCount = static_cast<int>(nextItems.size());
    if (newCount > 0) {
        beginInsertRows(QModelIndex(), 0, newCount - 1);
        items_ = std::move(nextItems);
        endInsertRows();
    }
    currentPath_ = path;
}

void RemoteModel::sortItemsVector(std::vector<Item> &items, int column,
                                  Qt::SortOrder order) const {
    const bool asc = (order == Qt::AscendingOrder);
    auto lessStr = [&](const QString &a, const QString &b) {
        int cmp = QString::compare(a, b, Qt::CaseInsensitive);
        return asc ? (cmp < 0) : (cmp > 0);
    };
    auto less = [&](const Item &a, const Item &b) {
        if (a.isDir != b.isDir)
            return a.isDir && !b.isDir;
        switch (column) {
        case 0:
            return lessStr(a.name, b.name);
        case 1:
            return asc ? (a.size < b.size) : (a.size > b.size);
        case 2:
            return asc ? (a.mtime < b.mtime) : (a.mtime > b.mtime);
        case 3:
            return asc ? (a.mode < b.mode) : (a.mode > b.mode);
        default:
            return lessStr(a.name, b.name);
        }
    };
    std::sort(items.begin(), items.end(), less);
}

bool RemoteModel::isDir(const QModelIndex &idx) const {
    if (!idx.isValid())
        return false;
    return items_[idx.row()].isDir;
}

QString RemoteModel::nameAt(const QModelIndex &idx) const {
    if (!idx.isValid())
        return {};
    return items_[idx.row()].name;
}

bool RemoteModel::hasSize(const QModelIndex &idx) const {
    if (!idx.isValid())
        return false;
    return items_[idx.row()].hasSize;
}

quint64 RemoteModel::sizeAt(const QModelIndex &idx) const {
    if (!idx.isValid())
        return 0;
    return items_[idx.row()].size;
}

QVariant RemoteModel::headerData(int section, Qt::Orientation orientation,
                                 int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case 0:
        return tr("Name");
    case 1:
        return tr("Size");
    case 2:
        return tr("Date");
    case 3:
        return tr("Permissions");
    }
    return {};
}

QStringList RemoteModel::mimeTypes() const {
    // Prefer native file URLs so Finder/Explorer can accept external drags
    return {QStringLiteral("text/uri-list")};
}

QMimeData *RemoteModel::mimeData(const QModelIndexList &indexes) const {
    Q_UNUSED(indexes);
    // Do not perform synchronous downloads here. DragAwareTreeView handles
    // asynchronous staging and will create the final QMimeData when ready.
    // Return an empty QMimeData so default drag handlers can still proceed
    // (they won't be used for remote model in our customized view).
    return new QMimeData();
}

bool RemoteModel::enumerateFilesUnderEx(
    const QString &baseRemote, std::vector<EnumeratedFile> &out,
    const EnumOptions &opt, bool *partialErrorOut, bool *someSizeUnknownOut,
    quint64 *dirCountOut, quint64 *symlinkSkippedOut, quint64 *deniedCountOut,
    quint64 *unknownSizeCountOut) const {
    if (partialErrorOut)
        *partialErrorOut = false;
    if (someSizeUnknownOut)
        *someSizeUnknownOut = false;
    if (!client_) {
        return false;
    }

    // Resolve max depth from settings if not provided or invalid
    int configuredMaxDepth = opt.maxDepth;
    if (configuredMaxDepth <= 0) {
        QSettings s("OpenSCP", "OpenSCP");
        configuredMaxDepth = s.value("Advanced/maxFolderDepth", 32).toInt();
        if (configuredMaxDepth < 1)
            configuredMaxDepth = 32;
    }

    RemoteWalker::Options walkerOptions;
    walkerOptions.includeHidden = showHidden_;
    walkerOptions.skipSymlinks = opt.skipSymlinks;
    walkerOptions.sanitizeRelativePath = true;
    walkerOptions.maxDepth = configuredMaxDepth;
    walkerOptions.cancel = opt.cancel;
    walkerOptions.onListError = [](const QString &remotePath,
                                   const QString &errorText) {
        if (openscp::sensitiveLoggingEnabled()) {
            qWarning(ocEnum) << "enumeration error at" << remotePath << ":"
                             << errorText;
        } else {
            qWarning(ocEnum) << "enumeration error during remote listing";
        }
    };

    RemoteWalker::Stats walkerStats;
    const bool walkOk = RemoteWalker::walk(
        client_, baseRemote, walkerOptions,
        [&out](const RemoteWalker::Entry &entry) {
            out.push_back(EnumeratedFile{entry.remotePath, entry.relativePath,
                                         static_cast<quint64>(
                                             entry.fileInfo.size),
                                         entry.fileInfo.has_size});
        },
        &walkerStats);

    if (partialErrorOut)
        *partialErrorOut = walkerStats.partialError;
    if (someSizeUnknownOut)
        *someSizeUnknownOut = walkerStats.unknownSizeCount > 0;
    if (dirCountOut)
        *dirCountOut = walkerStats.visitedDirectoryCount;
    if (symlinkSkippedOut)
        *symlinkSkippedOut = walkerStats.skippedSymlinkCount;
    if (deniedCountOut)
        *deniedCountOut = walkerStats.listFailureCount;
    if (unknownSizeCountOut)
        *unknownSizeCountOut = walkerStats.unknownSizeCount;

    return walkOk;
}

bool RemoteModel::enumerateFilesUnder(const QString &baseRemote,
                                      std::vector<EnumeratedFile> &out,
                                      QString *errorOut) const {
    bool partial = false, unk = false;
    EnumOptions opt; // defaults
    bool ok = enumerateFilesUnderEx(baseRemote, out, opt, &partial, &unk);
    if (!ok && errorOut)
        *errorOut = tr("Enumeration error");
    if (partial && errorOut)
        *errorOut = tr("Partial enumeration with errors");
    return ok && !partial;
}

void RemoteModel::sort(int column, Qt::SortOrder order) {
    sortColumn_ = column;
    sortOrder_ = order;
    if (items_.empty())
        return;
    emit layoutAboutToBeChanged();
    sortItemsVector(items_, sortColumn_, sortOrder_);
    emit layoutChanged();
}
