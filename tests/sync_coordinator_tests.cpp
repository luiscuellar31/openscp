#include "PlatformTestSupport.hpp"
#include "QtTestSupport.hpp"
#include "TestHarness.hpp"
#include "logic/remote/RemoteOperationController.hpp"
#include "logic/sync/SyncCoordinator.hpp"
#include "logic/transfers/TransferManager.hpp"
#include "mock/MockSftpClient.hpp"
#include "sync/SyncComparisonEngine.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <functional>
#include <optional>
#include <thread>
#include <utility>

#ifdef _WIN32
// clang-format off
#include <windows.h>
#include <aclapi.h>
// clang-format on
// Here DeleteFile is a SyncAction, not the Win32 function's macro.
#undef DeleteFile
#else
#include <sys/stat.h>
#endif

struct SyncCoordinatorTestAccess {
    static void setLargeTreeLimits(SyncCoordinator &coordinator, quint64 items,
                                   quint64 bytes) {
        coordinator.largeTreeItems_ = items;
        coordinator.largeTreeBytes_ = bytes;
    }
};

namespace {

using namespace std::chrono_literals;
using openscp::testsupport::waitUntil;

template <typename Client, typename... Args>
std::unique_ptr<openscp::RemoteClient> connectedClient(const std::string &host,
                                                       Args &&...args) {
    auto client = std::make_unique<Client>(std::forward<Args>(args)...);
    openscp::SessionOptions options;
    options.host = host;
    options.username = "tester";
    std::string error;
    if (!client->connect(options, error))
        return nullptr;
    return client;
}

class ChecksumMockClient final : public openscp::MockSftpClient {
    public:
    openscp::ProtocolCapabilities capabilities() const override {
        auto result = openscp::MockSftpClient::capabilities();
        result.can_checksum = true;
        return result;
    }

