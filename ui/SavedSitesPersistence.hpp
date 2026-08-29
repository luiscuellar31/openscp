// Shared persistence helpers for saved sites (QSettings array: "sites").
#pragma once

#include "SiteEntry.hpp"

#include <QVector>

#include <functional>

namespace SavedSitesPersistence {

// Plaintext credentials were used by early OpenSCP versions inside each
// QSettings site entry. They are exposed only transiently while loading so the
// credential repository can migrate them before rewriting the array.
struct LegacySecret {
    int siteIndex = -1;
    QString item;
    QString value;
};

struct LoadOptions {
    // Trim site names on read (used by quick-connect identity matching).
    bool trimSiteNames = false;
    // Optional ID generator used when IDs are missing/duplicated.
    std::function<QString()> createNewId;
};

struct LoadResult {
    QVector<SiteEntry> sites;
    QVector<LegacySecret> legacySecrets;
    bool needsSave = false;
};

struct SaveResult {
    bool ok = false;
    QString error;

    [[nodiscard]] explicit operator bool() const noexcept { return ok; }
};

LoadResult loadSites(const LoadOptions &options = {});
SaveResult saveSites(const QVector<SiteEntry> &sites, bool syncToDisk);

} // namespace SavedSitesPersistence
