#include "SiteCredentialRepository.hpp"
#include "TestHarness.hpp"

#include <QCoreApplication>
#include <QHash>

#include <iostream>
#include <memory>

namespace {

struct FakeSecretBackend {
    QHash<QString, QString> values;
    SecretStore::PersistStatus saveStatus = SecretStore::PersistStatus::Stored;

    SiteCredentialRepository::Backend interface() {
        return {
            [this](const QString &key, const QString &value) {
                if (saveStatus == SecretStore::PersistStatus::Stored)
                    values.insert(key, value);
                return SecretStore::PersistResult{saveStatus, {}};
            },
            [this](const QString &key) -> std::optional<QString> {
                const auto iterator = values.constFind(key);
                if (iterator == values.cend())
                    return std::nullopt;
                return iterator.value();
            },
            [this](const QString &key) { values.remove(key); },
        };
    }
};

SiteEntry site(QString id = QStringLiteral("site-1"),
               QString name = QStringLiteral("Example")) {
    SiteEntry entry;
    entry.siteId = std::move(id);
    entry.name = std::move(name);
    entry.opt.proxy_type = openscp::ProxyType::Socks5;
    return entry;
}

OPENSCP_TEST(testSaveLoadAndRemoval, test) {
    FakeSecretBackend backend;
    SiteCredentialRepository repository(backend.interface());
    const SiteEntry entry = site();

    openscp::SessionOptions options = entry.opt;
    options.password = "password";
    options.private_key_passphrase = "passphrase";
    options.proxy_password = "proxy";
    const auto saved = repository.save(entry, options);
    test.check(saved.anyCredentialHandled && saved.issues.isEmpty(),
               "all configured credentials should be saved");
    test.check(backend.values.size() == 3,
               "each credential should use an independent key");

    SiteCredentialRepository::clearCredentialFields(options);
    const auto loaded = repository.load(entry, options);
    test.check(loaded.anyCredentialHandled && options.password == "password" &&
                   options.private_key_passphrase == "passphrase" &&
                   options.proxy_password == "proxy",
               "saved credentials should round-trip into session options");

    SiteCredentialRepository::clearCredentialFields(options);
    const auto removed = repository.save(entry, options);
    Q_UNUSED(removed);
    test.check(backend.values.isEmpty(),
               "saving absent credentials should remove stale values");
}

OPENSCP_TEST(testLegacyNameMigrationAndCopy, test) {
    FakeSecretBackend backend;
    SiteCredentialRepository repository(backend.interface());
    const SiteEntry source =
        site(QStringLiteral("source-id"), QStringLiteral("Legacy"));
    backend.values.insert(SiteCredentialRepository::legacyNameKey(
                              source.name, SiteCredentialKind::Password),
                          QStringLiteral("legacy-password"));

    openscp::SessionOptions options = source.opt;
    const auto loaded = repository.load(source, options);
    const QString stable = SiteCredentialRepository::stableKey(
        source, SiteCredentialKind::Password);
    test.check(loaded.issues.isEmpty() &&
                   options.password == "legacy-password" &&
                   backend.values.contains(stable),
               "legacy name keys should migrate to stable site IDs");
    test.check(!backend.values.contains(SiteCredentialRepository::legacyNameKey(
                   source.name, SiteCredentialKind::Password)),
               "successful migration should remove the legacy key");

    SiteEntry target =
        site(QStringLiteral("target-id"), QStringLiteral("Duplicate"));
    const auto copied = repository.copy(source, target);
    test.check(copied.issues.isEmpty() &&
                   backend.values.value(SiteCredentialRepository::stableKey(
                       target, SiteCredentialKind::Password)) ==
                       QStringLiteral("legacy-password"),
               "copy should duplicate credentials under the target identity");
}

OPENSCP_TEST(testFailureReporting, test) {
    FakeSecretBackend backend;
    backend.saveStatus = SecretStore::PersistStatus::PermissionDenied;
    SiteCredentialRepository repository(backend.interface());
    SiteEntry entry = site();
    openscp::SessionOptions options = entry.opt;
    options.password = "secret";

    const auto result = repository.save(entry, options);
    test.check(!result.anyCredentialHandled && result.issues.size() == 1,
               "backend failures should be returned structurally");
    test.check(!result.issueMessages().value(0).isEmpty(),
               "backend failures should have a user-facing description");
}

} // namespace

int main(int argc, char **argv) {
    openscp::test::TestHarness harness("site credential repository");
    return harness.runWithApplication<QCoreApplication>(argc, argv);
}
