// Serialized, cancelable execution lane for remote filesystem operations.
#pragma once

#include "openscp/RemoteClient.hpp"

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QVector>
#include <QtGlobal>

#include <cstdint>
#include <memory>

class RemoteOperationController final : public QObject {
    Q_OBJECT

    public:
    using JobId = quint64;
    using SessionGeneration = quint64;

    enum class JobKind : quint8 {
        List,
        Stat,
        Mkdir,
        CreateFile,
        Rename,
        Delete,
        Chmod,
        HealthCheck,
        Search,
        Traverse,
        Checksum,
    };
    Q_ENUM(JobKind)

    enum class Outcome : quint8 {
        Succeeded,
        Failed,
        Canceled,
        Superseded,
    };
    Q_ENUM(Outcome)

    enum class DeleteKind : quint8 {
        File,
        Directory,
    };
    Q_ENUM(DeleteKind)

    struct JobKey {
        JobId id = 0;
        SessionGeneration generation = 0;
        JobKind kind = JobKind::List;
    };

    struct ResultHeader {
        JobKey job;
        Outcome outcome = Outcome::Failed;
        QString error;
        openscp::RemoteError remoteError;
        bool partial = false;
    };

    struct RemoteEntry {
        QString path;
        QString relativePath;
        openscp::FileInfo info;
        int depth = 0;
        bool isSymlink = false;
    };

    struct TraversalOptions {
        bool includeHidden = true;
        bool skipSymlinks = true;
        int maxDepth = 32;
        int batchSize = 250;
    };

    struct ListRequest {
        QString path = QStringLiteral("/");
        bool includeHidden = true;
    };

    struct StatRequest {
        QString path;
    };

    struct MkdirRequest {
        QString path;
        unsigned int mode = 0755;
        bool recursive = false;
    };

    struct CreateFileRequest {
        QString path;
        bool overwrite = false;
    };

    struct RenameRequest {
        QString from;
        QString to;
        bool overwrite = false;
    };

    struct DeleteRequest {
        QString path;
        DeleteKind kind = DeleteKind::File;
        bool recursive = false;
        TraversalOptions traversal;
        // Cleanup mode for completed directory moves: remove directories in
        // post-order, but never delete a file that remains on the server.
        bool emptyDirectoriesOnly = false;
    };

    struct ChmodRequest {
        QString path;
        std::uint32_t mode = 0644;
        bool recursive = false;
        TraversalOptions traversal;
    };

    struct HealthCheckRequest {
        QString path = QStringLiteral("/");
    };

    struct SearchRequest {
        QString rootPath = QStringLiteral("/");
        QString query;
        Qt::CaseSensitivity caseSensitivity = Qt::CaseInsensitive;
        bool includeDirectories = true;
        TraversalOptions traversal;
    };

    struct TraverseRequest {
        QString rootPath = QStringLiteral("/");
        bool includeDirectories = true;
        TraversalOptions traversal;
    };

    struct ChecksumRequest {
        QString path;
        QString algorithm = QStringLiteral("SHA-256");
    };

    struct SessionState {
        SessionGeneration generation = 0;
        bool available = false;
        openscp::Protocol protocol = openscp::Protocol::Sftp;
        openscp::ProtocolCapabilities capabilities;
    };

    struct Progress {
        JobKey job;
        QString currentPath;
        quint64 visitedEntries = 0;
        quint64 matchedEntries = 0;
        quint64 affectedEntries = 0;
        quint64 failedEntries = 0;
        quint64 processedBytes = 0;
        quint64 totalBytes = 0;
    };

    struct ListResult {
        ResultHeader result;
        QString path;
        QVector<RemoteEntry> entries;
    };

    struct StatResult {
        ResultHeader result;
        QString path;
        bool found = false;
        openscp::FileInfo info;
    };

    struct MutationResult {
        ResultHeader result;
        QString sourcePath;
        QString destinationPath;
        quint64 affectedEntries = 0;
        quint64 failedEntries = 0;
    };

