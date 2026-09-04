#include "logic/persistence/SiteCredentialRepository.hpp"

#include "logic/common/AppSettings.hpp"

#include <QByteArray>
#include <QCoreApplication>

#include <array>
#include <memory>
#include <string_view>

namespace {

constexpr std::array<SiteCredentialKind, 3> kCredentialKinds{
    SiteCredentialKind::Password,
    SiteCredentialKind::KeyPassphrase,
    SiteCredentialKind::ProxyPassword,
};

const openscp::SecureString *
credentialValue(const openscp::SessionOptions &options,
                SiteCredentialKind kind) {
    switch (kind) {
    case SiteCredentialKind::Password:
        return options.password ? &*options.password : nullptr;
    case SiteCredentialKind::KeyPassphrase:
        return options.private_key_passphrase ? &*options.private_key_passphrase
                                              : nullptr;
    case SiteCredentialKind::ProxyPassword:
        if (options.proxy_type != openscp::ProxyType::None)
            return options.proxy_password ? &*options.proxy_password : nullptr;
        return nullptr;
    }
    return nullptr;
}

void assignCredential(openscp::SessionOptions &options, SiteCredentialKind kind,
                      const QString &value) {
    QByteArray utf8Value = value.toUtf8();
    const std::string_view valueView(
        utf8Value.constData(), static_cast<std::size_t>(utf8Value.size()));
    switch (kind) {
    case SiteCredentialKind::Password:
        options.password.emplace(valueView);
        break;
    case SiteCredentialKind::KeyPassphrase:
        options.private_key_passphrase.emplace(valueView);
        break;
    case SiteCredentialKind::ProxyPassword:
        if (options.proxy_type != openscp::ProxyType::None)
            options.proxy_password.emplace(valueView);
        break;
    }
    utf8Value.fill('\0');
}

std::optional<SiteCredentialKind> kindFromItem(const QString &item) {
    if (item == QLatin1String("password"))
        return SiteCredentialKind::Password;
    if (item == QLatin1String("keypass"))
        return SiteCredentialKind::KeyPassphrase;
    if (item == QLatin1String("proxypass"))
        return SiteCredentialKind::ProxyPassword;
    return std::nullopt;
}

QString formatIssue(SiteCredentialKind kind,
                    const SecretStore::PersistResult &result) {
    QString message = QStringLiteral("%1: %2").arg(
        SiteCredentialRepository::itemLabel(kind),
        SiteCredentialRepository::statusLabel(result.status));
    if (!result.detail.trimmed().isEmpty())
        message += QStringLiteral(" (%1)").arg(result.detail.trimmed());
    return message;
}

SecretStore::PersistResult
issueFromLoadResult(const SecretStore::LoadResult &result) {
    using LoadStatus = SecretStore::LoadStatus;
    using PersistStatus = SecretStore::PersistStatus;
    switch (result.status) {
    case LoadStatus::Loaded:
    case LoadStatus::Missing:
        return {PersistStatus::Stored, result.detail};
    case LoadStatus::Unavailable:
        return {PersistStatus::Unavailable, result.detail};
    case LoadStatus::PermissionDenied:
        return {PersistStatus::PermissionDenied, result.detail};
    case LoadStatus::Corrupt:
        return {PersistStatus::Corrupt, result.detail};
    case LoadStatus::BackendError:
        return {PersistStatus::BackendError, result.detail};
    }
    return {PersistStatus::BackendError, result.detail};
}

SecretStore::PersistResult
issueFromDeleteResult(const SecretStore::DeleteResult &result) {
    using DeleteStatus = SecretStore::DeleteStatus;
    using PersistStatus = SecretStore::PersistStatus;
    switch (result.status) {
    case DeleteStatus::Removed:
    case DeleteStatus::Missing:
        return {PersistStatus::Stored, result.detail};
    case DeleteStatus::Unavailable:
        return {PersistStatus::Unavailable, result.detail};
    case DeleteStatus::PermissionDenied:
        return {PersistStatus::PermissionDenied, result.detail};
    case DeleteStatus::BackendError:
        return {PersistStatus::BackendError, result.detail};
    }
    return {PersistStatus::BackendError, result.detail};
}

} // namespace

QStringList SiteCredentialOperationResult::issueMessages() const {
    QStringList messages;
    messages.reserve(issues.size());
    for (const SiteCredentialIssue &issue : issues)
        messages.push_back(formatIssue(issue.kind, issue.result));
    return messages;
}

