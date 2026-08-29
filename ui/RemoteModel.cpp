// Remote model implementation (table: Name, Size, Date, Permissions).
#include "RemoteModel.hpp"

#include "MainWindowSharedUtils.hpp"
#include "TimeUtils.hpp"

#include <QApplication>
#include <QDateTime>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QIcon>
#include <QLocale>
#include <QMimeData>
#include <QMimeDatabase>
#include <QMimeType>
#include <QStyle>
#include <QVariant>

#include <algorithm>

RemoteModel::RemoteModel(QObject *parent) : QAbstractTableModel(parent) {
}

int RemoteModel::rowCount(const QModelIndex &parent) const {
    if (parent.isValid())
        return 0;
    if (loading_)
        return 1;
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
        return QApplication::style() ? QApplication::style()->standardIcon(
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
    if (loading_) {
        if (index.isValid() && index.row() == 0 && index.column() == 0 &&
            role == Qt::DisplayRole) {
            return tr("Loading…");
        }
        return {};
    }
    const Item *entry = itemForIndex(index);
    if (!entry)
        return {};
    const Item &item = *entry;
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
    if (loading_)
        return Qt::ItemIsEnabled;
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled;
}

void RemoteModel::setLoading(const QString &path) {
    beginResetModel();
    items_.clear();
    currentPath_ = normalizeRemotePath(path);
    loading_ = true;
    endResetModel();
}

void RemoteModel::clearLoading() {
    if (!loading_)
        return;
    beginResetModel();
    loading_ = false;
    endResetModel();
}

void RemoteModel::setEntries(const QString &path,
                             const std::vector<openscp::FileInfo> &entries) {
    std::vector<Item> nextItems;
    nextItems.reserve(entries.size());
    for (const auto &fileInfo : entries) {
        const QString name = QString::fromStdString(fileInfo.name);
        if (!showHidden_ && name.startsWith('.'))
            continue;
        nextItems.push_back({name, fileInfo.is_dir, fileInfo.size,
                             fileInfo.has_size, fileInfo.mtime, fileInfo.mode,
                             fileInfo.uid, fileInfo.gid});
    }
    sortItemsVector(nextItems, sortColumn_, sortOrder_);
    beginResetModel();
    loading_ = false;
    items_ = std::move(nextItems);
    currentPath_ = normalizeRemotePath(path);
    endResetModel();
    emit rootPathLoaded(currentPath_, true, QString());
}

void RemoteModel::replaceItems(std::vector<Item> &&nextItems,
                               const QString &path) {
    loading_ = false;
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

const RemoteModel::Item *
RemoteModel::itemForIndex(const QModelIndex &index) const {
    if (!index.isValid() || index.model() != this)
        return nullptr;
    const int row = index.row();
    if (row < 0 || row >= static_cast<int>(items_.size()))
        return nullptr;
    return &items_[row];
}

bool RemoteModel::isDir(const QModelIndex &index) const {
    const Item *item = itemForIndex(index);
    return item ? item->isDir : false;
}

QString RemoteModel::nameAt(const QModelIndex &index) const {
    const Item *item = itemForIndex(index);
    return item ? item->name : QString();
}

bool RemoteModel::hasSize(const QModelIndex &index) const {
    const Item *item = itemForIndex(index);
    return item ? item->hasSize : false;
}

quint64 RemoteModel::sizeAt(const QModelIndex &index) const {
    const Item *item = itemForIndex(index);
    return item ? item->size : 0;
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

void RemoteModel::sort(int column, Qt::SortOrder order) {
    sortColumn_ = column;
    sortOrder_ = order;
    if (items_.empty())
        return;
    emit layoutAboutToBeChanged();
    sortItemsVector(items_, sortColumn_, sortOrder_);
    emit layoutChanged();
}
