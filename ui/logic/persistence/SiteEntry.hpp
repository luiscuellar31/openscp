// Persistent saved-site value object, independent of Site Manager widgets.
#pragma once

#include "openscp/SessionOptions.hpp"

#include <QString>

struct SiteEntry {
    QString siteId;
    QString name;
    openscp::SessionOptions opt;
    // An empty local path means the platform home/current directory.
    QString initialLocalPath;
    // The remote namespace is always rooted.
    QString initialRemotePath = QStringLiteral("/");
    bool rememberLastPaths = false;
};