    bool checksum(const std::string &remotePath, const std::string &algorithm,
                  std::vector<std::uint8_t> &digest, std::string &error,
                  std::function<void(std::size_t, std::size_t)> progress,
                  std::function<bool()> shouldCancel) override {
        if (algorithm != "SHA-256") {
            error = "Unsupported checksum algorithm";
            setLastOperationError(openscp::RemoteErrorKind::Unsupported, error);
            return false;
        }
        if (remotePath == "/slow.bin") {
            for (std::size_t done = 0; done < 250; ++done) {
                if (shouldCancel && shouldCancel()) {
                    error = "Checksum calculation canceled";
                    setLastOperationError(openscp::RemoteErrorKind::Canceled,
                                          error);
                    return false;
                }
                if (progress)
                    progress(done + 1, 250);
                std::this_thread::sleep_for(2ms);
            }
        }
        const QByteArray content = remotePath == "/different.txt"
                                       ? QByteArrayLiteral("remote")
                                       : QByteArrayLiteral("same");
        const QByteArray calculated =
            QCryptographicHash::hash(content, QCryptographicHash::Sha256);
        digest.assign(calculated.cbegin(), calculated.cend());
        error.clear();
        return true;
    }
};

class IncompleteListingMockClient final : public openscp::MockSftpClient {
    public:
    bool list(const std::string &remotePath,
              std::vector<openscp::FileInfo> &entries,
              std::string &error) override {
        if (remotePath == "/home") {
            error = "Could not read /home";
            return false;
        }
        return MockSftpClient::list(remotePath, entries, error);
    }
};

class SpecialEntryMockClient final : public openscp::MockSftpClient {
    public:
    bool list(const std::string &remotePath,
              std::vector<openscp::FileInfo> &entries,
              std::string &error) override {
        if (!MockSftpClient::list(remotePath, entries, error))
            return false;
        if (remotePath == "/") {
            openscp::FileInfo pipe;
            pipe.name = "remote-pipe";
            pipe.mode = 0010644;
            entries.push_back(pipe);
        }
        return true;
    }
};

OPENSCP_TEST(testCoordinatorIsDestroyedAfterItsControllers, test) {
    // MainWindow creates the remote controller first, and Qt destroys
    // children in creation order, so the coordinator outlives it.
    QTemporaryDir localRoot;
    auto owner = std::make_unique<QObject>();
    auto *remote = new RemoteOperationController(owner.get());
    auto *transfers = new TransferManager(owner.get());
    auto *coordinator = new SyncCoordinator(remote, transfers, owner.get());
    test.check(localRoot.isValid() &&
                   remote->installSession(
                       connectedClient<openscp::MockSftpClient>("sync.test")) >
                       0,
               "the remote controller should accept a connected session");
    bool ready = false;
    QObject::connect(coordinator, &SyncCoordinator::preparationReady,
                     coordinator,
                     [&ready](const SyncPreparationResult &) { ready = true; });
    coordinator->start(localRoot.path(), QStringLiteral("/"));
    test.check(waitUntil([&] { return ready; }),
               "the comparison should complete");
    // Under AddressSanitizer a coordinator that still reaches the destroyed
    // controller fails here.
    owner.reset();
}

OPENSCP_TEST(testScanMarksSpecialEntriesAndRefusesUnsafeNames, test) {
#ifdef _WIN32
    test.skip("Windows has no FIFOs, and its names cannot hold a backslash or "
              "a line break.");
#else
    QTemporaryDir localRoot;
    test.check(
        localRoot.isValid() &&
            ::mkfifo(
                QFile::encodeName(localRoot.filePath("local-pipe")).constData(),
                0600) == 0,
        "a local FIFO should be created");
    for (const QString &name : {QStringLiteral("back\\slash.txt"),
                                QStringLiteral("line\nbreak.txt")}) {
        QFile file(localRoot.filePath(name));
        test.check(file.open(QIODevice::WriteOnly),
                   "a file with an unsafe name should be created");
    }

    RemoteOperationController remote;
    test.check(remote.installSession(
                   connectedClient<SpecialEntryMockClient>("sync.test")) > 0,
               "remote controller should accept a connected session");
    TransferManager transfers;
    SyncCoordinator coordinator(&remote, &transfers);
    bool ready = false;
    SyncPreparationResult prepared;
    QObject::connect(&coordinator, &SyncCoordinator::preparationReady,
                     &coordinator, [&](const SyncPreparationResult &result) {
                         prepared = result;
                         ready = true;
                     });
    coordinator.start(localRoot.path(), QStringLiteral("/"));
    test.check(waitUntil([&] { return ready; }),
               "a scan with special entries should complete");

    const auto typeOf =
        [](const QVector<SyncSnapshotEntry> &entries,
           const QString &path) -> std::optional<SyncEntryType> {
        for (const SyncSnapshotEntry &entry : entries) {
            if (entry.relativePath == path)
                return entry.type;
        }
        return std::nullopt;
    };
    test.check(
        typeOf(prepared.localSnapshot, QStringLiteral("local-pipe")) ==
                SyncEntryType::Other &&
            typeOf(prepared.remoteSnapshot, QStringLiteral("remote-pipe")) ==
                SyncEntryType::Other,
        "FIFOs should be scanned as entries that cannot be copied");
    test.check(
        !typeOf(prepared.localSnapshot, QStringLiteral("back\\slash.txt")) &&
            !typeOf(prepared.localSnapshot,
                    QStringLiteral("line\nbreak.txt")) &&
            prepared.invalidNames >= 2,
        "names that uploads refuse should stay out of the scan");

    const auto items = SyncComparisonEngine::compare(
        prepared.localSnapshot, prepared.remoteSnapshot, {}, prepared.coverage);
    test.check(std::none_of(items.cbegin(), items.cend(),
                            [](const SyncComparisonItem &item) {
                                return item.selected &&
                                       item.relativePath.endsWith(
                                           QStringLiteral("-pipe"));
                            }),
               "special entries should never be selected for copying");
#endif
}

#ifdef Q_OS_WIN
OPENSCP_TEST(testWindowsShortcutsSyncAsFiles, test) {
    // Qt describes the entry a shortcut leads to; synchronization compares
    // and hashes the shortcut itself, the file that uploads copy.
    QTemporaryDir localRoot;
    QTemporaryDir outside;
    QFile big(outside.filePath(QStringLiteral("big.txt")));
    test.check(localRoot.isValid() && outside.isValid() &&
                   big.open(QIODevice::WriteOnly) &&
                   big.write(QByteArray(5000, 'x')) == 5000,
               "the shortcut targets should be created");
    big.close();
    const QStringList names{QStringLiteral("folder.lnk"),
                            QStringLiteral("file.lnk")};
    test.check(QFile::link(outside.path(), localRoot.filePath(names[0])) &&
                   QFile::link(big.fileName(), localRoot.filePath(names[1])),
               "the shortcuts should be created");

    RemoteOperationController remote;
    test.check(remote.installSession(
                   connectedClient<ChecksumMockClient>("sync.test")) > 0,
               "remote controller should accept a connected session");
    TransferManager transfers;
    SyncCoordinator coordinator(&remote, &transfers);
    bool ready = false;
    SyncPreparationResult prepared;
    QObject::connect(&coordinator, &SyncCoordinator::preparationReady,
                     &coordinator, [&](const SyncPreparationResult &result) {
                         prepared = result;
                         ready = true;
                     });
    coordinator.start(localRoot.path(), QStringLiteral("/"));
    test.check(waitUntil([&] { return ready; }),
               "a scan with shortcuts should complete");

    bool checked = false;
    SyncChecksumResult checksums;
    QObject::connect(&coordinator, &SyncCoordinator::checksumReady,
                     &coordinator, [&](const SyncChecksumResult &value) {
                         checksums = value;
                         checked = true;
                     });
    QObject::connect(&coordinator, &SyncCoordinator::checksumFailed,
                     &coordinator, [&](const QString &) { checked = true; });
    coordinator.startChecksums(localRoot.path(), QStringLiteral("/"), names);
    test.check(waitUntil([&] { return checked; }),
               "shortcut checksums should complete");

    for (const QString &name : names) {
        QFile shortcut(localRoot.filePath(name));
        const QByteArray contents = shortcut.open(QIODevice::ReadOnly)
                                        ? shortcut.readAll()
                                        : QByteArray();
        const auto entry = std::find_if(prepared.localSnapshot.cbegin(),
                                        prepared.localSnapshot.cend(),
                                        [&](const SyncSnapshotEntry &value) {
                                            return value.relativePath == name;
                                        });
        test.check(entry != prepared.localSnapshot.cend() &&
                       entry->type == SyncEntryType::File &&
                       entry->size == quint64(contents.size()),
                   "a shortcut should be scanned as a file of its own size: " +
                       name.toStdString());
        test.check(
            checksums.localChecksums.value(name) ==
                QCryptographicHash::hash(contents, QCryptographicHash::Sha256),
            "a shortcut's checksum should be its own: " + name.toStdString());
    }
}
#endif

OPENSCP_TEST(testScanRecordsTheSizesAndTimesQFileInfoReads, test) {
    // Windows scans read them from the listing instead, which must agree
    // with QFileInfo to the millisecond, or files would compare as changed.
    QTemporaryDir localRoot;
    const QString folder = localRoot.filePath(QStringLiteral("folder"));
    const QString path = folder + QStringLiteral("/file.txt");
    const QDateTime modified =
        QDateTime::fromMSecsSinceEpoch(1'614'834'367'891, QTimeZone::utc());
    QFile file(path);
    test.check(QDir().mkpath(folder) && file.open(QIODevice::WriteOnly) &&
                   file.write("12345") == 5,
               "the scanned file should be written");
    file.close();
    test.check(
        file.open(QIODevice::WriteOnly | QIODevice::Append) &&
            file.setFileTime(modified, QFileDevice::FileModificationTime),
        "the scanned file should get its time");
    file.close();

    RemoteOperationController remote;
    test.check(remote.installSession(
                   connectedClient<openscp::MockSftpClient>("sync.test")) > 0,
               "remote controller should accept a connected session");
    TransferManager transfers;
    SyncCoordinator coordinator(&remote, &transfers);
    bool ready = false;
    SyncPreparationResult prepared;
    QObject::connect(&coordinator, &SyncCoordinator::preparationReady,
                     &coordinator, [&](const SyncPreparationResult &result) {
                         prepared = result;
                         ready = true;
                     });
    coordinator.start(localRoot.path(), QStringLiteral("/"));
    test.check(waitUntil([&] { return ready; }), "the scan should complete");

    const auto entry = std::find_if(
        prepared.localSnapshot.cbegin(), prepared.localSnapshot.cend(),
        [](const SyncSnapshotEntry &value) {
            return value.relativePath == QStringLiteral("folder/file.txt");
        });
    const qint64 expected = QFileInfo(path).lastModified().toMSecsSinceEpoch();
    test.check(entry != prepared.localSnapshot.cend() &&
                   entry->type == SyncEntryType::File && entry->size == 5u &&
                   entry->modifiedMs == expected &&
                   expected == modified.toMSecsSinceEpoch(),
               "the scan should record the file's size and time as QFileInfo "
               "reads them");
}

OPENSCP_TEST(testAsynchronousSnapshots, test) {
    QTemporaryDir localRoot;
    test.check(localRoot.isValid(), "local snapshot fixture should initialize");
#ifndef _WIN32
    QTemporaryDir linkedRootParent;
    test.check(linkedRootParent.isValid(),
               "linked local snapshot fixture should initialize");
    const QString linkedRoot = linkedRootParent.filePath("linked-root");
    test.check(QFile::link(localRoot.path(), linkedRoot),
               "a linked local root should be created");
#endif
    QFile localFile(localRoot.filePath("readme.txt"));
    test.check(localFile.open(QIODevice::WriteOnly),
               "local snapshot file should be writable");
    localFile.write("local");
    localFile.close();
    test.check(QDir().mkpath(localRoot.filePath("empty")),
               "local empty directory should be created");

    RemoteOperationController remote;
    test.check(remote.installSession(
                   connectedClient<openscp::MockSftpClient>("sync.test")) > 0,
               "remote controller should accept a connected session");
    TransferManager transfers;
    SyncCoordinator coordinator(&remote, &transfers);

    bool ready = false;
    bool failed = false;
    bool eventLoopAdvanced = false;
    SyncPreparationResult prepared;
    QObject::connect(&coordinator, &SyncCoordinator::preparationReady,
                     &coordinator, [&](const SyncPreparationResult &result) {
                         prepared = result;
                         ready = true;
                     });
    QObject::connect(&coordinator, &SyncCoordinator::preparationFailed,
                     &coordinator, [&](const QString &) { failed = true; });
    QTimer::singleShot(0, &coordinator, [&] { eventLoopAdvanced = true; });

#ifndef _WIN32
    coordinator.start(linkedRoot, QStringLiteral("/"));
#else
    coordinator.start(localRoot.path(), QStringLiteral("/"));
#endif
    test.check(waitUntil([&] { return ready || failed; }),
               "local and remote snapshots should complete asynchronously");
    test.check(ready && !failed,
               "partial child-list errors should still produce a preview");
    test.check(eventLoopAdvanced,
               "snapshot preparation must not block the Qt event loop");
#ifndef _WIN32
    test.check(
        prepared.localRoot == QFileInfo(localRoot.path()).canonicalFilePath(),
        "linked local roots should use their physical path for queued work");
#endif

    const auto contains = [](const QVector<SyncSnapshotEntry> &entries,
                             const QString &path) {
        return std::any_of(entries.cbegin(), entries.cend(),
                           [&](const SyncSnapshotEntry &entry) {
                               return entry.relativePath == path;
                           });
    };
    test.check(contains(prepared.localSnapshot, QStringLiteral("readme.txt")) &&
                   contains(prepared.localSnapshot, QStringLiteral("empty")),
               "local files and empty folders should enter the snapshot");
    test.check(contains(prepared.remoteSnapshot, QStringLiteral("readme.txt")),
               "remote controller batches should enter the snapshot");
}

OPENSCP_TEST(testMirrorDoesNotDeleteSkippedSourceLink, test) {
    QTemporaryDir localRoot;
    QTemporaryDir outside;
    test.check(localRoot.isValid() && outside.isValid(),
               "mirror scan fixtures should initialize");
    test.check(openscp::testsupport::createLocalLink(
                   outside.path(), localRoot.filePath("readme.txt")),
               "source link should be created");

    RemoteOperationController remote;
    test.check(remote.installSession(
                   connectedClient<openscp::MockSftpClient>("sync.test")) > 0,
               "remote controller should accept a connected session");
    TransferManager transfers;
    SyncCoordinator coordinator(&remote, &transfers);
    bool ready = false;
    SyncPreparationResult prepared;
    QObject::connect(&coordinator, &SyncCoordinator::preparationReady,
                     &coordinator, [&](const SyncPreparationResult &result) {
                         prepared = result;
                         ready = true;
                     });
    coordinator.start(localRoot.path(), QStringLiteral("/"));
    test.check(waitUntil([&] { return ready; }),
               "scan with a skipped source link should complete");
    test.check(prepared.skippedSymlinks > 0,
               "scan should report the skipped source link");
    test.check(prepared.coverage.localUnscannedPaths.contains(
                   QStringLiteral("readme.txt")),
               "scan should retain the exact unverified source path");

    SyncComparisonOptions options;
    options.mirror = true;
    const auto items = SyncComparisonEngine::compare(
        prepared.localSnapshot, prepared.remoteSnapshot, options,
        prepared.coverage);
    const auto deleted = std::find_if(
        items.cbegin(), items.cend(), [](const SyncComparisonItem &item) {
            return item.relativePath == QStringLiteral("readme.txt") &&
                   item.action == SyncAction::DeleteFile;
        });
    test.check(deleted == items.cend(),
               "mirror must not infer absence from a skipped source link");
    const auto healthySibling = std::find_if(
        items.cbegin(), items.cend(), [](const SyncComparisonItem &item) {
            return item.relativePath == QStringLiteral("home") &&
                   item.action == SyncAction::DeleteDirectory;
        });
    test.check(healthySibling != items.cend(),
               "mirror can still propose a deletion outside the skipped path");

    const auto staleItems = SyncComparisonEngine::compare(
        prepared.localSnapshot, prepared.remoteSnapshot, options);
    const auto plan = SyncComparisonEngine::makeExecutionPlan(
        staleItems, options, prepared.coverage);
    test.check(std::none_of(plan.deletes.cbegin(), plan.deletes.cend(),
                            [](const SyncDeleteOperation &deletion) {
                                return deletion.relativePath ==
                                       QStringLiteral("readme.txt");
                            }),
               "the execution plan must also reject a stale unsafe deletion");
}

OPENSCP_TEST(testMirrorDoesNotDeleteUnlistedRemoteSubtree, test) {
    QTemporaryDir localRoot;
    test.check(localRoot.isValid(), "local mirror fixture should initialize");
    test.check(QDir().mkpath(localRoot.filePath("home")),
               "local destination folder should be created");
    for (const QString &path :
         {QStringLiteral("home/notes.md"), QStringLiteral("unrelated.txt")}) {
        QFile file(localRoot.filePath(path));
        test.check(file.open(QIODevice::WriteOnly),
                   "local destination file should be created");
    }

    RemoteOperationController remote;
    test.check(remote.installSession(
                   connectedClient<IncompleteListingMockClient>("sync.test")) >
                   0,
               "remote controller should accept a connected session");
    TransferManager transfers;
    SyncCoordinator coordinator(&remote, &transfers);
    bool ready = false;
    SyncPreparationResult prepared;
    QObject::connect(&coordinator, &SyncCoordinator::preparationReady,
                     &coordinator, [&](const SyncPreparationResult &result) {
                         prepared = result;
                         ready = true;
                     });
    coordinator.start(localRoot.path(), QStringLiteral("/"));
    test.check(waitUntil([&] { return ready; }),
               "a partially listed remote tree should produce a preview");
    test.check(
        prepared.coverage.remoteUnscannedPaths.contains(QStringLiteral("home")),
        "the failed remote subtree should be recorded");

    SyncComparisonOptions options;
    options.direction = SyncDirection::RemoteToLocal;
    options.mirror = true;
    const auto items = SyncComparisonEngine::compare(
        prepared.localSnapshot, prepared.remoteSnapshot, options,
        prepared.coverage);
    const auto actionFor = [&items](const QString &path) {
        const auto found =
            std::find_if(items.cbegin(), items.cend(),
                         [&path](const SyncComparisonItem &item) {
                             return item.relativePath == path;
                         });
        return found == items.cend() ? SyncAction::Unknown : found->action;
    };
    test.check(
        actionFor(QStringLiteral("home/notes.md")) == SyncAction::Keep,
        "mirror must preserve entries under an unreadable source folder");
    test.check(actionFor(QStringLiteral("unrelated.txt")) ==
                   SyncAction::DeleteFile,
               "mirror can still delete verified destination extras");
}

#ifdef _WIN32
// Denies the current user listing a folder, as icacls /deny does, since
// Windows has no permission bits for it, until restore() or destruction.
class FolderListingDenied {
    public:
    explicit FolderListingDenied(const QString &folder)
        : path_(QDir::toNativeSeparators(folder).toStdWString()) {
        PACL current = nullptr;
        if (::GetNamedSecurityInfoW(path_.c_str(), SE_FILE_OBJECT,
                                    DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                    &current, nullptr,
                                    &original_) != ERROR_SUCCESS)
            return;
        HANDLE token = nullptr;
        if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token))
            return;
        DWORD size = 0;
        (void)::GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        std::vector<BYTE> user(size);
        const bool known = ::GetTokenInformation(token, TokenUser, user.data(),
                                                 size, &size) != 0;
        ::CloseHandle(token);
        if (!known)
            return;
        EXPLICIT_ACCESSW deny{};
        deny.grfAccessPermissions = FILE_LIST_DIRECTORY;
        deny.grfAccessMode = DENY_ACCESS;
        deny.grfInheritance = NO_INHERITANCE;
        deny.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        deny.Trustee.TrusteeType = TRUSTEE_IS_USER;
        deny.Trustee.ptstrName = static_cast<LPWSTR>(
            reinterpret_cast<TOKEN_USER *>(user.data())->User.Sid);
        PACL denying = nullptr;
        if (::SetEntriesInAclW(1, &deny, current, &denying) != ERROR_SUCCESS)
            return;
        originalDacl_ = current;
        applied_ = ::SetNamedSecurityInfoW(
                       path_.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                       nullptr, nullptr, denying, nullptr) == ERROR_SUCCESS;
        ::LocalFree(denying);
    }

    ~FolderListingDenied() {
        restore();
        if (original_)
            ::LocalFree(original_);
    }

    FolderListingDenied(const FolderListingDenied &) = delete;
    FolderListingDenied &operator=(const FolderListingDenied &) = delete;

    bool applied() const { return applied_; }

    void restore() {
        if (applied_) {
            (void)::SetNamedSecurityInfoW(path_.data(), SE_FILE_OBJECT,
                                          DACL_SECURITY_INFORMATION, nullptr,
                                          nullptr, originalDacl_, nullptr);
        }
        applied_ = false;
    }

    private:
    std::wstring path_;
    PSECURITY_DESCRIPTOR original_ = nullptr;
    PACL originalDacl_ = nullptr;
    bool applied_ = false;
};
#endif

