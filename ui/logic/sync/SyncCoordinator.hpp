// Asynchronous snapshot preparation and persistent execution for one-way sync.
#pragma once

#include "sync/SyncTypes.hpp"

#include <QHash>
#include <QObject>
#include <QStringList>

#include <atomic>
#include <memory>
#include <thread>

class RemoteOperationController;
class TransferManager;

struct SyncPreparationResult {
    QVector<SyncSnapshotEntry> localSnapshot;
    QVector<SyncSnapshotEntry> remoteSnapshot;
    QString localRoot;
    QString remoteRoot;
    QStringList warnings;
    quint64 itemCount = 0;
    quint64 knownBytes = 0;
    quint64 skippedSymlinks = 0;
    quint64 inaccessibleFolders = 0;
    quint64 invalidNames = 0;
    quint64 unknownSizes = 0;
    quint64 depthLimits = 0;
};

Q_DECLARE_METATYPE(SyncPreparationResult)

struct SyncChecksumResult {
    QString algorithm = QStringLiteral("SHA-256");
    QHash<QString, QByteArray> localChecksums;
    QHash<QString, QByteArray> remoteChecksums;
    QStringList failures;
};

Q_DECLARE_METATYPE(SyncChecksumResult)

class SyncCoordinator final : public QObject {
    Q_OBJECT

    public:
    explicit SyncCoordinator(RemoteOperationController *remoteOperations,
                             TransferManager *transfers,
                             QObject *parent = nullptr);
    ~SyncCoordinator() override;

    void start(const QString &localRoot, const QString &remoteRoot,
               bool allowLargeTree = false);
    void continueLargeTree();
    void cancel();
    bool isPreparing() const;

    void startChecksums(const QString &localRoot, const QString &remoteRoot,
                        const QStringList &relativePaths);
    void cancelChecksums();
    bool isCalculatingChecksums() const;

    // Enqueues a complete execution plan as one persistent, ordered batch.
    // Returns its batch ID, or zero when no work was accepted.
    quint64 enqueuePlan(const SyncExecutionPlan &plan, const QString &localRoot,
                        const QString &remoteRoot, const QString &sessionKey,
                        qsizetype *taskCountOut = nullptr);

    signals:
    void progressChanged(quint64 itemCount, quint64 knownBytes,
                         const QString &currentPath);
    void preparationReady(const SyncPreparationResult &result);
    void preparationFailed(const QString &message);
    void preparationCanceled();
    void largeTreeConfirmationRequired(quint64 itemCount, quint64 knownBytes);
    void checksumProgressChanged(qsizetype completedChecksums,
                                 qsizetype totalChecksums,
                                 const QString &currentPath,
                                 quint64 processedBytes, quint64 totalBytes);
    void checksumReady(const SyncChecksumResult &result);
    void checksumFailed(const QString &message);
    void checksumCanceled();

    private:
    struct PreparationState;
    struct ChecksumState;

    void maybeFinish(const std::shared_ptr<PreparationState> &state);
    void stopState(bool emitCanceled);
    void maybeFinishChecksums(const std::shared_ptr<ChecksumState> &state);
    void stopChecksumState(bool emitCanceled);

    RemoteOperationController *remoteOperations_ = nullptr;
    TransferManager *transfers_ = nullptr;
    std::shared_ptr<PreparationState> state_;
    std::jthread localWorker_;
    quint64 generation_ = 0;
    std::shared_ptr<ChecksumState> checksumState_;
    std::jthread checksumWorker_;
    quint64 checksumGeneration_ = 0;
    QString retryLocalRoot_;
    QString retryRemoteRoot_;
};
