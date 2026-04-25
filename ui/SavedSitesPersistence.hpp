// Shared persistence helpers for saved sites (QSettings array: "sites").
#pragma once

#include "SiteManagerDialog.hpp"

#include <QVector>

#include <functional>

namespace SavedSitesPersistence {

struct LoadOptions {
    // Trim site names on read (used by quick-connect identity matching).
    bool trimSiteNames = false;
    // Optional ID generator used when IDs are missing/duplicated.
    std::function<QString()> createNewId;
};

struct LoadResult {
    QVector<SiteEntry> sites;
    bool needsSave = false;
};

LoadResult loadSites(const LoadOptions &options = {});
void saveSites(const QVector<SiteEntry> &sites, bool syncToDisk);

} // namespace SavedSitesPersistence
