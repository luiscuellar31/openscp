// Helpers shared by saved-site entry points for migrating legacy credentials.
#pragma once

#include "SavedSitesPersistence.hpp"
#include "SecretStore.hpp"

#include <QCoreApplication>
#include <QStringList>

namespace SavedSiteSecrets {

struct MigrationResult {
    bool complete = true;
    QStringList issues;
};

inline QString stableKey(const SiteEntry &site, const QString &item) {
    if (!site.siteId.trimmed().isEmpty())
        return QStringLiteral("site-id:%1:%2").arg(site.siteId.trimmed(), item);
    return QStringLiteral("site:%1:%2").arg(site.name, item);
}

inline QString itemLabel(const QString &item) {
    if (item == QStringLiteral("password")) {
        return QCoreApplication::translate("SavedSiteSecrets", "Password");
    }
    if (item == QStringLiteral("keypass")) {
        return QCoreApplication::translate("SavedSiteSecrets",
                                           "Key passphrase");
    }
    if (item == QStringLiteral("proxypass")) {
        return QCoreApplication::translate("SavedSiteSecrets",
                                           "Proxy password");
    }
    return QCoreApplication::translate("SavedSiteSecrets", "Credential");
}

inline QString statusLabel(SecretStore::PersistStatus status) {
    switch (status) {
    case SecretStore::PersistStatus::Stored:
        return QCoreApplication::translate("SavedSiteSecrets", "stored");
    case SecretStore::PersistStatus::Unavailable:
        return QCoreApplication::translate("SavedSiteSecrets", "unavailable");
    case SecretStore::PersistStatus::PermissionDenied:
        return QCoreApplication::translate("SavedSiteSecrets",
                                           "permission denied");
    case SecretStore::PersistStatus::BackendError:
        return QCoreApplication::translate("SavedSiteSecrets",
                                           "backend error");
    }
    return QCoreApplication::translate("SavedSiteSecrets", "unknown");
}

// Returns incomplete when rewriting QSettings would lose a credential. The
// caller must leave the legacy array untouched in that case.
inline MigrationResult
migrateLegacyPlaintext(const SavedSitesPersistence::LoadResult &loaded) {
    MigrationResult migration;
    if (loaded.legacySecrets.isEmpty())
        return migration;

    SecretStore store;
    for (const SavedSitesPersistence::LegacySecret &legacy :
         loaded.legacySecrets) {
        if (legacy.siteIndex < 0 || legacy.siteIndex >= loaded.sites.size()) {
            migration.complete = false;
            migration.issues.push_back(QCoreApplication::translate(
                "SavedSiteSecrets",
                "A legacy credential could not be matched to its site."));
            continue;
        }

        const SiteEntry &site = loaded.sites.at(legacy.siteIndex);
        // Old proxy passwords attached to a site without a proxy are stale and
        // were intentionally discarded by the previous migration path.
        if (legacy.item == QStringLiteral("proxypass") &&
            site.opt.proxy_type == openscp::ProxyType::None) {
            continue;
        }

        const QString key = stableKey(site, legacy.item);
        if (key.trimmed().isEmpty()) {
            migration.complete = false;
            migration.issues.push_back(QCoreApplication::translate(
                "SavedSiteSecrets",
                "%1: no stable site identity is available.")
                                           .arg(site.name));
            continue;
        }
        if (store.getSecret(key))
            continue;

        const SecretStore::PersistResult result =
            store.setSecret(key, legacy.value);
        if (result.isStored())
            continue;

        migration.complete = false;
        QString issue =
            QCoreApplication::translate("SavedSiteSecrets", "%1 — %2: %3")
                .arg(site.name, itemLabel(legacy.item),
                     statusLabel(result.status));
        if (!result.detail.trimmed().isEmpty())
            issue += QStringLiteral(" (%1)").arg(result.detail.trimmed());
        migration.issues.push_back(issue);
    }
    return migration;
}

} // namespace SavedSiteSecrets
