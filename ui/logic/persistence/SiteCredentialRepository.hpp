// Centralized saved-site credential storage.
#pragma once

#include "logic/persistence/SecretStore.hpp"
#include "logic/persistence/SiteEntry.hpp"

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

class SiteCredentialRepository {
    public:
    struct Backend {
        std::function<SecretStore::PersistResult(const QString &,
                                                 const QString &)>
            store;
        std::function<SecretStore::LoadResult(const QString &)> load;
        std::function<SecretStore::DeleteResult(const QString &)> remove;
    };

    explicit SiteCredentialRepository(Backend backend = systemBackend());

    [[nodiscard]] SiteCredentialOperationResult
    save(const SiteEntry &site, const openscp::SessionOptions &options,
         bool removeMissing = true);
    [[nodiscard]] SiteCredentialOperationResult
    load(const SiteEntry &site, openscp::SessionOptions &options);
    [[nodiscard]] SiteCredentialOperationResult copy(const SiteEntry &source,
                                                     const SiteEntry &target);

    [[nodiscard]] SiteCredentialOperationResult
    removeAll(const SiteEntry &site);

    [[nodiscard]] static QString stableKey(const SiteEntry &site,
                                           SiteCredentialKind kind);
    [[nodiscard]] static QString itemName(SiteCredentialKind kind);
    [[nodiscard]] static QString itemLabel(SiteCredentialKind kind);
    [[nodiscard]] static QString statusLabel(SecretStore::PersistStatus status);
    static void clearCredentialFields(openscp::SessionOptions &options);
    [[nodiscard]] static Backend systemBackend();

    private:
    [[nodiscard]] std::optional<QString>
    readValue(const SiteEntry &site, SiteCredentialKind kind,
              SiteCredentialOperationResult &result);
    [[nodiscard]] SecretStore::PersistResult storeValue(const SiteEntry &site,
                                                        SiteCredentialKind kind,
                                                        const QString &value);
    Backend backend_;
};
