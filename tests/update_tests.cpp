#include "TestHarness.hpp"
#include "logic/common/AppSettings.hpp"
#include "logic/updates/AppImageInstaller.hpp"
#include "logic/updates/UpdateController.hpp"
#include "logic/updates/UpdateMetadata.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QNetworkReply>
#include <QTemporaryDir>
#include <QTimer>

#include <openssl/evp.h>

#include <cstring>
#include <memory>

#ifdef Q_OS_LINUX
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace {
using namespace openscpui;
using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
struct SigningFixture {
    // Deterministic test key, never used by production or release scripts.
    QByteArray seed = QByteArray(32, 'T');
    Key key{EVP_PKEY_new_raw_private_key(
                EVP_PKEY_ED25519, nullptr,
                reinterpret_cast<const unsigned char *>(seed.constData()), 32),
            EVP_PKEY_free};
    QByteArray publicKey() const {
        QByteArray result(32, Qt::Uninitialized);
        std::size_t size = 32;
        if (EVP_PKEY_get_raw_public_key(
                key.get(), reinterpret_cast<unsigned char *>(result.data()),
                &size) != 1)
            return {};
        return result;
    }
    QByteArray sign(const QByteArray &data) const {
        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(
            EVP_MD_CTX_new(), EVP_MD_CTX_free);
        QByteArray result(64, Qt::Uninitialized);
        std::size_t size = 64;
        if (EVP_DigestSignInit(context.get(), nullptr, nullptr, nullptr,
                               key.get()) != 1 ||
            EVP_DigestSign(
                context.get(), reinterpret_cast<unsigned char *>(result.data()),
                &size,
                reinterpret_cast<const unsigned char *>(data.constData()),
                static_cast<std::size_t>(data.size())) != 1)
            return {};
        return result;
    }
};

QByteArray manifest(bool duplicate = false) {
    const QJsonObject artifact{{"name", "OpenSCP-1.2.0-x86_64.AppImage"},
                               {"size", 1024},
                               {"sha256", QString(64, QLatin1Char('a'))},
                               {"minimum_glibc", "2.34"}};
    QJsonArray artifacts{artifact};
    if (duplicate)
        artifacts.append(artifact);
    return QJsonDocument(QJsonObject{{"schema", 1},
                                     {"version", "1.2.0"},
                                     {"artifacts", artifacts}})
        .toJson(QJsonDocument::Compact);
}

OPENSCP_TEST(testReleaseAndUrlValidation, test) {
    test.check(isUpdateVersion("1.10.0", "1.9.9"),
               "versions compare numerically");
    for (const auto &version :
         {"1.0.0", "1.1.0-beta", "01.3.0", "1.3", "../1.3.0", "9999999999.0.0"})
        test.check(!isUpdateVersion(QString::fromLatin1(version), "1.1.0"),
                   "reject downgrades and invalid tags");
    for (const auto &url :
         {"http://github.com/a", "https://github.com.evil.org/a",
          "https://github.com:444/a", "https://user@github.com/a",
          "file:///tmp/a"})
        test.check(!isTrustedUpdateUrl(QUrl(QString::fromLatin1(url))),
                   "reject insecure or foreign URLs");
    test.check(isTrustedUpdateUrl(
                   QUrl("https://release-assets.githubusercontent.com/a")),
               "allow GitHub asset redirects");
    QString error;
    const QByteArray release =
        R"({"tag_name":"v1.2.0","draft":false,"prerelease":false,"html_url":"https://evil.org","assets":[{"name":"update-manifest.json"},{"name":"update-manifest.sig"}]})";
    auto parsed = parseUpdateRelease(release, "1.1.0", error);
    test.check(parsed && parsed->hasManifest &&
                   parsed->page.host() == "github.com",
               "construct trusted release page independently of "
               "server-supplied links");
    test.check(!parseUpdateRelease(release, "1.2.0", error) && error.isEmpty(),
               "current version produces no update");
    auto draft = release;
    draft.replace("\"draft\":false", "\"draft\":true");
    test.check(!parseUpdateRelease(draft, "1.1.0", error),
               "draft releases are never offered");
    auto prerelease = release;
    prerelease.replace("\"prerelease\":false", "\"prerelease\":true");
    test.check(!parseUpdateRelease(prerelease, "1.1.0", error),
               "prereleases are never offered");
    test.check(
        !parseUpdateRelease(QByteArray(1024 * 1024 + 1, 'x'), "1.1.0", error) &&
            !error.isEmpty(),
        "bound untrusted metadata");
}

OPENSCP_TEST(testManifestAuthenticationAndCompatibility, test) {
    SigningFixture fixture;
    const auto data = manifest();
    const auto signature = fixture.sign(data);
    QString error;
    auto verify = [&](const QByteArray &source, const QByteArray &sig,
                      const QString &version = "1.2.0",
                      const QString &arch = "x86_64",
                      const QString &glibc = "2.34") {
        return verifyUpdateManifest(source, sig, fixture.publicKey(), version,
                                    arch, glibc, error);
    };
    auto artifact = verify(data, signature);
    test.check(artifact && artifact->size == 1024 &&
                   artifact->url.path().endsWith(
                       "/v1.2.0/OpenSCP-1.2.0-x86_64.AppImage"),
               "accept signed matching artifact");
    test.check(!verify(data + ' ', signature),
               "authenticate exact manifest bytes");
    auto altered = signature;
    altered[0] = static_cast<char>(altered[0] ^ 1);
    test.check(!verify(data, altered), "reject forged signatures");
    test.check(!verify(data, signature.left(63)),
               "reject truncated signatures");
    test.check(!verify(data, signature, "1.3.0"),
               "bind signed manifest to requested release version");
    test.check(!verify(data, signature, "1.2.0", "aarch64"),
               "never install another architecture");
    test.check(!verify(data, signature, "1.2.0", "x86_64", "2.33"),
               "reject incompatible glibc baseline");
    const auto repeated = manifest(true);
    test.check(!verify(repeated, fixture.sign(repeated)),
               "reject ambiguous artifact duplicates");
    auto huge = data;
    huge.replace("1024", "268435457");
    test.check(!verify(huge, fixture.sign(huge)),
               "reject authenticated oversized artifacts");
}

class FakeReply : public QNetworkReply {
    public:
    FakeReply(const QNetworkRequest &request, QByteArray data, QObject *parent)
        : QNetworkReply(parent), data_(std::move(data)) {
        setRequest(request);
        setUrl(request.url());
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, 200);
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        QTimer::singleShot(0, this, [this] {
            if (isFinished())
                return;
            emit readyRead();
            if (!isFinished()) {
                setFinished(true);
                emit finished();
            }
        });
    }
    void abort() override {
        if (isFinished())
            return;
        setError(OperationCanceledError, "Canceled");
        setFinished(true);
        emit finished();
    }
    qint64 bytesAvailable() const override {
        return data_.size() - position_ + QNetworkReply::bytesAvailable();
    }

    protected:
    qint64 readData(char *buffer, qint64 maximum) override {
        const qint64 count =
            qMin(maximum, static_cast<qint64>(data_.size()) - position_);
        if (count <= 0)
            return -1;
        std::memcpy(buffer, data_.constData() + position_,
                    static_cast<std::size_t>(count));
        position_ += count;
        return count;
    }

    private:
    QByteArray data_;
    qint64 position_ = 0;
};
class FakeNetwork : public QNetworkAccessManager {
    public:
    QByteArray response;
    QList<QByteArray> responses;
    int requests = 0;

