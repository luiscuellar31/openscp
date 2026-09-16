// Shared persistence helpers for saved sites (QSettings array: "sites").
#pragma once

#include "logic/persistence/SiteEntry.hpp"

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

struct SaveResult {
    bool ok = false;
    QString error;

    [[nodiscard]] explicit operator bool() const noexcept { return ok; }
};

[[nodiscard]] QString createSiteId();
LoadResult loadSites(const LoadOptions &options = {});
SaveResult saveSites(const QVector<SiteEntry> &sites);

} // namespace SavedSitesPersistence