OPENSCP_TEST(testMirrorDoesNotDeleteUnreadableLocalSubtree, test) {
    QTemporaryDir localRoot;
    test.check(localRoot.isValid(), "local mirror fixture should initialize");
    const QString locked = localRoot.filePath("home");
    test.check(QDir().mkpath(locked), "local source folder should be created");
#ifdef _WIN32
    FolderListingDenied denied(locked);
    test.check(denied.applied(),
               "local source folder should become unreadable");
#else
    test.check(QFile::setPermissions(locked, QFileDevice::Permissions{}),
               "local source folder should become unreadable");
#endif

    RemoteOperationController remote;
    test.check(remote.installSession(
                   connectedClient<openscp::MockSftpClient>("sync.test")) > 0,
               "remote controller should accept a connected session");
    TransferManager transfers;
    SyncCoordinator coordinator(&remote, &transfers);
    bool ready = false;
    SyncPreparationResult prepared;
    QObject::connect(&coordinator, &SyncCoordinator::preparationReady,
                     &coordinator, [&](const SyncPreparationResult &result) {
                         prepared = result;
                         ready = true;
                     });
    coordinator.start(localRoot.path(), QStringLiteral("/"));
    const bool completed = waitUntil([&] { return ready; });
#ifdef _WIN32
    denied.restore();
#else
    QFile::setPermissions(locked, QFileDevice::ReadOwner |
                                      QFileDevice::WriteOwner |
                                      QFileDevice::ExeOwner);
#endif
    test.check(completed, "scan with an unreadable local folder should finish");
    test.check(
        prepared.coverage.localUnscannedPaths.contains(QStringLiteral("home")),
        "unreadable local subtree should be recorded");

    SyncComparisonOptions options;
    options.mirror = true;
    const auto items = SyncComparisonEngine::compare(
        prepared.localSnapshot, prepared.remoteSnapshot, options,
        prepared.coverage);
    const auto actionFor = [&items](const QString &path) {
        const auto found =
            std::find_if(items.cbegin(), items.cend(),
                         [&path](const SyncComparisonItem &item) {
                             return item.relativePath == path;
                         });
        return found == items.cend() ? SyncAction::Unknown : found->action;
    };
    test.check(actionFor(QStringLiteral("home/demo")) == SyncAction::Keep,
               "mirror must preserve remote children of an unreadable source");
    test.check(actionFor(QStringLiteral("readme.txt")) ==
                   SyncAction::DeleteFile,
               "unrelated verified destination extras can still be deleted");
}