    protected:
    QNetworkReply *createRequest(Operation, const QNetworkRequest &request,
                                 QIODevice *) override {
        ++requests;
        return new FakeReply(
            request, responses.isEmpty() ? response : responses.takeFirst(),
            this);
    }
};

OPENSCP_TEST(testCheckOptInLimitsAndCancellation, test) {
    QTemporaryDir settingsDir;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       settingsDir.path());
    FakeNetwork network;
    network.response =
        R"({"tag_name":"v1.1.0","draft":false,"prerelease":false,"assets":[]})";
    UpdateController controller(nullptr, &network);
    controller.check(false);
    test.check(network.requests == 0, "automatic checking is opt in");
    AppSettings().setValue(settingskeys::kUpdateAutomaticChecks, true);
    bool completed = false;
    QString error;
    QObject::connect(&controller, &UpdateController::checked, &controller,
                     [&](bool, const QString &message) {
                         completed = true;
                         error = message;
                     });
    auto wait = [&] {
        QEventLoop loop;
        QTimer deadline;
        deadline.setSingleShot(true);
        QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
        const auto connection = QObject::connect(
            &controller, &UpdateController::checked, &loop, &QEventLoop::quit);
        deadline.start(1000);
        if (!completed)
            loop.exec();
        QObject::disconnect(connection);
    };
    controller.check(false);
    wait();
    test.check(completed && error.isEmpty() && !controller.busy(),
               "async check completes without installation");
    controller.check(false);
    test.check(network.requests == 1,
               "automatic checks are limited to once per day");
    completed = false;
    network.response = QByteArray(1024 * 1024 + 1, 'x');
    controller.check(true);
    wait();
    test.check(completed && !error.isEmpty() && !controller.busy(),
               "oversized network replies fail closed");
    completed = false;
    controller.check(true);
    controller.cancel();
    test.check(completed && !error.isEmpty() && !controller.busy(),
               "cancellation releases the update state");
    bool rejected = false;
    QObject::connect(
        &controller, &UpdateController::installed, &controller,
        [&](bool success, const QString &) { rejected = !success; });
    controller.install(UpdateArtifact{"bad", QUrl("https://github.com/bad"), 1,
                                      QByteArray(32, 'a')});
    test.check(rejected && !controller.installing(),
               "unverified artifacts cannot reach installation");
}

