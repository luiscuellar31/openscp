#include "logic/updates/AppImageInstaller.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUuid>

#ifdef Q_OS_LINUX
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace openscpui {
#ifdef Q_OS_LINUX
namespace {
struct Descriptor {
    explicit Descriptor(int descriptor) : value(descriptor) {}
    Descriptor(const Descriptor &) = delete;
    Descriptor &operator=(const Descriptor &) = delete;
    int value;
    ~Descriptor() {
        if (value >= 0)
            ::close(value);
    }
};
bool sameFile(const struct stat &a, const struct stat &b) {
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino &&
           a.st_size == b.st_size && a.st_mtim.tv_sec == b.st_mtim.tv_sec &&
           a.st_mtim.tv_nsec == b.st_mtim.tv_nsec &&
           a.st_ctim.tv_sec == b.st_ctim.tv_sec &&
           a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
}
bool sameContents(const struct stat &a, const struct stat &b) {
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino &&
           a.st_size == b.st_size && a.st_mode == b.st_mode &&
           a.st_mtim.tv_sec == b.st_mtim.tv_sec &&
           a.st_mtim.tv_nsec == b.st_mtim.tv_nsec;
}
} // namespace
#endif

bool installAppImageUpdate(const QString &original, const QString &download,
                           const UpdateArtifact &artifact, QString &error) {
    error = QStringLiteral("Could not safely replace the AppImage. The current "
                           "image has been preserved.");
#ifdef Q_OS_LINUX
    if (!QFileInfo(original).isAbsolute() || artifact.size <= 0 ||
        artifact.size > kMaximumUpdateSize || artifact.sha256.size() != 32)
        return false;
    const auto name = QFile::encodeName(QFileInfo(original).fileName());
    Descriptor directory{::open(
        QFile::encodeName(QFileInfo(original).absolutePath()).constData(),
        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)};
    struct stat directoryInfo {};
    if (directory.value < 0 || ::fstat(directory.value, &directoryInfo) != 0 ||
        directoryInfo.st_uid != ::geteuid() ||
        (directoryInfo.st_mode & 0022) != 0)
        return false;
    Descriptor old{::openat(directory.value, name.constData(),
                            O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC)};
    struct stat identity {};
    char header[11]{};
    if (old.value < 0 || ::fstat(old.value, &identity) != 0 ||
        !S_ISREG(identity.st_mode) || identity.st_uid != ::geteuid() ||
        (identity.st_mode & 06000) != 0 ||
        ::read(old.value, header, sizeof(header)) !=
            static_cast<ssize_t>(sizeof(header)) ||
        QByteArray(header, 4) != QByteArray("\x7f"
                                            "ELF",
                                            4) ||
        QByteArray(header + 8, 3) != QByteArray("AI\x02", 3))
        return false;
    const QByteArray lockName = name + ".update.lock";
    Descriptor lock{::openat(directory.value, lockName.constData(),
                             O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600)};
    struct stat lockInfo {};
    if (lock.value < 0 || ::fstat(lock.value, &lockInfo) != 0 ||
        !S_ISREG(lockInfo.st_mode) || lockInfo.st_uid != ::geteuid() ||
        lockInfo.st_nlink != 1 || ::flock(lock.value, LOCK_EX | LOCK_NB) != 0)
        return false;
    const QByteArray stagedName =
        ".openscp-update-" + QUuid::createUuid().toByteArray(QUuid::Id128);
    Descriptor staged{
        ::openat(directory.value, stagedName.constData(),
                 O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600)};
    if (staged.value < 0)
        return false;
    struct Cleanup {
        int directory;
        QByteArray name;
        ~Cleanup() { ::unlinkat(directory, name.constData(), 0); }
    } cleanup{directory.value, stagedName};
    Descriptor input{::open(QFile::encodeName(download).constData(),
                            O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC)};
    struct stat inputInfo {};
    if (input.value < 0 || ::fstat(input.value, &inputInfo) != 0 ||
        !S_ISREG(inputInfo.st_mode) || inputInfo.st_size != artifact.size)
        return false;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray buffer(256 * 1024, Qt::Uninitialized);
    qint64 total = 0;
    while (true) {
        const auto count = ::read(input.value, buffer.data(),
                                  static_cast<std::size_t>(buffer.size()));
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (count == 0)
            break;
        if (total == 0 && (count < 11 ||
                           QByteArrayView(buffer.constData(), 4) !=
                               QByteArrayView(header, 4) ||
                           QByteArrayView(buffer.constData() + 8, 3) !=
                               QByteArrayView(header + 8, 3)))
            return false;
        total += count;
        if (total > artifact.size)
            return false;
        // The QByteArray overload supports the oldest supported Qt 6.2.
        hash.addData(QByteArray::fromRawData(buffer.constData(), count));
        ssize_t written = 0;
        while (written < count) {
            const auto part =
                ::write(staged.value, buffer.constData() + written,
                        static_cast<std::size_t>(count - written));
            if (part < 0 && errno == EINTR)
                continue;
            if (part <= 0)
                return false;
            written += part;
        }
    }
    if (total != artifact.size || hash.result() != artifact.sha256 ||
        ::fchmod(staged.value, identity.st_mode & 0755) != 0 ||
        ::fsync(staged.value) != 0)
        return false;
    struct stat current {};
    if (::fstatat(directory.value, name.constData(), &current,
                  AT_SYMLINK_NOFOLLOW) != 0 ||
        !sameFile(identity, current))
        return false;
    const QByteArray temporaryBackup = stagedName + ".previous";
    Cleanup backupCleanup{directory.value, temporaryBackup};
    if (::linkat(directory.value, name.constData(), directory.value,
                 temporaryBackup.constData(), 0) != 0)
        return false;
    struct stat backupInfo {};
    if (::fstatat(directory.value, temporaryBackup.constData(), &backupInfo,
                  AT_SYMLINK_NOFOLLOW) != 0 ||
        backupInfo.st_dev != identity.st_dev ||
        backupInfo.st_ino != identity.st_ino)
        return false;
    // linkat changes ctime; revalidate identity after making the backup.
    struct stat linkedIdentity {};
    if (::fstat(old.value, &linkedIdentity) != 0 ||
        !sameContents(identity, linkedIdentity) ||
        ::fstatat(directory.value, name.constData(), &current,
                  AT_SYMLINK_NOFOLLOW) != 0 ||
        !sameFile(linkedIdentity, current) ||
        ::renameat(directory.value, temporaryBackup.constData(),
                   directory.value, (name + ".previous").constData()) != 0 ||
        ::fsync(directory.value) != 0 ||
        ::fstatat(directory.value, name.constData(), &current,
                  AT_SYMLINK_NOFOLLOW) != 0 ||
        !sameContents(identity, current) ||
        ::renameat(directory.value, stagedName.constData(), directory.value,
                   name.constData()) != 0)
        return false;
    if (::fsync(directory.value) != 0) {
        error = QStringLiteral(
            "The update was installed, but syncing its folder failed. The "
            "previous image is available as .previous.");
        return true;
    }
    error.clear();
    return true;
#else
    Q_UNUSED(original)
    Q_UNUSED(download)
    Q_UNUSED(artifact)
    return false;
#endif
}
} // namespace openscpui