// Local files, and the mock server's tree with extra remote folders.
struct LargeTreeFixture {
    QTemporaryDir localRoot;
    RemoteOperationController remote;
    TransferManager transfers;

    bool prepare(int localFiles, int remoteFolders) {
        for (int index = 0; index < localFiles; ++index) {
            QFile file(QDir(localRoot.path())
                           .filePath(QStringLiteral("file-%1.txt").arg(index)));
            if (!file.open(QIODevice::WriteOnly) || file.write("x") != 1)
                return false;
        }
        auto client = connectedClient<openscp::MockSftpClient>("sync.test");
        std::string error;
        if (!client || (remoteFolders > 0 && !client->mkdir("/bulk", error)))
            return false;
        for (int index = 0; index < remoteFolders; ++index) {
            if (!client->mkdir("/bulk/folder-" + std::to_string(index), error))
                return false;
        }
        return localRoot.isValid() &&
               remote.installSession(std::move(client)) > 0;
    }
};

OPENSCP_TEST(testLargeTreeScansPauseUntilConfirmed, test) {
    struct Case {
        int localFiles;
        int remoteFolders;
        quint64 itemLimit;
        const char *label;
    };
    for (const Case &scenario :
         {Case{6, 0, 3, "local"}, Case{0, 600, 300, "remote"}}) {
        const std::string label = std::string(" (") + scenario.label + ")";
        LargeTreeFixture fixture;
        test.check(fixture.prepare(scenario.localFiles, scenario.remoteFolders),
                   "the comparison fixture should be ready" + label);

        std::optional<SyncPreparationResult> unlimited;
        {
            SyncCoordinator coordinator(&fixture.remote, &fixture.transfers);
            QObject::connect(&coordinator, &SyncCoordinator::preparationReady,
                             &coordinator,
                             [&](const SyncPreparationResult &result) {
                                 unlimited = result;
                             });
            coordinator.start(fixture.localRoot.path(), QStringLiteral("/"));
            test.check(waitUntil([&] { return unlimited.has_value(); }),
                       "a comparison under the threshold should not ask" +
                           label);
        }
        if (!unlimited)
            continue;

        SyncCoordinator coordinator(&fixture.remote, &fixture.transfers);
        SyncCoordinatorTestAccess::setLargeTreeLimits(
            coordinator, scenario.itemLimit, quint64(1) << 40);
        int remoteScans = 0;
        int questions = 0;
        std::optional<SyncPreparationResult> prepared;
        QObject::connect(
            &fixture.remote, &RemoteOperationController::jobStarted,
            &coordinator,
            [&](const RemoteOperationController::JobKey &) { ++remoteScans; });
        QObject::connect(&coordinator,
                         &SyncCoordinator::largeTreeConfirmationRequired,
                         &coordinator, [&](quint64, quint64) { ++questions; });
        QObject::connect(
            &coordinator, &SyncCoordinator::preparationReady, &coordinator,
            [&](const SyncPreparationResult &result) { prepared = result; });
        coordinator.start(fixture.localRoot.path(), QStringLiteral("/"));
        test.check(waitUntil([&] { return questions == 1; }),
                   "crossing the threshold should ask once" + label);
        test.check(!waitUntil([&] { return prepared.has_value(); },
                              std::chrono::milliseconds(300)),
                   "the comparison should wait for the answer" + label);

        coordinator.continueLargeTree();
        test.check(waitUntil([&] { return prepared.has_value(); }),
                   "accepting should finish the comparison" + label);
        if (!prepared)
            continue;
        test.check(questions == 1 && remoteScans == 1,
                   "accepting should resume the scans instead of repeating "
                   "them" +
                       label);
        test.check(
            prepared->localSnapshot.size() == unlimited->localSnapshot.size() &&
                prepared->remoteSnapshot.size() ==
                    unlimited->remoteSnapshot.size() &&
                prepared->coverage.remoteUnscannedPaths.isEmpty(),
            "a resumed comparison should cover both trees completely" + label);
    }
}

