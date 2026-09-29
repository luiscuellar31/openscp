#pragma once

#include <QByteArray>
#include <QString>
#include <QUrl>

#include <optional>

namespace openscpui {

enum class UpdateInstallation { Manual, MacBundle, AppImage, Flatpak, Snap };

struct UpdateArtifact {
    QString name;
    QUrl url;
    qint64 size = 0;
    QByteArray sha256;
};

struct UpdateRelease {
    QString version;
    QUrl page;
    bool hasManifest = false;
    std::optional<UpdateArtifact> artifact;
};

inline constexpr qint64 kMaximumUpdateSize = 256 * 1024 * 1024;
inline constexpr qint64 kMaximumManifestSize = 64 * 1024;

QString updateArchitecture();
bool isUpdateVersion(const QString &candidate, const QString &current);
bool isTrustedUpdateUrl(const QUrl &url);
std::optional<UpdateRelease> parseUpdateRelease(const QByteArray &json,
                                                const QString &current,
                                                QString &error);
std::optional<UpdateArtifact>
verifyUpdateManifest(const QByteArray &json, const QByteArray &signature,
                     const QByteArray &publicKey, const QString &version,
                     const QString &architecture, const QString &glibcVersion,
                     QString &error);
UpdateInstallation detectUpdateInstallation();

} // namespace openscpui
