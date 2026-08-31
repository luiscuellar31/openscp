// Remote model implementation (table: Name, Size, Date, Permissions).
#include "RemoteModel.hpp"

#include "MainWindowSharedUtils.hpp"
#include "TimeUtils.hpp"

#include <QApplication>
#include <QDateTime>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QMimeData>
#include <QMimeDatabase>
#include <QMimeType>
#include <QStyle>
#include <QVariant>

#include <algorithm>
#include <array>

namespace {

QIcon remoteFolderIcon() {
    QIcon themed = QIcon::fromTheme(QStringLiteral("folder"));
    if (!themed.isNull())
        return themed;
    return QApplication::style()
               ? QApplication::style()->standardIcon(QStyle::SP_DirIcon)
               : QIcon();
}

QIcon remoteFileIcon() {
    QIcon themed = QIcon::fromTheme(QStringLiteral("text-x-generic"));
    if (!themed.isNull())
        return themed;
    return QApplication::style()
               ? QApplication::style()->standardIcon(QStyle::SP_FileIcon)
               : QIcon();
}

QIcon remoteLinkIcon() {
    QIcon themed = QIcon::fromTheme(QStringLiteral("emblem-symbolic-link"));
    if (!themed.isNull())
        return themed;
    return QApplication::style()
               ? QApplication::style()->standardIcon(QStyle::SP_FileLinkIcon)
               : QIcon();
}

QIcon iconFromMimeTheme(const QString &name) {
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

QString permissionText(bool isDir, bool isLink, quint32 mode) {
    QString permissions(10, '-');
    permissions[0] = isLink ? 'l' : (isDir ? 'd' : '-');
    constexpr std::array<std::pair<quint32, char>, 9> bits{{
        {0400, 'r'},
        {0200, 'w'},
        {0100, 'x'},
        {0040, 'r'},
        {0020, 'w'},
        {0010, 'x'},
        {0004, 'r'},
        {0002, 'w'},
        {0001, 'x'},
    }};
    qsizetype position = 1;
    for (const auto &[mask, flag] : bits) {
        if ((mode & mask) != 0)
            permissions[position] = QLatin1Char(flag);
        ++position;
    }
    return permissions;
}

} // namespace

RemoteModel::RemoteModel(QObject *parent) : QAbstractTableModel(parent) {
    iconCache_.setMaxCost(128);
}

int RemoteModel::rowCount(const QModelIndex &parent) const {
    if (parent.isValid())
        return 0;
    if (loading_)
        return 1;
    return static_cast<int>(items_.size());
}

QIcon RemoteModel::iconForRemoteEntry(const Item &item) const {
    const QString currentTheme = QIcon::themeName();
    if (cachedIconTheme_ != currentTheme) {
        iconCache_.clear();
        cachedIconTheme_ = currentTheme;
    }

    QString key;
    if (item.isLink) {
        key = QStringLiteral("__link");
    } else if (item.isDir) {
        key = QStringLiteral("__dir");
    } else {
        const QString ext = QFileInfo(item.name).completeSuffix().toLower();
        key = ext.isEmpty() ? QStringLiteral("__file")
                            : QStringLiteral("ext:") + ext;
    }

    if (const QIcon *cached = iconCache_.object(key))
        return *cached;

    QIcon icon;
#ifdef Q_OS_MAC
    static QFileIconProvider provider;
    if (item.isDir) {
        icon = provider.icon(QFileIconProvider::Folder);
    } else if (!item.isLink) {
        const QString ext = QFileInfo(item.name).completeSuffix().toLower();
        QString probeName = QStringLiteral("remote-entry");
        if (!ext.isEmpty())
            probeName += QStringLiteral(".") + ext;
        icon = provider.icon(QFileInfo(probeName));
    }
#endif
    if (icon.isNull() && item.isLink) {
        icon = remoteLinkIcon();
    }
    if (icon.isNull() && item.isDir) {
        icon = remoteFolderIcon();
    }
    if (icon.isNull() && !item.isDir && !item.isLink) {
        icon = iconFromMimeTheme(item.name);
    }
    if (icon.isNull() && !item.isDir && !item.isLink) {
        icon = remoteFileIcon();
    }
    if (icon.isNull()) {
        icon = remoteFileIcon();
    }

    iconCache_.insert(key, new QIcon(icon));
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
    if (role == Qt::DecorationRole && index.column() == 0) {
        return iconForRemoteEntry(item);
    }
    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case 0: {
            QString suffix;
            if (item.isLink)
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
            return locale_.formattedDataSize(static_cast<qint64>(item.size), 1,
                                             QLocale::DataSizeIecFormat);
        case 2:
            if (item.mtime > 0)
                return openscpui::localShortTime(item.mtime);
            else
                return QVariant();
        case 3:
            return item.permissions;
        default:
            break;
        }
    }
    if (role == Qt::ToolTipRole) {
        if (item.isDir)
            return tr("Folder");
        if (!item.hasSize) {
            return tr("Size: unknown (not provided by the server)");
        }
        QString tip = tr("File");
        const QString human = locale_.formattedDataSize(
            static_cast<qint64>(item.size), 1, QLocale::DataSizeIecFormat);
        const QString bytes =
            locale_.toString(static_cast<qulonglong>(item.size));
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
        const bool isLink = (fileInfo.mode & 0120000u) == 0120000u;
        nextItems.push_back(
            {name, fileInfo.is_dir, isLink, fileInfo.size, fileInfo.has_size,
             fileInfo.mtime, fileInfo.mode, fileInfo.uid, fileInfo.gid,
             permissionText(fileInfo.is_dir, isLink, fileInfo.mode)});
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
    return &items_[static_cast<std::size_t>(row)];
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
    default:
        break;
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
