#include "logic/common/AppSettings.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace openscpui {
namespace {

// Settings follow whoever is running. A build that renames itself, such as
// the demo, then keeps its own store instead of writing into a real
// installation, and so does a test binary that names itself.
QString runningName(const QString &name) {
    return name.isEmpty() ? QStringLiteral("OpenSCP") : name;
}

QString storeApplicationName(AppSettings::Store store) {
    // The secret fallback keeps its fixed name so existing installations go
    // on finding the secrets they already stored.
    return store == AppSettings::Store::SecretFallback
               ? QStringLiteral("Secrets")
               : runningName(QCoreApplication::applicationName());
}

QString statusError(QSettings::Status status) {
    switch (status) {
    case QSettings::NoError:
        return {};
    case QSettings::AccessError:
        return QStringLiteral("The settings file could not be accessed.");
    case QSettings::FormatError:
        return QStringLiteral("The settings file has an invalid format.");
    }
    return QStringLiteral("The settings file could not be synchronized.");
}

} // namespace

QString defaultStagingRootPath() {
    return QDir(QDir::homePath())
        .filePath(QStringLiteral("Downloads/OpenSCP-Dragged"));
}

QString effectiveStagingRootPath(const QSettings &settings) {
    const QString configuredRoot =
        settings.value(settingskeys::kStagingRoot).toString();
    return configuredRoot.isEmpty() ? defaultStagingRootPath() : configuredRoot;
}

AppSettings::AppSettings(Store store)
    // Passing the format explicitly matters: the organization/application
    // constructor always uses NativeFormat, so a caller that redirects
    // settings, such as the test harness, would be ignored.
    : QSettings(QSettings::defaultFormat(), QSettings::UserScope,
                runningName(QCoreApplication::organizationName()),
                storeApplicationName(store)) {
}

AppSettings::AppSettings(const QString &organization,
                         const QString &application)
    : QSettings(organization, application) {
}

AppSettings::AppSettings(const QString &fileName, Format format)
    : QSettings(fileName, format) {
}

AppSettings::~AppSettings() {
    (void)syncSecure();
}

SettingsSyncResult AppSettings::syncSecure() {
    sync();
    if (status() != QSettings::NoError)
        return {false, statusError(status())};
    return ensureOwnerOnly();
}

SettingsSyncResult AppSettings::ensureOwnerOnly() const {
#ifdef Q_OS_UNIX
    const QString path = fileName();
    if (path.isEmpty() || !QFileInfo::exists(path))
        return {true, {}};

    constexpr QFileDevice::Permissions ownerOnly =
        QFileDevice::ReadOwner | QFileDevice::WriteOwner;
    if (!QFile::setPermissions(path, ownerOnly)) {
        return {false,
                QStringLiteral("Could not restrict settings permissions for %1")
                    .arg(path)};
    }

    constexpr QFileDevice::Permissions nonOwnerPermissions =
        QFileDevice::ReadGroup | QFileDevice::WriteGroup |
        QFileDevice::ExeGroup | QFileDevice::ReadOther |
        QFileDevice::WriteOther | QFileDevice::ExeOther;
    if ((QFileInfo(path).permissions() & nonOwnerPermissions) != 0) {
        return {false,
                QStringLiteral("Settings permissions are not owner-only: %1")
                    .arg(path)};
    }
#endif
    return {true, {}};
}

} // namespace openscpui