SiteCredentialRepository::SiteCredentialRepository(Backend backend)
    : backend_(std::move(backend)) {
}

SiteCredentialRepository::Backend SiteCredentialRepository::systemBackend() {
    auto store = std::make_shared<SecretStore>();
    return {
        [store](const QString &key, const QString &value) {
            return store->setSecret(key, value);
        },
        [store](const QString &key) { return store->getSecret(key); },
        [store](const QString &key) { return store->removeSecret(key); },
    };
}

QString SiteCredentialRepository::itemName(SiteCredentialKind kind) {
    switch (kind) {
    case SiteCredentialKind::Password:
        return QStringLiteral("password");
    case SiteCredentialKind::KeyPassphrase:
        return QStringLiteral("keypass");
    case SiteCredentialKind::ProxyPassword:
        return QStringLiteral("proxypass");
    }
    return {};
}

QString SiteCredentialRepository::itemLabel(SiteCredentialKind kind) {
    switch (kind) {
    case SiteCredentialKind::Password:
        return QCoreApplication::translate("SiteCredentialRepository",
                                           "Password");
    case SiteCredentialKind::KeyPassphrase:
        return QCoreApplication::translate("SiteCredentialRepository",
                                           "Key passphrase");
    case SiteCredentialKind::ProxyPassword:
        return QCoreApplication::translate("SiteCredentialRepository",
                                           "Proxy password");
    }
    return QCoreApplication::translate("SiteCredentialRepository",
                                       "Credential");
}

QString
SiteCredentialRepository::statusLabel(SecretStore::PersistStatus status) {
    switch (status) {
    case SecretStore::PersistStatus::Stored:
        return QCoreApplication::translate("SiteCredentialRepository",
                                           "stored");
    case SecretStore::PersistStatus::Unavailable:
        return QCoreApplication::translate("SiteCredentialRepository",
                                           "unavailable");
    case SecretStore::PersistStatus::PermissionDenied:
        return QCoreApplication::translate("SiteCredentialRepository",
                                           "permission denied");
    case SecretStore::PersistStatus::BackendError:
        return QCoreApplication::translate("SiteCredentialRepository",
                                           "backend error");
    case SecretStore::PersistStatus::Corrupt:
        return QCoreApplication::translate("SiteCredentialRepository",
                                           "corrupt credential");
    }
    return QCoreApplication::translate("SiteCredentialRepository", "unknown");
}

QString SiteCredentialRepository::stableKey(const SiteEntry &site,
                                            SiteCredentialKind kind) {
    const QString item = itemName(kind);
    if (!site.siteId.trimmed().isEmpty()) {
        return QStringLiteral("site-id:%1:%2").arg(site.siteId.trimmed(), item);
    }
    return legacyNameKey(site.name, kind);
}

QString SiteCredentialRepository::legacyNameKey(const QString &siteName,
                                                SiteCredentialKind kind) {
    return QStringLiteral("site:%1:%2").arg(siteName, itemName(kind));
}

SecretStore::PersistResult SiteCredentialRepository::storeValue(
    const SiteEntry &site, SiteCredentialKind kind, const QString &value) {
    return backend_.store(stableKey(site, kind), value);
}

SiteCredentialOperationResult
SiteCredentialRepository::save(const SiteEntry &site,
                               const openscp::SessionOptions &options,
                               bool removeMissing) {
    SiteCredentialOperationResult result;
    for (SiteCredentialKind kind : kCredentialKinds) {
        const openscp::SecureString *value = credentialValue(options, kind);
        if (!value || value->empty()) {
            if (removeMissing) {
                const SecretStore::DeleteResult deleteResult =
                    backend_.remove(stableKey(site, kind));
                if (!deleteResult.isRemovedOrMissing()) {
                    result.issues.push_back(
                        {kind, issueFromDeleteResult(deleteResult)});
                }
            }
            continue;
        }

        const SecretStore::PersistResult persistResult = storeValue(
            site, kind,
            QString::fromUtf8(value->data(),
                              static_cast<qsizetype>(value->size())));
        if (persistResult.isStored())
            result.anyCredentialHandled = true;
        else
            result.issues.push_back({kind, persistResult});
    }
    return result;
}

