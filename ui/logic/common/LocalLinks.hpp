// How local folder operations tell a link from the entry it leads to.
#pragma once

#include <QFileInfo>

// Whether local operations must take `info` as a link: copying recreates it,
// deleting removes the link itself, and scans never descend into it. Windows
// junctions count, though isSymLink() misses them, and Windows shortcuts
// (.lnk files), which isSymLink() reports, are ordinary files there.
inline bool isLocalLink(const QFileInfo &info) {
#ifdef Q_OS_WIN
    return info.isSymbolicLink() || info.isJunction();
#else
    return info.isSymLink();
#endif
}
