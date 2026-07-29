// Centralized saved-site credential storage and legacy migration.
#pragma once

#include "SavedSitesPersistence.hpp"
#include "SecretStore.hpp"

#include <QStringList>
#include <QVector>

#include <functional>
#include <optional>

enum class SiteCredentialKind {
    Password,
    KeyPassphrase,
    ProxyPassword,
};

struct SiteCredentialIssue {
    SiteCredentialKind kind = SiteCredentialKind::Password;
    SecretStore::PersistResult result;
};

struct SiteCredentialOperationResult {
    bool anyCredentialHandled = false;
    QVector<SiteCredentialIssue> issues;

    [[nodiscard]] QStringList issueMessages() const;
};

struct SiteCredentialMigrationResult {
    bool complete = true;
    QStringList issues;
};

class SiteCredentialRepository {
    public:
    struct Backend {
        std::function<SecretStore::PersistResult(const QString &,
                                                 const QString &)>
            store;
        std::function<std::optional<QString>(const QString &)> load;
        std::function<void(const QString &)> remove;
    };

    explicit SiteCredentialRepository(Backend backend = systemBackend());

    [[nodiscard]] SiteCredentialOperationResult
    save(const SiteEntry &site, const openscp::SessionOptions &options,
         bool removeMissing = true);
    [[nodiscard]] SiteCredentialOperationResult
    load(const SiteEntry &site, openscp::SessionOptions &options,
         bool migrateLegacyNameKeys = true, int legacySettingsIndex = -1);
    [[nodiscard]] SiteCredentialOperationResult copy(const SiteEntry &source,
                                                     const SiteEntry &target);

    void removeAll(const SiteEntry &site, bool includeLegacyNameKeys = true);
    void removeLegacyNameKeys(const QString &siteName);

    [[nodiscard]] static QString stableKey(const SiteEntry &site,
                                           SiteCredentialKind kind);
    [[nodiscard]] static QString legacyNameKey(const QString &siteName,
                                               SiteCredentialKind kind);
    [[nodiscard]] static QString itemName(SiteCredentialKind kind);
    [[nodiscard]] static QString itemLabel(SiteCredentialKind kind);
    [[nodiscard]] static QString statusLabel(SecretStore::PersistStatus status);
    static void clearCredentialFields(openscp::SessionOptions &options);
    [[nodiscard]] static SiteCredentialMigrationResult
    migrateLegacyPlaintext(const SavedSitesPersistence::LoadResult &loaded);
    [[nodiscard]] static Backend systemBackend();

    private:
    [[nodiscard]] std::optional<QString>
    readWithLegacyFallback(const SiteEntry &site, SiteCredentialKind kind,
                           bool migrateLegacyNameKeys,
                           SiteCredentialOperationResult &result);
    [[nodiscard]] SecretStore::PersistResult storeValue(const SiteEntry &site,
                                                        SiteCredentialKind kind,
                                                        const QString &value);
    void loadLegacyPlaintext(const SiteEntry &site, int settingsIndex,
                             openscp::SessionOptions &options,
                             SiteCredentialOperationResult &result);

    Backend backend_;
};