std::optional<QString> SiteCredentialRepository::readWithLegacyFallback(
    const SiteEntry &site, SiteCredentialKind kind, bool migrateLegacyNameKeys,
    SiteCredentialOperationResult &result) {
    const QString stable = stableKey(site, kind);
    const SecretStore::LoadResult stableResult = backend_.load(stable);
    if (stableResult.isLoaded())
        return stableResult.value;
    if (stableResult.status != SecretStore::LoadStatus::Missing) {
        result.issues.push_back({kind, issueFromLoadResult(stableResult)});
        return std::nullopt;
    }

    const QString legacy = legacyNameKey(site.name, kind);
    if (legacy == stable)
        return std::nullopt;
    const SecretStore::LoadResult legacyResult = backend_.load(legacy);
    if (!legacyResult.isLoaded()) {
        if (legacyResult.status != SecretStore::LoadStatus::Missing) {
            result.issues.push_back({kind, issueFromLoadResult(legacyResult)});
        }
        return std::nullopt;
    }

    if (migrateLegacyNameKeys) {
        const SecretStore::PersistResult persistResult =
            backend_.store(stable, legacyResult.value);
        if (persistResult.isStored()) {
            const SecretStore::DeleteResult deleteResult =
                backend_.remove(legacy);
            if (!deleteResult.isRemovedOrMissing()) {
                result.issues.push_back(
                    {kind, issueFromDeleteResult(deleteResult)});
            }
        } else {
            result.issues.push_back({kind, persistResult});
        }
    }
    return legacyResult.value;
}

SiteCredentialOperationResult SiteCredentialRepository::load(
    const SiteEntry &site, openscp::SessionOptions &options,
    bool migrateLegacyNameKeys, int legacySettingsIndex) {
    SiteCredentialOperationResult result;
    for (SiteCredentialKind kind : kCredentialKinds) {
        if (kind == SiteCredentialKind::ProxyPassword &&
            options.proxy_type == openscp::ProxyType::None) {
            options.proxy_password.reset();
            continue;
        }
        const std::optional<QString> value =
            readWithLegacyFallback(site, kind, migrateLegacyNameKeys, result);
        if (!value)
            continue;
        assignCredential(options, kind, *value);
        result.anyCredentialHandled = true;
    }
    if (legacySettingsIndex >= 0)
        loadLegacyPlaintext(site, legacySettingsIndex, options, result);
    return result;
}

void SiteCredentialRepository::loadLegacyPlaintext(
    const SiteEntry &site, int settingsIndex, openscp::SessionOptions &options,
    SiteCredentialOperationResult &result) {
    openscpui::AppSettings settings;
    const int siteCount = settings.beginReadArray(QStringLiteral("sites"));
    if (settingsIndex < 0 || settingsIndex >= siteCount) {
        settings.endArray();
        return;
    }
    settings.setArrayIndex(settingsIndex);

    struct LegacyValue {
        SiteCredentialKind kind;
        QString settingsKey;
        QString value;
    };
    const std::array<LegacyValue, 3> legacyValues{{
        {SiteCredentialKind::Password, QStringLiteral("password"),
         settings.value(QStringLiteral("password")).toString()},
        {SiteCredentialKind::KeyPassphrase, QStringLiteral("keyPass"),
         settings.value(QStringLiteral("keyPass")).toString()},
        {SiteCredentialKind::ProxyPassword, QStringLiteral("proxyPass"),
         settings.value(QStringLiteral("proxyPass")).toString()},
    }};
    settings.endArray();

    QStringList keysToRemove;
    for (const LegacyValue &legacy : legacyValues) {
        if (legacy.value.isEmpty())
            continue;
        if (legacy.kind == SiteCredentialKind::ProxyPassword &&
            options.proxy_type == openscp::ProxyType::None) {
            keysToRemove.push_back(legacy.settingsKey);
            continue;
        }

        const openscp::SecureString *existing =
            credentialValue(options, legacy.kind);
        if (!existing || existing->empty()) {
            assignCredential(options, legacy.kind, legacy.value);
            result.anyCredentialHandled = true;
        }

        const SecretStore::PersistResult persistResult =
            storeValue(site, legacy.kind, legacy.value);
        if (persistResult.isStored())
            keysToRemove.push_back(legacy.settingsKey);
        else
            result.issues.push_back({legacy.kind, persistResult});
    }

    if (keysToRemove.isEmpty())
        return;
    settings.beginWriteArray(QStringLiteral("sites"));
    settings.setArrayIndex(settingsIndex);
    for (const QString &key : keysToRemove)
        settings.remove(key);
    settings.endArray();
    const openscpui::SettingsSyncResult syncResult = settings.syncSecure();
    if (!syncResult.ok) {
        for (const LegacyValue &legacy : legacyValues) {
            if (keysToRemove.contains(legacy.settingsKey)) {
                result.issues.push_back(
                    {legacy.kind,
                     {SecretStore::PersistStatus::BackendError,
                      syncResult.error}});
            }
        }
    }
}

