#include "logic/updates/UpdateMetadata.hpp"

#include "AppVersion.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSysInfo>
#include <QVersionNumber>

#include <openssl/evp.h>

#include <memory>
#include <string>
#include <utility>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace openscpui {
namespace {
const QString kReleaseRoot =
    QStringLiteral("https://github.com/luiscuellar31/openscp/releases/");

// The AppId of the Inno Setup installer, which names its uninstall key. It
// must match AppId in packaging/windows/OpenSCP.iss and never change.
const QString kWindowsSetupAppId =
    QStringLiteral("{2C03B5C3-87DD-4DAB-B609-F89E831909E6}");

bool validVersion(const QString &text) {
    static const QRegularExpression pattern(QStringLiteral(
        "^(0|[1-9][0-9]{0,8})\\.(0|[1-9][0-9]{0,8})\\.(0|[1-9][0-9]{0,8})$"));
    return pattern.match(text).hasMatch();
}

QUrl assetUrl(const QString &version, const QString &name) {
    return QUrl(kReleaseRoot + QStringLiteral("download/v") + version +
                QLatin1Char('/') + name);
}

// The artifact list of a manifest that `publicKey` signed for `version`.
std::optional<QJsonArray> signedArtifacts(const QByteArray &json,
                                          const QByteArray &signature,
                                          const QByteArray &publicKey,
                                          const QString &version) {
    if (json.isEmpty() || json.size() > kMaximumManifestSize ||
        signature.size() != 64 || publicKey.size() != 32 ||
        !validVersion(version))
        return std::nullopt;
    using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
    using Context = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
    Key key(EVP_PKEY_new_raw_public_key(
                EVP_PKEY_ED25519, nullptr,
                reinterpret_cast<const unsigned char *>(publicKey.constData()),
                32),
            EVP_PKEY_free);
    Context context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!key || !context ||
        EVP_DigestVerifyInit(context.get(), nullptr, nullptr, nullptr,
                             key.get()) != 1 ||
        EVP_DigestVerify(
            context.get(),
            reinterpret_cast<const unsigned char *>(signature.constData()), 64,
            reinterpret_cast<const unsigned char *>(json.constData()),
            static_cast<std::size_t>(json.size())) != 1)
        return std::nullopt;
    const auto document = QJsonDocument::fromJson(json);
    const auto root = document.object();
    if (!document.isObject() || root.value("schema").toInt() != 1 ||
        root.value("version").toString() != version ||
        !root.value("artifacts").isArray())
        return std::nullopt;
    return root.value("artifacts").toArray();
}

// The artifact that a manifest entry describes, if its size and digest are
// well formed.
std::optional<UpdateArtifact> artifactFromEntry(const QJsonObject &entry,
                                                const QString &name,
                                                const QString &version) {
    const auto sizeValue = entry.value("size");
    const qint64 size = sizeValue.toInteger(-1);
    const QByteArray hash = entry.value("sha256").toString().toLatin1();
    static const QRegularExpression hashPattern(
        QStringLiteral("^[0-9a-f]{64}$"));
    if (!sizeValue.isDouble() ||
        sizeValue.toDouble() != static_cast<double>(size) || size <= 0 ||
        size > kMaximumUpdateSize ||
        !hashPattern.match(QString::fromLatin1(hash)).hasMatch())
        return std::nullopt;
    return UpdateArtifact{name, assetUrl(version, name), size,
                          QByteArray::fromHex(hash)};
}
} // namespace

QString updateArchitecture() {
    const QString arch = QSysInfo::buildCpuArchitecture();
    return arch == QStringLiteral("arm64") ? QStringLiteral("aarch64") : arch;
}

bool isUpdateVersion(const QString &candidate, const QString &current) {
    return validVersion(candidate) && validVersion(current) &&
           QVersionNumber::fromString(candidate) >
               QVersionNumber::fromString(current);
}