OPENSCP_TEST(testDecliningALargeTreeStopsTheComparison, test) {
    LargeTreeFixture fixture;
    test.check(fixture.prepare(6, 0), "the comparison fixture should be ready");
    SyncCoordinator coordinator(&fixture.remote, &fixture.transfers);
    SyncCoordinatorTestAccess::setLargeTreeLimits(coordinator, 3,
                                                  quint64(1) << 40);
    bool canceled = false;
    bool ready = false;
    QObject::connect(
        &coordinator, &SyncCoordinator::largeTreeConfirmationRequired,
        &coordinator, [&](quint64, quint64) { coordinator.cancel(); });
    QObject::connect(&coordinator, &SyncCoordinator::preparationCanceled,
                     &coordinator, [&] { canceled = true; });
    QObject::connect(&coordinator, &SyncCoordinator::preparationReady,
                     &coordinator,
                     [&](const SyncPreparationResult &) { ready = true; });
    coordinator.start(fixture.localRoot.path(), QStringLiteral("/"));
    test.check(waitUntil([&] { return canceled; }),
               "declining should cancel the comparison");
    test.check(
        !waitUntil([&] { return ready; }, std::chrono::milliseconds(300)) &&
            !coordinator.isPreparing(),
        "a declined comparison should not finish");
}

