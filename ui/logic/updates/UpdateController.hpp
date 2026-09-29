#pragma once

#include "logic/updates/UpdateMetadata.hpp"

#include <QNetworkAccessManager>
#include <QPointer>
#include <QTemporaryFile>
#include <QThreadPool>

#include <functional>
#include <memory>

class QNetworkReply;

namespace openscpui {
class UpdateController : public QObject {
    Q_OBJECT
    public:
    explicit UpdateController(QObject *parent = nullptr,
                              QNetworkAccessManager *network = nullptr);
    // An injected manager is borrowed and must outlive this controller.
    ~UpdateController() override;
    void check(bool manual);
    void install(const UpdateArtifact &artifact);
    void cancel();
    void setCanInstall(std::function<bool()> canInstall) {
        canInstall_ = std::move(canInstall);
    }
    bool busy() const { return busy_; }
    bool installing() const { return installing_; }
    UpdateInstallation installation() const { return installation_; }

    signals:
    void available(const openscpui::UpdateRelease &release);
    void checked(bool manual, const QString &error);
    void progress(qint64 received, qint64 total);
    void installed(bool success, const QString &error);

    private:
    QNetworkReply *get(const QUrl &url);
    void request(
        const QUrl &url, qint64 limit,
        std::function<void(const QByteArray &, const QString &)> completion);
    void finishCheck(bool manual, const UpdateRelease &release);
    void publishDownloaded(const UpdateArtifact &artifact);

    QNetworkAccessManager *network_;
    QThreadPool worker_;
    QPointer<QNetworkReply> reply_;
    std::unique_ptr<QTemporaryFile> download_;
    UpdateInstallation installation_;
    QByteArray publicKey_;
    std::optional<UpdateArtifact> verifiedArtifact_;
    std::function<bool()> canInstall_;
    bool busy_ = false;
    bool installing_ = false;
    bool restartRequired_ = false;
};
} // namespace openscpui
