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
    const auto &item = items_[index.row()];
    const bool isLink = (item.mode & 0120000u) == 0120000u; // S_IFLNK
    if (role == Qt::DecorationRole && index.column() == 0) {
        return iconForRemoteEntry(item.name, item.isDir, isLink);
    }
    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case 0: {
            QString suffix;
            if (isLink)
                suffix = "@";
            else if (item.isDir)
                suffix = "/";
            return item.name + suffix;
        }
        case 1:
            if (item.isDir)
                return QVariant();
            if (!item.hasSize)
                return QStringLiteral("—");
            return QLocale().formattedDataSize((qint64)item.size, 1,
                                               QLocale::DataSizeIecFormat);
        case 2:
            if (item.mtime > 0)
                return openscpui::localShortTime(item.mtime);
            else
                return QVariant();
        case 3: {
            // Permissions in rwxr-xr-x style
            QString permissionsText(10, '-');
            const quint32 modeBits = item.mode;
            // file type
            bool isSymlink = (modeBits & 0120000u) == 0120000u;
            permissionsText[0] = isSymlink ? 'l' : (item.isDir ? 'd' : '-');
            auto setPermissionBit = [&](int position, quint32 mask,
                                        QChar flag) {
                if (modeBits & mask)
                    permissionsText[position] = flag;
            };
            setPermissionBit(1, 0400, 'r');
            setPermissionBit(2, 0200, 'w');
            setPermissionBit(3, 0100, 'x');
            setPermissionBit(4, 0040, 'r');
            setPermissionBit(5, 0020, 'w');
            setPermissionBit(6, 0010, 'x');
            setPermissionBit(7, 0004, 'r');
            setPermissionBit(8, 0002, 'w');
            setPermissionBit(9, 0001, 'x');
            return permissionsText;
        }
        }
    }
    if (role == Qt::ToolTipRole) {
        const auto &item = items_[index.row()];
        if (item.isDir)
            return tr("Folder");
        if (!item.hasSize) {
            return tr("Size: unknown (not provided by the server)");
        }
        QString tip = tr("File");
        const QString human = QLocale().formattedDataSize(
            (qint64)item.size, 1, QLocale::DataSizeIecFormat);
        const QString bytes = QLocale().toString((qulonglong)item.size);
        tip += QString(" • %1 (%2 bytes)").arg(human, bytes);
        if (item.mtime > 0)
            tip += " • " + openscpui::localShortTime(item.mtime);
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
        std::vector<openscp::FileInfo> remoteEntries;
        std::string listError;
        if (!client_->list(normalized.toStdString(), remoteEntries, listError)) {
            const QString listErrorText = QString::fromStdString(listError);
            if (errorOut)
                *errorOut = listErrorText;
            return false;
        }

        std::vector<Item> nextItems;
        nextItems.reserve(remoteEntries.size());
        for (const auto &fileInfo : remoteEntries) {
            const QString name = QString::fromStdString(fileInfo.name);
            if (!showHiddenNow && name.startsWith('.'))
                continue;
            nextItems.push_back(
                {name, fileInfo.is_dir, fileInfo.size, fileInfo.has_size,
                 fileInfo.mtime, fileInfo.mode, fileInfo.uid, fileInfo.gid});
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
    const openscp::SessionOptions sessionOptions = *sessionOpt_;
    QPointer<RemoteModel> self(this);
    std::thread([self, reqId, normalized, showHiddenNow, sortColNow,
                 sortOrdNow, baseClient, sessionOptions]() mutable {
        std::vector<openscp::FileInfo> remoteEntries;
        std::string listError;
        bool listOk = false;
        QString listErrorText;

        std::unique_ptr<openscp::SftpClient> listClient;
        std::string connErr;
        if (!self || !baseClient) {
            listErrorText = QStringLiteral("Remote model is no longer available");
        } else {
            listClient = baseClient->newConnectionLike(sessionOptions, connErr);
            if (!listClient) {
                listErrorText = connErr.empty()
                                    ? QStringLiteral(
                                          "Could not start remote listing")
                                    : QString::fromStdString(connErr);
            } else {
                listOk =
                    listClient->list(normalized.toStdString(), remoteEntries,
                                     listError);
                if (!listOk)
                    listErrorText = QString::fromStdString(listError);
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
            [self, reqId, normalized, listOk, listErrorText,
             remoteEntries = std::move(remoteEntries),
             showHiddenNow, sortColNow, sortOrdNow]() mutable {
                if (!self)
                    return;
                if (reqId != self->listRequestSeq_.load())
                    return;
                if (!listOk) {
                    emit self->rootPathLoaded(normalized, false, listErrorText);
                    return;
                }
                std::vector<Item> nextItems;
                nextItems.reserve(remoteEntries.size());
                for (const auto &fileInfo : remoteEntries) {
                    const QString name = QString::fromStdString(fileInfo.name);
                    if (!showHiddenNow && name.startsWith('.'))
                        continue;
                    nextItems.push_back({name, fileInfo.is_dir, fileInfo.size,
                                         fileInfo.has_size, fileInfo.mtime,
                                         fileInfo.mode, fileInfo.uid,
                                         fileInfo.gid});
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
    auto lessCaseInsensitive = [&](const QString &leftName,
                                   const QString &rightName) {
        int cmp = QString::compare(leftName, rightName, Qt::CaseInsensitive);
        return asc ? (cmp < 0) : (cmp > 0);
    };
    auto compareItems = [&](const Item &leftItem, const Item &rightItem) {
        if (leftItem.isDir != rightItem.isDir)
            return leftItem.isDir && !rightItem.isDir;
        switch (column) {
        case 0:
            return lessCaseInsensitive(leftItem.name, rightItem.name);
        case 1:
            return asc ? (leftItem.size < rightItem.size)
                       : (leftItem.size > rightItem.size);
        case 2:
            return asc ? (leftItem.mtime < rightItem.mtime)
                       : (leftItem.mtime > rightItem.mtime);
        case 3:
            return asc ? (leftItem.mode < rightItem.mode)
                       : (leftItem.mode > rightItem.mode);
        default:
            return lessCaseInsensitive(leftItem.name, rightItem.name);
        }
    };
    std::sort(items.begin(), items.end(), compareItems);
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
        QSettings settings("OpenSCP", "OpenSCP");
        configuredMaxDepth = settings.value("Advanced/maxFolderDepth", 32).toInt();
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
    bool hasPartialErrors = false;
    bool hasUnknownSizes = false;
    EnumOptions options; // defaults
    bool enumerationOk = enumerateFilesUnderEx(baseRemote, out, options,
                                               &hasPartialErrors,
                                               &hasUnknownSizes);
    if (!enumerationOk && errorOut)
        *errorOut = tr("Enumeration error");
    if (hasPartialErrors && errorOut)
        *errorOut = tr("Partial enumeration with errors");
    return enumerationOk && !hasPartialErrors;
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
