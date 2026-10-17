#include "logic/common/SingleInstance.hpp"

#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QDir>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QThread>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace openscpui {

namespace {

constexpr char kActivationMessage[] = "openscp-activate\n";
constexpr qint64 kMaxMessageBytes = 64;

// Where the socket goes on Unix, if not in the temporary folder.
QString socketDirectory() {
#ifdef Q_OS_LINUX
    // Every Flatpak instance gets its own /tmp, but shares this folder with
    // the other instances of the same app.
    const QString flatpakId = qEnvironmentVariable("FLATPAK_ID");
    const QString runtimeDirectory = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (!flatpakId.isEmpty() && !runtimeDirectory.isEmpty())
        return runtimeDirectory + QStringLiteral("/app/") + flatpakId;
#endif
    return {};
}

} // namespace

SingleInstance::SingleInstance(QString dataDirectory, QObject *parent)
    : QObject(parent), dataDirectory_(std::move(dataDirectory)) {
}

SingleInstance::~SingleInstance() = default;

QString SingleInstance::serverName(const QString &dataDirectory) {
    // Windows pipe names and the temporary folder of Linux are shared by all
    // users, so the name depends on the folder, which is the user's own.
    const QByteArray digest =
        QCryptographicHash::hash(QDir(dataDirectory).absolutePath().toUtf8(),
                                 QCryptographicHash::Sha256)
            .toHex()
            .left(16);
    const QString name =
        QStringLiteral("openscp-") + QString::fromLatin1(digest);
    const QString directory = socketDirectory();
    return directory.isEmpty() ? name : directory + QLatin1Char('/') + name;
}

SingleInstance::Role SingleInstance::start(std::chrono::milliseconds patience) {
    if (!QDir().mkpath(dataDirectory_))
        return Role::Primary;
    lock_ = std::make_unique<QLockFile>(
        QDir(dataDirectory_).filePath(QStringLiteral("instance.lock")));
    // A lock is stale only when its process has ended, however old it is.
    lock_->setStaleLockTime(0);

    const QDeadlineTimer deadline(patience);
    while (true) {
        if (lock_->tryLock(0)) {
            listen();
            return Role::Primary;
        }
        if (lock_->error() != QLockFile::LockFailedError) {
            // OpenSCP runs as before when it cannot lock its data folder.
            lock_.reset();
            return Role::Primary;
        }
        if (askPrimaryToActivate())
            return Role::Forwarded;
        // The lock holder may still be starting, or be closing and about to
        // release the lock.
        if (deadline.hasExpired())
            return Role::Unreachable;
        QThread::msleep(100);
    }
}

void SingleInstance::listen() {
    const QString name = serverName(dataDirectory_);
    server_ = new QLocalServer(this);
    server_->setSocketOptions(QLocalServer::UserAccessOption);
    // A socket file left by a process that crashed; the lock is ours now.
    QLocalServer::removeServer(name);
    if (!server_->listen(name))
        return;

    connect(server_, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket *client = server_->nextPendingConnection()) {
            connect(client, &QLocalSocket::disconnected, client,
                    &QObject::deleteLater);
            const auto readMessage = [this, client] {
                if (!client->canReadLine()) {
                    if (client->bytesAvailable() > kMaxMessageBytes)
                        client->abort();
                    return;
                }
                if (client->readLine(kMaxMessageBytes) ==
                    QByteArrayView(kActivationMessage)) {
                    emit activationRequested();
                }
                client->disconnectFromServer();
            };
            connect(client, &QLocalSocket::readyRead, this, readMessage);
            // The message may have arrived with the connection.
            readMessage();
        }
    });
}

bool SingleInstance::askPrimaryToActivate() const {
#ifdef Q_OS_WIN
    // Windows lets only the program that the user just started bring a
    // window to the front, unless that program passes the right on.
    qint64 pid = 0;
    QString hostname;
    QString appname;
    if (lock_->getLockInfo(&pid, &hostname, &appname) && pid > 0)
        AllowSetForegroundWindow(static_cast<DWORD>(pid));
#endif

    QLocalSocket socket;
    socket.connectToServer(serverName(dataDirectory_));
    if (!socket.waitForConnected(500))
        return false;
    const QByteArrayView message(kActivationMessage);
    const bool sent =
        socket.write(message.data(), message.size()) == message.size() &&
        (socket.bytesToWrite() == 0 || socket.waitForBytesWritten(1000));
    socket.disconnectFromServer();
    if (socket.state() != QLocalSocket::UnconnectedState)
        socket.waitForDisconnected(1000);
    return sent;
}

} // namespace openscpui
