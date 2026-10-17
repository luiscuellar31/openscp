#pragma once

#include <QObject>
#include <QString>

#include <chrono>
#include <memory>

class QLocalServer;
class QLockFile;

namespace openscpui {

// Keeps one OpenSCP per data folder, which holds the transfer queue, so that
// two processes never restore, run or save the same queue. The first process
// locks the folder and listens; a later one asks it to show its window, and
// then quits.
class SingleInstance : public QObject {
    Q_OBJECT

    public:
    enum class Role {
        // This process runs OpenSCP; activationRequested() reports later
        // launches.
        Primary,
        // The running OpenSCP was asked to show its window.
        Forwarded,
        // OpenSCP holds the folder but did not answer in time, as while it
        // starts or closes, or when it hangs.
        Unreachable,
    };

    explicit SingleInstance(QString dataDirectory, QObject *parent = nullptr);
    ~SingleInstance() override;

    // Becomes the primary instance or forwards to it, retrying until
    // `patience` runs out while the lock holder cannot be reached. A folder
    // that cannot hold a lock makes this process primary, unguarded.
    Role start(std::chrono::milliseconds patience = std::chrono::seconds(5));

    // The local socket name for a data folder: one per user and folder.
    static QString serverName(const QString &dataDirectory);

    signals:
    void activationRequested();

    private:
    void listen();
    bool askPrimaryToActivate() const;

    QString dataDirectory_;
    std::unique_ptr<QLockFile> lock_;
    QLocalServer *server_ = nullptr;
};

} // namespace openscpui
