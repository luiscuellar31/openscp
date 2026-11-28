#include "logic/common/AppSettings.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <unistd.h>
#endif

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
        return QCoreApplication::translate(
            "AppSettings", "The settings file could not be accessed.");
    case QSettings::FormatError:
        return QCoreApplication::translate(
            "AppSettings", "The settings file has an invalid format.");
    }
    return QCoreApplication::translate(
        "AppSettings", "The settings file could not be synchronized.");
}

// QSettings writes a new file in place of the old one, with the old one's
// mode, or with the umask's when there is none: a new file would be readable
// by others until ensureOwnerOnly() restricts it. Created private first, it
// never is.
void createPrivateFileIfMissing(const QSettings &settings) {
#ifdef Q_OS_UNIX
    // The native format is that same INI file, except on macOS, where it is
    // a property list that the system writes.
#ifdef Q_OS_DARWIN
    const bool iniFile = settings.format() == QSettings::IniFormat;
#else
    const bool iniFile = settings.format() == QSettings::IniFormat ||
                         settings.format() == QSettings::NativeFormat;
#endif
    const QString path = settings.fileName();
    if (!iniFile || path.isEmpty() || QFileInfo::exists(path) ||
        settings.allKeys().isEmpty())
        return;
    QDir().mkpath(QFileInfo(path).absolutePath());
    const int descriptor =
        ::open(QFile::encodeName(path).constData(),
               O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (descriptor >= 0)
        (void)::close(descriptor);
#else
    Q_UNUSED(settings);
#endif
}

} // namespace

QString defaultStagingRootPath() {
    // Windows can move the Downloads folder, often onto another drive, and
    // Linux can name it in the user's language, such as ~/Descargas.
    const QString downloads =
        QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (!downloads.isEmpty())
        return QDir(downloads).filePath(QStringLiteral("OpenSCP-Dragged"));
    return QDir(QDir::homePath())
        .filePath(QStringLiteral("Downloads/OpenSCP-Dragged"));
}

QString effectiveStagingRootPath(const QSettings &settings) {
    const QString configuredRoot =
        settings.value(settingskeys::kStagingRoot).toString();
    return configuredRoot.isEmpty() ? defaultStagingRootPath() : configuredRoot;
}

int effectiveNoHostVerificationTtlMinutes(const QSettings &settings) {
    return qBound(1,
                  settings
                      .value(settingskeys::kNoHostVerificationTtlMinutes,
                             settingskeys::kDefaultNoHostVerificationTtlMinutes)
                      .toInt(),
                  120);
}

AppSettings::AppSettings(Store store)
    // Passing the format explicitly matters: the organization/application
    // constructor always uses NativeFormat, so a caller that redirects
    // settings, such as the test harness, would be ignored.
    : QSettings(QSettings::defaultFormat(), QSettings::UserScope,
                runningName(QCoreApplication::organizationName()),
                storeApplicationName(store)) {
    // Otherwise a key missing from the user's file is read from the
    // organization-wide file and the machine-wide one. With IniFormat, that
    // one is C:\ProgramData\OpenSCP\OpenSCP.ini on Windows, which any user
    // may create: another account could plant sites or weaker security
    // defaults there. OpenSCP keeps every setting in the user's own file.
    setFallbacksEnabled(false);
}

AppSettings::AppSettings(const QString &organization,
                         const QString &application)
    // As above: the plain organization/application constructor would always
    // use NativeFormat, the registry on Windows.
    : QSettings(QSettings::defaultFormat(), QSettings::UserScope, organization,
                application) {
    setFallbacksEnabled(false);
}

AppSettings::AppSettings(const QString &fileName, Format format)
    : QSettings(fileName, format) {
}

AppSettings::~AppSettings() {
    (void)syncSecure();
}

SettingsSyncResult AppSettings::syncSecure() {
    createPrivateFileIfMissing(*this);
    sync();
    if (status() != QSettings::NoError)
        return {false, statusError(status())};
    return ensureOwnerOnly();
}

SettingsSyncResult AppSettings::ensureOwnerOnly() const {
#ifdef Q_OS_UNIX
    const QString path = fileName();
    const QFileInfo file(path);
    if (path.isEmpty() || !file.exists())
        return {true, {}};

    constexpr QFileDevice::Permissions ownerOnly =
        QFileDevice::ReadOwner | QFileDevice::WriteOwner;
    constexpr QFileDevice::Permissions modeBits =
        ownerOnly | QFileDevice::ExeOwner | QFileDevice::ReadGroup |
        QFileDevice::WriteGroup | QFileDevice::ExeGroup |
        QFileDevice::ReadOther | QFileDevice::WriteOther |
        QFileDevice::ExeOther;
    // Every instance runs this when it closes, most of them after only
    // reading: setting the same mode again would still change the file's
    // metadata each time.
    if ((file.permissions() & modeBits) == ownerOnly)
        return {true, {}};
    if (!QFile::setPermissions(path, ownerOnly)) {
        return {false, QCoreApplication::translate(
                           "AppSettings",
                           "Could not restrict settings permissions for %1")
                           .arg(path)};
    }

    constexpr QFileDevice::Permissions nonOwnerPermissions =
        QFileDevice::ReadGroup | QFileDevice::WriteGroup |
        QFileDevice::ExeGroup | QFileDevice::ReadOther |
        QFileDevice::WriteOther | QFileDevice::ExeOther;
    if ((QFileInfo(path).permissions() & nonOwnerPermissions) != 0) {
        return {false, QCoreApplication::translate(
                           "AppSettings",
                           "Settings permissions are not owner-only: %1")
                           .arg(path)};
    }
#endif
    return {true, {}};
}

} // namespace openscpui
