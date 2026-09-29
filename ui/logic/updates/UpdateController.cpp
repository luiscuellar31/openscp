#include "logic/updates/UpdateController.hpp"

#include "AppVersion.hpp"
#include "logic/common/AppSettings.hpp"
#include "logic/updates/AppImageInstaller.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRunnable>
#include <QThreadPool>
#include <QTimer>

#if defined(Q_OS_LINUX) && defined(__GLIBC__)
#include <gnu/libc-version.h>
#endif

namespace openscpui {
namespace {
QUrl manifestUrl(const QString &version, const QString &name) {
    return QUrl(
        QStringLiteral(
            "https://github.com/luiscuellar31/openscp/releases/download/v%1/%2")
            .arg(version, name));
}
QString glibcVersion() {
#if defined(Q_OS_LINUX) && defined(__GLIBC__)
    return QString::fromLatin1(gnu_get_libc_version());
#else
    return {};
#endif
}
} // namespace

UpdateController::UpdateController(QObject *parent,
                                   QNetworkAccessManager *network)
    : QObject(parent),
      network_(network ? network : new QNetworkAccessManager(this)),
      installation_(detectUpdateInstallation()),
      publicKey_(QByteArray::fromBase64(
          QByteArrayLiteral(OPENSCP_UPDATE_PUBLIC_KEY))) {
}

UpdateController::~UpdateController() {
    if (reply_) {
        disconnect(reply_, nullptr, this, nullptr);
        reply_->abort();
    }
    worker_.waitForDone();
}

void UpdateController::cancel() {
    if (reply_)
        reply_->abort();
}

QNetworkReply *UpdateController::get(const QUrl &url) {
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", "OpenSCP/" OPENSCP_APP_VERSION);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::UserVerifiedRedirectPolicy);
    request.setTransferTimeout(30000);
    auto *reply = network_->get(request);
    reply->setReadBufferSize(256 * 1024);
    QTimer::singleShot(30 * 60 * 1000, reply, [reply] { reply->abort(); });
    connect(reply, &QNetworkReply::redirected, this, [reply](const QUrl &next) {
        if (isTrustedUpdateUrl(next))
            reply->redirectAllowed();
        else
            reply->abort();
    });
    reply_ = reply;
    return reply;
}

void UpdateController::request(
    const QUrl &url, qint64 limit,
    std::function<void(const QByteArray &, const QString &)> completion) {
    auto *reply = get(url);
    QTimer::singleShot(60000, reply, [reply] { reply->abort(); });
    auto buffer = std::make_shared<QByteArray>();
    auto tooLarge = std::make_shared<bool>(false);
    connect(reply, &QNetworkReply::readyRead, this,
            [reply, buffer, tooLarge, limit] {
                if (reply->bytesAvailable() > limit - buffer->size()) {
                    *tooLarge = true;
                    reply->abort();
                    return;
                }
                buffer->append(reply->readAll());
            });
    connect(
        reply, &QNetworkReply::finished, this,
        [this, reply, buffer, tooLarge, limit,
         completion = std::move(completion)] {
            QString error;
            if (*tooLarge || reply->bytesAvailable() > limit - buffer->size())
                error =
                    QStringLiteral("Update response exceeds its size limit.");
            else if (reply->error() != QNetworkReply::NoError)
                error = reply->errorString();
            else if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                         .toInt() != 200)
                error = QStringLiteral("Unexpected update server response.");
            else
                buffer->append(reply->readAll());
            reply_ = nullptr;
            reply->deleteLater();
            completion(*buffer, error);
        });
}

void UpdateController::check(bool manual) {
    if (restartRequired_) {
        if (manual)
            emit checked(true,
                         QStringLiteral("An update has been installed. Restart "
                                        "OpenSCP before checking again."));
        return;
    }
    if (busy_) {
        if (manual)
            emit checked(
                true,
                QStringLiteral("An update operation is already in progress."));
        return;
    }
    AppSettings settings;
    const auto now = QDateTime::currentSecsSinceEpoch();
    const auto last =
        settings.value(settingskeys::kUpdateLastCheck, 0).toLongLong();
    if (!manual && (!settings.value(settingskeys::kUpdateAutomaticChecks, false)
                         .toBool() ||
                    (last >= 0 && last <= now && now - last < 24 * 60 * 60)))
        return;
    settings.setValue(settingskeys::kUpdateLastCheck, now);
    busy_ = true;
    request(
        QUrl(QStringLiteral("https://api.github.com/repos/luiscuellar31/"
                            "openscp/releases/latest")),
        1024 * 1024,
        [this, manual](const QByteArray &data, const QString &networkError) {
            QString error = networkError;
            auto release =
                error.isEmpty()
                    ? parseUpdateRelease(
                          data, QStringLiteral(OPENSCP_APP_VERSION), error)
                    : std::nullopt;
            if (!release) {
                busy_ = false;
                emit checked(manual, error);
                return;
            }
            if (installation_ != UpdateInstallation::AppImage ||
                publicKey_.size() != 32 || !release->hasManifest) {
                finishCheck(manual, *release);
                return;
            }
            request(
                manifestUrl(release->version,
                            QStringLiteral("update-manifest.json")),
                kMaximumManifestSize,
                [this, manual, release = *release](
                    const QByteArray &manifest, const QString &manifestError) {
                    if (!manifestError.isEmpty()) {
                        finishCheck(manual, release);
                        return;
                    }
                    request(manifestUrl(release.version,
                                        QStringLiteral("update-manifest.sig")),
                            64,
                            [this, manual, candidate = UpdateRelease(release),
                             manifest](const QByteArray &signature,
                                       const QString &signatureError) mutable {
                                QString verificationError = signatureError;
                                if (verificationError.isEmpty())
                                    candidate.artifact = verifyUpdateManifest(
                                        manifest, signature, publicKey_,
                                        candidate.version, updateArchitecture(),
                                        glibcVersion(), verificationError);
                                // Invalid/missing signatures allow only
                                // visiting the release page.
                                finishCheck(manual, candidate);
                            });
                });
        });
}