    struct HealthResult {
        ResultHeader result;
        bool connected = false;
        bool roundTripSucceeded = false;
    };

    struct ChecksumResult {
        ResultHeader result;
        QString path;
        QString algorithm;
        QByteArray digest;
        quint64 processedBytes = 0;
        quint64 totalBytes = 0;
    };

    struct EntryBatch {
        JobKey job;
        QVector<RemoteEntry> entries;
        bool finalBatch = false;
    };

    struct Completion {
        ResultHeader result;
        quint64 visitedEntries = 0;
        quint64 matchedEntries = 0;
        quint64 affectedEntries = 0;
        quint64 failedEntries = 0;
        quint64 skippedSymlinks = 0;
        quint64 depthLimits = 0;
        quint64 invalidNames = 0;
        quint64 unknownSizes = 0;
    };

    explicit RemoteOperationController(QObject *parent = nullptr);
    ~RemoteOperationController() override;

    RemoteOperationController(const RemoteOperationController &) = delete;
    RemoteOperationController &
    operator=(const RemoteOperationController &) = delete;

    // The controller takes ownership of an already-connected client. Replacing
    // or clearing a session cancels all work from older generations.
    SessionGeneration
    installSession(std::unique_ptr<openscp::RemoteClient> connectedClient);
    SessionGeneration clearSession();
    SessionGeneration currentGeneration() const;
    bool hasRequestedSession() const;

    JobId submit(const ListRequest &request);
    JobId submit(const StatRequest &request);
    JobId submit(const MkdirRequest &request);
    JobId submit(const CreateFileRequest &request);
    JobId submit(const RenameRequest &request);
    JobId submit(const DeleteRequest &request);
    JobId submit(const ChmodRequest &request);
    JobId submit(const HealthCheckRequest &request);
    JobId submit(const SearchRequest &request);
    JobId submit(const TraverseRequest &request);
    JobId submit(const ChecksumRequest &request);

    // Cancellation is cooperative. For active network calls it additionally
    // invokes RemoteClient::interrupt() from the caller's thread.
    bool cancel(JobId jobId);
    bool setPaused(JobId jobId, bool paused);
    int cancelGeneration(SessionGeneration generation);
    int cancelAll();

    // Idempotent. Requests stop, interrupts active I/O, joins the lane, and
    // disconnects/destroys its client before returning.
    void shutdown();

    signals:
    void sessionChanged(const RemoteOperationController::SessionState &state);
    void jobStarted(const RemoteOperationController::JobKey &job);
    void jobProgress(const RemoteOperationController::Progress &progress);
    void listCompleted(const RemoteOperationController::ListResult &result);
    void statCompleted(const RemoteOperationController::StatResult &result);
    void
    mutationCompleted(const RemoteOperationController::MutationResult &result);
    void
    healthCheckCompleted(const RemoteOperationController::HealthResult &result);
    void
    checksumCompleted(const RemoteOperationController::ChecksumResult &result);
    void entriesBatchReady(const RemoteOperationController::EntryBatch &batch);
    void jobFinished(const RemoteOperationController::Completion &completion);

    private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

Q_DECLARE_METATYPE(RemoteOperationController::JobKey)
Q_DECLARE_METATYPE(RemoteOperationController::ResultHeader)
Q_DECLARE_METATYPE(RemoteOperationController::RemoteEntry)
Q_DECLARE_METATYPE(RemoteOperationController::SessionState)
Q_DECLARE_METATYPE(RemoteOperationController::Progress)
Q_DECLARE_METATYPE(RemoteOperationController::ListResult)
Q_DECLARE_METATYPE(RemoteOperationController::StatResult)
Q_DECLARE_METATYPE(RemoteOperationController::MutationResult)
Q_DECLARE_METATYPE(RemoteOperationController::HealthResult)
Q_DECLARE_METATYPE(RemoteOperationController::ChecksumResult)
Q_DECLARE_METATYPE(RemoteOperationController::EntryBatch)
Q_DECLARE_METATYPE(RemoteOperationController::Completion)