OPENSCP_TEST(testPersistentExecutionPlan, test) {
    RemoteOperationController remote;
    TransferManager transfers;
    transfers.setSessionIdentity(QStringLiteral("site-id"));
    SyncCoordinator coordinator(&remote, &transfers);

    SyncExecutionPlan plan;
    plan.direction = SyncDirection::LocalToRemote;
    plan.directoriesToCreate = {QStringLiteral("new/folder")};
    plan.copies = {{QStringLiteral("new/folder/file.txt"), 12}};
    plan.deletes = {
        {QStringLiteral("old/file.txt"), SyncEntryType::File},
        {QStringLiteral("old"), SyncEntryType::Directory},
    };

    int addedNotifications = 0;
    QObject::connect(&transfers, &TransferManager::tasksAdded, &transfers,
                     [&](const QVector<quint64> &) { ++addedNotifications; });
    qsizetype taskCount = 0;
    const quint64 batchId = coordinator.enqueuePlan(
        plan, QStringLiteral("/local/root"), QStringLiteral("/remote/root"),
        QStringLiteral("site-id"), &taskCount);
    const auto tasks = transfers.tasksSnapshot();
    test.check(batchId != 0 && taskCount == 4 && tasks.size() == 4,
               "the whole sync plan should become one persistent batch");
    test.check(addedNotifications == 1,
               "the sync plan should be queued in one addition");
    if (tasks.size() != 4)
        return;

    test.check(tasks[0].type == TransferTask::Type::CreateRemoteDirectory &&
                   tasks[1].type == TransferTask::Type::Upload &&
                   tasks[2].type == TransferTask::Type::DeleteRemoteFile &&
                   tasks[3].type == TransferTask::Type::DeleteRemoteDirectory,
               "sync actions should map to queue task types");
    test.check(tasks[0].dst == QStringLiteral("/remote/root/new/folder") &&
                   tasks[1].src ==
                       QStringLiteral("/local/root/new/folder/file.txt"),
               "execution paths should remain confined to their roots");
    test.check(tasks[0].dependsOnTaskId == 0 && !tasks[0].waitsForBatch &&
                   tasks[1].dependsOnTaskId == tasks[0].taskId &&
                   !tasks[1].waitsForBatch,
               "copies should wait only for the directory that contains them");
    test.check(tasks[2].dependsOnTaskId == 0 && tasks[2].waitsForBatch &&
                   tasks[3].dependsOnTaskId == tasks[2].taskId &&
                   tasks[3].waitsForBatch,
               "deletes should run in order after the rest of the batch");
    test.check(std::all_of(tasks.cbegin(), tasks.cend(),
                           [batchId](const TransferTask &task) {
                               return task.batchId == batchId &&
                                      task.sessionKey ==
                                          QStringLiteral("site-id");
                           }),
               "every sync task should retain batch and session identity");
}