bool isTrustedUpdateUrl(const QUrl &url) {
    if (!url.isValid() || url.scheme() != QStringLiteral("https") ||
        !url.userInfo().isEmpty() || (url.port(-1) != -1 && url.port() != 443))
        return false;
    const QString host = url.host().toLower();
    return host == QStringLiteral("api.github.com") ||
           host == QStringLiteral("github.com") ||
           host == QStringLiteral("objects.githubusercontent.com") ||
           host == QStringLiteral("release-assets.githubusercontent.com");
}

std::optional<UpdateRelease> parseUpdateRelease(const QByteArray &json,
                                                const QString &current,
                                                QString &error) {
    error.clear();
    if (json.size() > 1024 * 1024) {
        error = QStringLiteral("Release response is too large.");
        return std::nullopt;
    }
    const auto document = QJsonDocument::fromJson(json);
    const auto object = document.object();
    if (!document.isObject() || !object.value("draft").isBool() ||
        !object.value("prerelease").isBool() ||
        !object.value("assets").isArray()) {
        error = QStringLiteral("Invalid release response.");
        return std::nullopt;
    }
    if (object.value("draft").toBool() || object.value("prerelease").toBool())
        return std::nullopt;
    const QString tag = object.value("tag_name").toString();
    if (!tag.startsWith(QLatin1Char('v')) || !validVersion(tag.mid(1))) {
        error = QStringLiteral("Invalid release version.");
        return std::nullopt;
    }
    if (!isUpdateVersion(tag.mid(1), current))
        return std::nullopt;
    UpdateRelease release;
    release.version = tag.mid(1);
    release.page = QUrl(kReleaseRoot + QStringLiteral("tag/") + tag);
    bool manifest = false;
    bool signature = false;
    for (const auto &asset : object.value("assets").toArray()) {
        const auto name = asset.toObject().value("name").toString();
        manifest |= name == QStringLiteral("update-manifest.json");
        signature |= name == QStringLiteral("update-manifest.sig");
    }
    release.hasManifest = manifest && signature;
    return release;
}

std::optional<UpdateArtifact>
verifyUpdateManifest(const QByteArray &json, const QByteArray &signature,
                     const QByteArray &publicKey, const QString &version,
                     const QString &architecture, const QString &glibcVersion,
                     QString &error) {
    error = QStringLiteral("The update signature or manifest is invalid.");
    const auto artifacts = signedArtifacts(json, signature, publicKey, version);
    if (!artifacts)
        return std::nullopt;
    if (architecture != QStringLiteral("x86_64") &&
        architecture != QStringLiteral("aarch64"))
        return std::nullopt;
    const QString expected =
        QStringLiteral("OpenSCP-%1-%2.AppImage").arg(version, architecture);
    std::optional<UpdateArtifact> result;
    for (const auto &value : *artifacts) {
        const auto object = value.toObject();
        if (object.value("name").toString() != expected)
            continue;
        const QString minimum = object.value("minimum_glibc").toString();
        static const QRegularExpression glibcPattern(
            QStringLiteral("^[0-9]{1,3}\\.[0-9]{1,3}$"));
        if (!glibcPattern.match(minimum).hasMatch() ||
            !glibcPattern.match(glibcVersion).hasMatch() ||
            QVersionNumber::fromString(glibcVersion) <
                QVersionNumber::fromString(minimum)) {
            error =
                QStringLiteral("This update requires a newer Linux system.");
            return std::nullopt;
        }
        const auto artifact = artifactFromEntry(object, expected, version);
        if (result || !artifact)
            return std::nullopt;
        result = artifact;
    }
    if (result)
        error.clear();
    return result;
}

