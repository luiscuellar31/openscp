// Implementación del modelo remoto (tabla: Nombre, Tamaño, Fecha, Permisos).
#include "RemoteModel.hpp"
#include <QVariant>
#include <QDateTime>
#include <QLocale>
#include <QMimeData>

RemoteModel::RemoteModel(openscp::SftpClient* client, QObject* parent)
    : QAbstractTableModel(parent), client_(client) {}

int RemoteModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return static_cast<int>(items_.size());
}

QVariant RemoteModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= (int)items_.size())
        return {};
    const auto& it = items_[index.row()];
    if (role == Qt::DisplayRole) {
        switch (index.column()) {
            case 0: {
                bool isLnk = (it.mode & 0120000u) == 0120000u; // S_IFLNK
                QString suffix;
                if (isLnk) suffix = "@"; else if (it.isDir) suffix = "/";
                return it.name + suffix;
            }
            case 1:
                if (it.isDir) return QVariant();
                return QLocale().formattedDataSize((qint64)it.size, 1, QLocale::DataSizeIecFormat);
            case 2:
                if (it.mtime > 0)
                    return QLocale().toString(QDateTime::fromSecsSinceEpoch((qint64)it.mtime), QLocale::ShortFormat);
                else
                    return QVariant();
            case 3: {
                // permisos estilo rwxr-xr-x
                QString s(10, '-');
                const quint32 m = it.mode;
                // tipo
                bool isLnk = (m & 0120000u) == 0120000u;
                s[0] = isLnk ? 'l' : (it.isDir ? 'd' : '-');
                auto bit = [&](int pos, quint32 mask, QChar ch) { if (m & mask) s[pos] = ch; };
                bit(1, 0400, 'r'); bit(2, 0200, 'w'); bit(3, 0100, 'x');
                bit(4, 0040, 'r'); bit(5, 0020, 'w'); bit(6, 0010, 'x');
                bit(7, 0004, 'r'); bit(8, 0002, 'w'); bit(9, 0001, 'x');
                return s;
            }
        }
    }
    if (role == Qt::ToolTipRole) {
        const auto& it = items_[index.row()];
        if (it.isDir) return "Carpeta";
        QString tip = "Archivo";
        if (it.size > 0) {
            const QString human = QLocale().formattedDataSize((qint64)it.size, 1, QLocale::DataSizeIecFormat);
            const QString bytes = QLocale().toString((qulonglong)it.size);
            tip += QString(" • %1 (%2 bytes)").arg(human, bytes);
        }
        if (it.mtime > 0) {
            QDateTime dt = QDateTime::fromSecsSinceEpoch((qint64)it.mtime);
            tip += " • " + QLocale().toString(dt, QLocale::ShortFormat);
        }
        return tip;
    }
    return {};
}

Qt::ItemFlags RemoteModel::flags(const QModelIndex& index) const {
    if (!index.isValid()) return Qt::NoItemFlags;
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled;
}

bool RemoteModel::setRootPath(const QString& path, QString* errorOut) {
    if (!client_) {
        if (errorOut) *errorOut = "Sin cliente SFTP";
        return false;
    }
    std::vector<openscp::FileInfo> out;
    std::string err;
    if (!client_->list(path.toStdString(), out, err)) {
        if (errorOut) *errorOut = QString::fromStdString(err);
        return false;
    }

    beginResetModel();
    items_.clear();
    items_.reserve(out.size());
    for (const auto& f : out) {
        items_.push_back({ QString::fromStdString(f.name), f.is_dir, f.size, f.mtime, f.mode, f.uid, f.gid });
    }
    currentPath_ = path;
    endResetModel();
    return true;
}

bool RemoteModel::isDir(const QModelIndex& idx) const {
    if (!idx.isValid()) return false;
    return items_[idx.row()].isDir;
}

QString RemoteModel::nameAt(const QModelIndex& idx) const {
    if (!idx.isValid()) return {};
    return items_[idx.row()].name;
}

QVariant RemoteModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    switch (section) {
        case 0: return "Nombre";
        case 1: return "Tamaño";
        case 2: return "Fecha";
        case 3: return "Permisos";
    }
    return {};
}

QStringList RemoteModel::mimeTypes() const {
    // No necesitamos URL reales; basta con tener algún tipo para que inicie el drag.
    // Usamos el tipo de datos estándar que Qt entiende.
    return { QStringLiteral("text/plain") };
}

QMimeData* RemoteModel::mimeData(const QModelIndexList& indexes) const {
    if (indexes.isEmpty()) return nullptr;
    QStringList names;
    for (const QModelIndex& idx : indexes) {
        if (!idx.isValid() || idx.column() != 0) continue;
        const auto& it = items_[idx.row()];
        names << it.name;
    }
    if (names.isEmpty()) return nullptr;
    QMimeData* md = new QMimeData();
    md->setText(names.join('\n'));
    return md;
}

void RemoteModel::sort(int column, Qt::SortOrder order) {
    if (items_.empty()) return;
    beginResetModel();
    const bool asc = (order == Qt::AscendingOrder);
    auto lessStr = [&](const QString& a, const QString& b) {
        int cmp = QString::compare(a, b, Qt::CaseInsensitive);
        return asc ? (cmp < 0) : (cmp > 0);
    };
    auto less = [&](const Item& a, const Item& b) {
        // Directorios primero, luego criterio
        if (a.isDir != b.isDir) return a.isDir && !b.isDir;
        switch (column) {
            case 0: return lessStr(a.name, b.name);
            case 1: return asc ? (a.size < b.size) : (a.size > b.size);
            case 2: return asc ? (a.mtime < b.mtime) : (a.mtime > b.mtime);
            case 3: return asc ? (a.mode < b.mode) : (a.mode > b.mode);
        }
        return lessStr(a.name, b.name);
    };
    std::sort(items_.begin(), items_.end(), less);
    endResetModel();
}