SiteCredentialOperationResult
SiteCredentialRepository::copy(const SiteEntry &source,
                               const SiteEntry &target) {
    openscp::SessionOptions options = source.opt;
    SiteCredentialOperationResult result = load(source, options, true);
    const SiteCredentialOperationResult saveResult =
        save(target, options, true);
    result.anyCredentialHandled =
        result.anyCredentialHandled || saveResult.anyCredentialHandled;
    result.issues += saveResult.issues;
    return result;
}

SiteCredentialOperationResult
SiteCredentialRepository::removeLegacyNameKeys(const QString &siteName) {
    SiteCredentialOperationResult result;
    if (siteName.isEmpty())
        return result;
    for (SiteCredentialKind kind : kCredentialKinds) {
        const SecretStore::DeleteResult deleteResult =
            backend_.remove(legacyNameKey(siteName, kind));
        if (deleteResult.status == SecretStore::DeleteStatus::Removed)
            result.anyCredentialHandled = true;
        else if (!deleteResult.isRemovedOrMissing())
            result.issues.push_back(
                {kind, issueFromDeleteResult(deleteResult)});
    }
    return result;
}

SiteCredentialOperationResult
SiteCredentialRepository::removeAll(const SiteEntry &site,
                                    bool includeLegacyNameKeys) {
    SiteCredentialOperationResult result;
    for (SiteCredentialKind kind : kCredentialKinds) {
        const SecretStore::DeleteResult deleteResult =
            backend_.remove(stableKey(site, kind));
        if (deleteResult.status == SecretStore::DeleteStatus::Removed)
            result.anyCredentialHandled = true;
        else if (!deleteResult.isRemovedOrMissing())
            result.issues.push_back(
                {kind, issueFromDeleteResult(deleteResult)});
    }
    if (includeLegacyNameKeys) {
        SiteCredentialOperationResult legacyResult =
            removeLegacyNameKeys(site.name);
        result.anyCredentialHandled =
            result.anyCredentialHandled || legacyResult.anyCredentialHandled;
        result.issues += legacyResult.issues;
    }
    return result;
}

void SiteCredentialRepository::clearCredentialFields(
    openscp::SessionOptions &options) {
    options.password.reset();
    options.private_key_passphrase.reset();
    options.proxy_password.reset();
}

SiteCredentialMigrationResult SiteCredentialRepository::migrateLegacyPlaintext(
    const SavedSitesPersistence::LoadResult &loaded) {
    SiteCredentialMigrationResult migration;
    SiteCredentialRepository repository;
    for (const SavedSitesPersistence::LegacySecret &legacy :
         loaded.legacySecrets) {
        if (legacy.siteIndex < 0 || legacy.siteIndex >= loaded.sites.size()) {
            migration.complete = false;
            migration.issues.push_back(QCoreApplication::translate(
                "SiteCredentialRepository",
                "A legacy credential could not be matched to its site."));
            continue;
        }

        const std::optional<SiteCredentialKind> kind =
            kindFromItem(legacy.item);
        if (!kind) {
            migration.complete = false;
            migration.issues.push_back(QCoreApplication::translate(
                "SiteCredentialRepository",
                "A legacy credential has an unknown type."));
            continue;
        }

        const SiteEntry &site = loaded.sites.at(legacy.siteIndex);
        if (*kind == SiteCredentialKind::ProxyPassword &&
            site.opt.proxy_type == openscp::ProxyType::None) {
            continue;
        }
        const SecretStore::LoadResult loadResult =
            repository.backend_.load(stableKey(site, *kind));
        if (loadResult.isLoaded())
            continue;
        if (loadResult.status != SecretStore::LoadStatus::Missing) {
            migration.complete = false;
            migration.issues.push_back(QStringLiteral("%1 — %2").arg(
                site.name,
                formatIssue(*kind, issueFromLoadResult(loadResult))));
            continue;
        }

        const SecretStore::PersistResult result =
            repository.storeValue(site, *kind, legacy.value);
        if (result.isStored())
            continue;

        migration.complete = false;
        migration.issues.push_back(QStringLiteral("%1 — %2").arg(
            site.name, formatIssue(*kind, result)));
    }
    return migration;
}