std::optional<UpdateArtifact>
verifyWindowsUpdateManifest(const QByteArray &json, const QByteArray &signature,
                            const QByteArray &publicKey, const QString &version,
                            const QString &architecture, int windowsBuild,
                            QString &error) {
    error = QStringLiteral("The update signature or manifest is invalid.");
    const auto artifacts = signedArtifacts(json, signature, publicKey, version);
    if (!artifacts || architecture != QStringLiteral("x86_64"))
        return std::nullopt;
    const QString expected =
        QStringLiteral("OpenSCP-%1-x64-setup.exe").arg(version);
    std::optional<UpdateArtifact> result;
    for (const auto &value : *artifacts) {
        const auto object = value.toObject();
        if (object.value("name").toString() != expected)
            continue;
        const auto minimumValue = object.value("minimum_windows_build");
        const qint64 minimum = minimumValue.toInteger(-1);
        if (!minimumValue.isDouble() ||
            minimumValue.toDouble() != static_cast<double>(minimum) ||
            minimum <= 0 || minimum > 1000000)
            return std::nullopt;
        if (windowsBuild < minimum) {
            error = QStringLiteral(
                "This update requires a newer version of Windows.");
            return std::nullopt;
        }
        const auto artifact = artifactFromEntry(object, expected, version);
        if (result || !artifact)
            return std::nullopt;
        result = artifact;
    }
    if (result)
        error.clear();
    return result;
}

UpdateInstallation detectUpdateInstallation() {
    if (QFileInfo::exists(QStringLiteral("/.flatpak-info")))
        return UpdateInstallation::Flatpak;
    if (!qEnvironmentVariableIsEmpty("SNAP"))
        return UpdateInstallation::Snap;
#ifdef Q_OS_LINUX
    if (QStringLiteral(OPENSCP_PACKAGE_KIND) == QStringLiteral("appimage")) {
        const QString directory =
            QFileInfo(qEnvironmentVariable("APPDIR")).canonicalFilePath();
        const QString executable =
            QFileInfo(QCoreApplication::applicationFilePath())
                .canonicalFilePath();
        if (!directory.isEmpty() &&
            executable == directory + QStringLiteral("/usr/bin/openscp") &&
            QFileInfo(qEnvironmentVariable("APPIMAGE")).isAbsolute())
            return UpdateInstallation::AppImage;
    }
#endif
#ifdef Q_OS_MACOS
    if (QCoreApplication::applicationDirPath().endsWith(
            QStringLiteral(".app/Contents/MacOS")))
        return UpdateInstallation::MacBundle;
#endif
    if (windowsSetupScope())
        return UpdateInstallation::WindowsSetup;
    return UpdateInstallation::Manual;
}

std::optional<WindowsSetupScope> windowsSetupScope() {
    return windowsSetupScope(kWindowsSetupAppId,
                             QCoreApplication::applicationDirPath());
}

std::optional<WindowsSetupScope>
windowsSetupScope(const QString &appId, const QString &applicationDirectory) {
#ifdef Q_OS_WIN
    // Inno Setup records where it installed in its uninstall key: in the
    // user's hive for that user, in the machine's for all users.
    const std::wstring key =
        (QStringLiteral("Software\\Microsoft\\Windows\\CurrentVersion"
                        "\\Uninstall\\") +
         appId + QStringLiteral("_is1"))
            .toStdWString();
    const QString directory =
        QDir::cleanPath(QDir::fromNativeSeparators(applicationDirectory));
    const std::pair<HKEY, WindowsSetupScope> hives[] = {
        {HKEY_CURRENT_USER, WindowsSetupScope::CurrentUser},
        {HKEY_LOCAL_MACHINE, WindowsSetupScope::AllUsers},
    };
    for (const auto &[hive, scope] : hives) {
        // Read only: QSettings would create the key it opens.
        DWORD size = 0;
        if (RegGetValueW(hive, key.c_str(), L"InstallLocation", RRF_RT_REG_SZ,
                         nullptr, nullptr, &size) != ERROR_SUCCESS)
            continue;
        std::wstring buffer(size / sizeof(wchar_t) + 1, L'\0');
        size = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
        if (RegGetValueW(hive, key.c_str(), L"InstallLocation", RRF_RT_REG_SZ,
                         nullptr, buffer.data(), &size) != ERROR_SUCCESS)
            continue;
        const QString location = QString::fromWCharArray(buffer.c_str());
        if (!location.isEmpty() &&
            QDir::cleanPath(QDir::fromNativeSeparators(location))
                    .compare(directory, Qt::CaseInsensitive) == 0)
            return scope;
    }
#else
    Q_UNUSED(appId)
    Q_UNUSED(applicationDirectory)
#endif
    return std::nullopt;
}
} // namespace openscpui