OPENSCP_TEST(testExecutionPlanRunsIndependentWorkInParallel, test) {
    RemoteOperationController remote;
    TransferManager transfers;
    transfers.setSessionIdentity(QStringLiteral("site-id"));
    SyncCoordinator coordinator(&remote, &transfers);

    SyncExecutionPlan plan;
    plan.direction = SyncDirection::RemoteToLocal;
    plan.directoriesToCreate = {QStringLiteral("a"), QStringLiteral("c"),
                                QStringLiteral("a/b")};
    plan.copies = {{QStringLiteral("a/b/x.txt"), 1},
                   {QStringLiteral("c/y.txt"), 1},
                   {QStringLiteral("z.txt"), 1}};

    qsizetype taskCount = 0;
    coordinator.enqueuePlan(plan, QStringLiteral("/local/root"),
                            QStringLiteral("/remote/root"),
                            QStringLiteral("site-id"), &taskCount);
    const auto tasks = transfers.tasksSnapshot();
    test.check(taskCount == 6 && tasks.size() == 6,
               "every directory and copy should become a task");
    if (tasks.size() != 6)
        return;

    const auto taskFor = [&](const QString &destination) {
        const auto found = std::find_if(
            tasks.cbegin(), tasks.cend(),
            [&](const TransferTask &task) { return task.dst == destination; });
        return found == tasks.cend() ? TransferTask{} : *found;
    };
    const TransferTask a = taskFor(QStringLiteral("/local/root/a"));
    const TransferTask ab = taskFor(QStringLiteral("/local/root/a/b"));
    const TransferTask c = taskFor(QStringLiteral("/local/root/c"));
    const TransferTask x = taskFor(QStringLiteral("/local/root/a/b/x.txt"));
    const TransferTask y = taskFor(QStringLiteral("/local/root/c/y.txt"));
    const TransferTask z = taskFor(QStringLiteral("/local/root/z.txt"));
    test.check(a.type == TransferTask::Type::CreateLocalDirectory &&
                   x.type == TransferTask::Type::Download,
               "remote-to-local plans should create local work");
    test.check(a.dependsOnTaskId == 0 && c.dependsOnTaskId == 0 &&
                   ab.dependsOnTaskId == a.taskId,
               "directories should wait only for their parent directory");
    test.check(x.dependsOnTaskId == ab.taskId &&
                   y.dependsOnTaskId == c.taskId && z.dependsOnTaskId == 0,
               "copies should wait only for the directory that contains them");
}