#ifdef Q_OS_LINUX
QByteArray image(const char *payload) {
    QByteArray data(64, '\0');
    data.replace(0, 4,
                 QByteArray("\x7f"
                            "ELF",
                            4));
    data.replace(8, 3, QByteArray("AI\x02", 3));
    return data + payload;
}
void writeFile(const QString &path, const QByteArray &data) {
    QFile file(path);
    if (file.open(QIODevice::WriteOnly))
        file.write(data);
    file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
}
QByteArray readFile(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
OPENSCP_TEST(testAtomicAppImageInstallAndFailurePreservation, test) {
    QTemporaryDir directory;
    const QString original = directory.filePath("OpenSCP.AppImage");
    const QString download = directory.filePath("download");
    const auto before = image("original");
    const auto after = image("updated");
    writeFile(original, before);
    writeFile(download, after);
    UpdateArtifact artifact{
        "",
        {},
        after.size(),
        QCryptographicHash::hash(after, QCryptographicHash::Sha256)};
    QString error;
    auto bad = artifact;
    bad.sha256[0] = static_cast<char>(bad.sha256[0] ^ 1);
    test.check(!installAppImageUpdate(original, download, bad, error) &&
                   readFile(original) == before,
               "hash failure preserves current image");
    writeFile(download, QByteArray(after.size(), 'x'));
    bad.sha256 = QCryptographicHash::hash(QByteArray(after.size(), 'x'),
                                          QCryptographicHash::Sha256);
    test.check(!installAppImageUpdate(original, download, bad, error) &&
                   readFile(original) == before,
               "reject signed content that is not an AppImage");
    writeFile(download, after);
    bad = artifact;
    bad.size += 1;
    test.check(!installAppImageUpdate(original, download, bad, error) &&
                   readFile(original) == before,
               "incomplete file preserves current image");
    const auto lockPath = QFile::encodeName(original + ".update.lock");
    const int lock = ::open(lockPath.constData(), O_RDWR | O_CREAT, 0600);
    test.check(lock >= 0 && ::flock(lock, LOCK_EX | LOCK_NB) == 0,
               "test owns the update lock");
    test.check(!installAppImageUpdate(original, download, artifact, error),
               "concurrent installers cannot replace the same image");
    if (lock >= 0)
        ::close(lock);
    const QString linked = directory.filePath("linked.AppImage");
    QFile::link(original, linked);
    test.check(!installAppImageUpdate(linked, download, artifact, error) &&
                   readFile(original) == before,
               "reject symlink destination");
    test.check(installAppImageUpdate(original, download, artifact, error),
               "install authenticated image atomically");
    test.check(readFile(original) == after &&
                   readFile(original + ".previous") == before,
               "preserve a usable previous version");
    test.check(QFileInfo(original).isExecutable(),
               "preserve executable permissions");
    QFile::setPermissions(directory.path(),
                          QFile::ReadOwner | QFile::WriteOwner |
                              QFile::ExeOwner | QFile::WriteOther);
    test.check(!installAppImageUpdate(original, download, artifact, error),
               "reject a publicly writable installation folder");
    QFile::setPermissions(directory.path(), QFile::ReadOwner |
                                                QFile::WriteOwner |
                                                QFile::ExeOwner);
}

OPENSCP_TEST(testAppImageDownloadThroughVerifiedMetadata, test) {
    // Run this additional integration path by building with the fixture public
    // key and launching the test binary as APPDIR/usr/bin/openscp. Normal
    // source builds remain manual and cannot be tricked into installation by
    // APPIMAGE.
    if (!qEnvironmentVariableIsSet("OPENSCP_TEST_APPIMAGE_FLOW"))
        return;
    test.check(detectUpdateInstallation() == UpdateInstallation::AppImage,
               "the integration fixture must run from its AppDir");
    QTemporaryDir directory;
    const QString original = directory.filePath("OpenSCP.AppImage");
    const auto before = image("original");
    const auto after = image("updated") + QByteArray(1024 * 1024, 'x');
    qputenv("APPIMAGE", QFile::encodeName(original));
    SigningFixture fixture;
    const QString version = QStringLiteral("999.0.0");
    const QString name = QStringLiteral("OpenSCP-%1-%2.AppImage")
                             .arg(version, updateArchitecture());
    const QByteArray signedManifest =
        QJsonDocument(
            QJsonObject{
                {"schema", 1},
                {"version", version},
                {"artifacts",
                 QJsonArray{QJsonObject{
                     {"name", name},
                     {"size", after.size()},
                     {"sha256",
                      QString::fromLatin1(QCryptographicHash::hash(
                                              after, QCryptographicHash::Sha256)
                                              .toHex())},
                     {"minimum_glibc", "2.34"}}}}})
            .toJson(QJsonDocument::Compact);
    const QByteArray release =
        R"({"tag_name":"v999.0.0","draft":false,"prerelease":false,"assets":[{"name":"update-manifest.json"},{"name":"update-manifest.sig"}]})";
    for (bool corrupt : {true, false}) {
        writeFile(original, before);
        FakeNetwork network;
        auto payload = after;
        if (corrupt)
            payload[payload.size() - 1] = 'y';
        network.responses = {release, signedManifest,
                             fixture.sign(signedManifest), payload};
        UpdateController controller(nullptr, &network);
        controller.setCanInstall([] { return true; });
        QEventLoop loop;
        QTimer deadline;
        deadline.setSingleShot(true);
        QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
        bool offered = false;
        bool completed = false;
        bool installed = false;
        QObject::connect(&controller, &UpdateController::available, &controller,
                         [&](const UpdateRelease &update) {
                             offered = update.artifact.has_value();
                             if (update.artifact)
                                 controller.install(*update.artifact);
                             else
                                 loop.quit();
                         });
        QObject::connect(&controller, &UpdateController::installed, &controller,
                         [&](bool success, const QString &) {
                             completed = true;
                             installed = success;
                             loop.quit();
                         });
        deadline.start(5000);
        controller.check(true);
        loop.exec();
        test.check(offered && completed && installed == !corrupt &&
                       network.requests == 4,
                   "verified metadata controls streaming downloads and rejects "
                   "corrupted payloads");
        test.check(readFile(original) == (corrupt ? before : after),
                   "download completion preserves or atomically updates the "
                   "installed image");
        if (installed) {
            test.check(readFile(original + ".previous") == before,
                       "the complete download flow preserves a rollback image");
            controller.check(true);
            test.check(network.requests == 4,
                       "no further updates until the new version is launched");
        }
    }
    qunsetenv("APPIMAGE");
}
#endif
} // namespace

int main(int argc, char **argv) {
    openscp::test::TestHarness harness("updates");
    return harness.runWithApplication<QCoreApplication>(argc, argv);
}