void UpdateController::finishCheck(bool manual, const UpdateRelease &release) {
    busy_ = false;
    verifiedArtifact_ = release.artifact;
    AppSettings settings;
    if (!manual &&
        settings.value(settingskeys::kUpdateSkippedVersion).toString() ==
            release.version)
        return;
    emit available(release);
}

void UpdateController::install(const UpdateArtifact &artifact) {
    if (busy_ || installation_ != UpdateInstallation::AppImage ||
        !verifiedArtifact_ || artifact.url != verifiedArtifact_->url ||
        artifact.sha256 != verifiedArtifact_->sha256 ||
        artifact.size != verifiedArtifact_->size || !canInstall_ ||
        !canInstall_() || !isTrustedUpdateUrl(artifact.url) ||
        artifact.size <= 0 || artifact.size > kMaximumUpdateSize ||
        artifact.sha256.size() != 32) {
        emit installed(false,
                       QStringLiteral("No verified update is ready to install, "
                                      "or file operations are pending."));
        return;
    }
    download_ = std::make_unique<QTemporaryFile>(
        QDir::tempPath() + QStringLiteral("/openscp-update-XXXXXX"));
    if (!download_->open()) {
        emit installed(false, download_->errorString());
        return;
    }
    busy_ = true;
    auto *reply = get(artifact.url);
    auto written = std::make_shared<qint64>(0);
    auto writeError = std::make_shared<bool>(false);
    auto drain = [this, reply, written, writeError, artifact] {
        if (*writeError)
            return;
        while (reply->bytesAvailable() > 0) {
            const QByteArray chunk = reply->read(256 * 1024);
            if (chunk.size() > artifact.size - *written ||
                download_->write(chunk) != chunk.size()) {
                *writeError = true;
                reply->abort();
                return;
            }
            *written += chunk.size();
            emit progress(*written, artifact.size);
        }
    };
    connect(reply, &QNetworkReply::readyRead, this, drain);
    connect(
        reply, &QNetworkReply::finished, this,
        [this, reply, written, writeError, artifact, drain] {
            drain();
            QString error;
            if (*writeError || *written != artifact.size || !download_->flush())
                error = QStringLiteral(
                    "The update download is incomplete or could not be saved.");
            else if (reply->error() != QNetworkReply::NoError)
                error = reply->errorString();
            else if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                         .toInt() != 200)
                error = QStringLiteral("Unexpected update server response.");
            reply_ = nullptr;
            reply->deleteLater();
            if (!error.isEmpty()) {
                busy_ = false;
                download_.reset();
                emit installed(false, error);
                return;
            }
            publishDownloaded(artifact);
        },
        Qt::QueuedConnection);
}

void UpdateController::publishDownloaded(const UpdateArtifact &artifact) {
    if (!canInstall_ || !canInstall_()) {
        busy_ = false;
        download_.reset();
        emit installed(false,
                       QStringLiteral("File operations are still pending."));
        return;
    }
    installing_ = true;
    const QString original = qEnvironmentVariable("APPIMAGE");
    const QString file = download_->fileName();
    // The destructor joins this worker before releasing the temporary file.
    worker_.start(QRunnable::create([this, original, file, artifact] {
        QString error;
        const bool success =
            installAppImageUpdate(original, file, artifact, error);
        QMetaObject::invokeMethod(
            this,
            [this, success, error] {
                installing_ = false;
                busy_ = false;
                restartRequired_ = success;
                verifiedArtifact_.reset();
                download_.reset();
                emit installed(success, error);
            },
            Qt::QueuedConnection);
    }));
}
} // namespace openscpui
