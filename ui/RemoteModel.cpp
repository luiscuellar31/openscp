// Remote model implementation (table: Name, Size, Date, Permissions).
#include "RemoteModel.hpp"
#include "TimeUtils.hpp"
#include <QCoreApplication>
#include <QDateTime>
#include <QLoggingCategory>
#include <QMetaObject>
#include <QPointer>
#include <QSet>
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

QVariant RemoteModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 ||
        index.row() >= (int)items_.size())
        return {};
    const auto &it = items_[index.row()];
    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case 0: {
            bool isLnk = (it.mode & 0120000u) == 0120000u; // S_IFLNK
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
            *errorOut = tr("No SFTP client available");
        return false;
    }

    QString normalized = path.trimmed();
    if (normalized.isEmpty())
        normalized = QStringLiteral("/");
    if (!normalized.startsWith('/'))
        normalized.prepend('/');
    if (normalized.size() > 1 && normalized.endsWith('/'))
        normalized.chop(1);

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

static QString normalizeRemotePath(const QString &p) {
    if (p.isEmpty())
        return QStringLiteral("/");
    QString q = p;
    if (!q.startsWith('/'))
        q.prepend('/');
    // remove trailing slash except root
    if (q.size() > 1 && q.endsWith('/'))
        q.chop(1);
    return q;
}

static QString sanitizeRelative(const QString &rel) {
    // Remove control chars and forbid ".." segments
    QString out;
    out.reserve(rel.size());
    for (QChar ch : rel) {
        if (ch.unicode() < 0x20)
            continue; // skip control chars
#ifdef Q_OS_WIN
        if (ch == ':')
            continue; // avoid colon on Windows
#endif
        QChar c = ch;
        if (c == '\\')
            c = '/';
        out.append(c);
    }
    QStringList parts = out.split('/', Qt::SkipEmptyParts);
    QStringList safe;
    for (const QString &part : parts) {
        if (part == ".")
            continue;
        if (part == "..")
            return QString(); // invalid
        safe.append(part);
    }
    return safe.join('/');
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

    auto joinRemote = [](const QString &base, const QString &name) {
        if (base == "/")
            return QStringLiteral("/") + name;
        return base.endsWith('/') ? base + name : base + "/" + name;
    };
    const QString base = normalizeRemotePath(baseRemote);
    QSet<QString> visited;
    // Resolve max depth from settings if not provided or invalid
    int configuredMaxDepth = opt.maxDepth;
    if (configuredMaxDepth <= 0) {
        QSettings s("OpenSCP", "OpenSCP");
        configuredMaxDepth = s.value("Advanced/maxFolderDepth", 32).toInt();
        if (configuredMaxDepth < 1)
            configuredMaxDepth = 32;
    }

    std::function<void(const QString &, const QString &, int)> walk;
    walk = [&](const QString &cur, const QString &rel, int depth) {
        if (opt.cancel && opt.cancel->load(std::memory_order_relaxed))
            return;
        if (depth > configuredMaxDepth) {
            qWarning(ocEnum) << "max depth reached at" << cur;
            return;
        }
        const QString normCur = normalizeRemotePath(cur);
        if (visited.contains(normCur))
            return; // prevent cycles
        visited.insert(normCur);
        if (dirCountOut)
            (*dirCountOut)++;

        std::vector<openscp::FileInfo> children;
        std::string err;
        if (!client_->list(normCur.toStdString(), children, err)) {
            qWarning(ocEnum) << "enumeration error at" << normCur << ":"
                             << QString::fromStdString(err);
            if (partialErrorOut)
                *partialErrorOut = true;
            if (deniedCountOut)
                (*deniedCountOut)++;
            return;
        }
        for (const auto &e : children) {
            if (opt.cancel && opt.cancel->load(std::memory_order_relaxed))
                return;
            const QString name = QString::fromStdString(e.name);
            if (!showHidden_ && name.startsWith('.'))
                continue;
            bool isSymlink = (e.mode & 0120000u) == 0120000u;
            if (isSymlink && opt.skipSymlinks) {
                if (symlinkSkippedOut)
                    (*symlinkSkippedOut)++;
                continue;
            }
            const QString childRemote = joinRemote(normCur, name);
            const QString childRel0 = rel.isEmpty() ? name : (rel + "/" + name);
            const QString childRel = sanitizeRelative(childRel0);
            if (childRel.isEmpty())
                continue;
            if (e.is_dir) {
                walk(childRemote, childRel, depth + 1);
                if (opt.cancel && opt.cancel->load(std::memory_order_relaxed))
                    return;
            } else {
                quint64 sz = (quint64)e.size;
                const bool hsz = e.has_size;
                if (!hsz) {
                    if (someSizeUnknownOut)
                        *someSizeUnknownOut = true;
                    if (unknownSizeCountOut)
                        (*unknownSizeCountOut)++;
                }
                out.push_back(EnumeratedFile{childRemote, childRel, sz, hsz});
            }
        }
    };
    walk(base, QString(), 0);
    return true;
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