OPENSCP_TEST(testOnDemandChecksums, test) {
    QTemporaryDir localRoot;
    test.check(localRoot.isValid(), "checksum fixture should initialize");
    const auto writeFile = [&](const QString &name, const QByteArray &content) {
        QFile file(localRoot.filePath(name));
        if (!file.open(QIODevice::WriteOnly))
            return false;
        return file.write(content) == content.size();
    };
    test.check(
        writeFile(QStringLiteral("same.txt"), QByteArrayLiteral("same")) &&
            writeFile(QStringLiteral("different.txt"),
                      QByteArrayLiteral("local")),
        "checksum fixture files should be writable");

    RemoteOperationController remote;
    remote.installSession(connectedClient<ChecksumMockClient>("checksum.test"));
    TransferManager transfers;
    SyncCoordinator coordinator(&remote, &transfers);
    bool ready = false;
    bool failed = false;
    bool eventLoopAdvanced = false;
    SyncChecksumResult result;
    QObject::connect(&coordinator, &SyncCoordinator::checksumReady,
                     &coordinator, [&](const SyncChecksumResult &value) {
                         result = value;
                         ready = true;
                     });
    QObject::connect(&coordinator, &SyncCoordinator::checksumFailed,
                     &coordinator, [&](const QString &) { failed = true; });
    QTimer::singleShot(0, &coordinator, [&] { eventLoopAdvanced = true; });

    coordinator.startChecksums(
        localRoot.path(), QStringLiteral("/"),
        {QStringLiteral("same.txt"), QStringLiteral("different.txt")});
    test.check(waitUntil([&] { return ready || failed; }),
               "local and remote checksums should complete asynchronously");
    test.check(ready && !failed,
               "supported SFTP checksums should produce a result");
    test.check(eventLoopAdvanced,
               "local checksum calculation must not block the event loop");
    test.check(result.localChecksums.value(QStringLiteral("same.txt")) ==
                   result.remoteChecksums.value(QStringLiteral("same.txt")),
               "identical files should produce identical SHA-256 values");
    test.check(
        result.localChecksums.value(QStringLiteral("different.txt")) !=
            result.remoteChecksums.value(QStringLiteral("different.txt")),
        "different files should produce different SHA-256 values");
}

OPENSCP_TEST(testChecksumCancellation, test) {
    QTemporaryDir localRoot;
    QFile slowFile(localRoot.filePath(QStringLiteral("slow.bin")));
    test.check(slowFile.open(QIODevice::WriteOnly),
               "checksum cancellation fixture should open");
    slowFile.write("same");
    slowFile.close();

    RemoteOperationController remote;
    remote.installSession(connectedClient<ChecksumMockClient>("checksum.test"));
    TransferManager transfers;
    SyncCoordinator coordinator(&remote, &transfers);
    bool remotePhaseStarted = false;
    bool canceled = false;
    QObject::connect(
        &coordinator, &SyncCoordinator::checksumProgressChanged, &coordinator,
        [&](qsizetype completed, qsizetype, const QString &path, quint64,
            quint64 totalBytes) {
            if (completed >= 1 && path == QStringLiteral("slow.bin") &&
                totalBytes == 250) {
                remotePhaseStarted = true;
            }
        });
    QObject::connect(&coordinator, &SyncCoordinator::checksumCanceled,
                     &coordinator, [&] { canceled = true; });

    coordinator.startChecksums(localRoot.path(), QStringLiteral("/"),
                               {QStringLiteral("slow.bin")});
    test.check(waitUntil([&] { return remotePhaseStarted; }),
               "checksum cancellation should reach the remote phase");
    coordinator.cancelChecksums();
    test.check(waitUntil([&] { return canceled; }),
               "checksum cancellation should complete cooperatively");
    test.check(!coordinator.isCalculatingChecksums(),
               "a canceled checksum run should release its state");
}

} // namespace

int main(int argc, char **argv) {
    openscp::test::TestHarness harness("sync coordinator");
    return harness.runWithApplication<QCoreApplication>(argc, argv);
}
