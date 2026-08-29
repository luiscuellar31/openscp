#include "AppSettings.hpp"

#include <QFile>
#include <QFileInfo>

namespace openscpui {
namespace {

QString storeApplicationName(AppSettings::Store store) {
    return store == AppSettings::Store::SecretFallback
               ? QStringLiteral("Secrets")
               : QStringLiteral("OpenSCP");
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

AppSettings::AppSettings(Store store)
    : QSettings(QStringLiteral("OpenSCP"), storeApplicationName(store)) {
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
