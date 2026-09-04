#include "logic/persistence/SiteCredentialRepository.hpp"

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
    return QStringLiteral("site-id:%1:%2")
        .arg(site.siteId.trimmed(), itemName(kind));
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

std::optional<QString>
SiteCredentialRepository::readValue(const SiteEntry &site,
                                    SiteCredentialKind kind,
                                    SiteCredentialOperationResult &result) {
    const QString stable = stableKey(site, kind);
    const SecretStore::LoadResult stableResult = backend_.load(stable);
    if (stableResult.isLoaded())
        return stableResult.value;
    if (stableResult.status != SecretStore::LoadStatus::Missing) {
        result.issues.push_back({kind, issueFromLoadResult(stableResult)});
    }
    return std::nullopt;
}

SiteCredentialOperationResult
SiteCredentialRepository::load(const SiteEntry &site,
                               openscp::SessionOptions &options) {
    SiteCredentialOperationResult result;
    for (SiteCredentialKind kind : kCredentialKinds) {
        if (kind == SiteCredentialKind::ProxyPassword &&
            options.proxy_type == openscp::ProxyType::None) {
            options.proxy_password.reset();
            continue;
        }
        const std::optional<QString> value = readValue(site, kind, result);
        if (!value)
            continue;
        assignCredential(options, kind, *value);
        result.anyCredentialHandled = true;
    }
    return result;
}

SiteCredentialOperationResult
SiteCredentialRepository::copy(const SiteEntry &source,
                               const SiteEntry &target) {
    openscp::SessionOptions options = source.opt;
    SiteCredentialOperationResult result = load(source, options);
    const SiteCredentialOperationResult saveResult =
        save(target, options, true);
    result.anyCredentialHandled =
        result.anyCredentialHandled || saveResult.anyCredentialHandled;
    result.issues += saveResult.issues;
    return result;
}

SiteCredentialOperationResult
SiteCredentialRepository::removeAll(const SiteEntry &site) {
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
    return result;
}

void SiteCredentialRepository::clearCredentialFields(
    openscp::SessionOptions &options) {
    options.password.reset();
    options.private_key_passphrase.reset();
    options.proxy_password.reset();
}
