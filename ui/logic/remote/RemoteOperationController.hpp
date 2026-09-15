// Serialized, cancelable execution lane for remote filesystem operations.
#pragma once

#include "logic/remote/RemoteOperationTypes.hpp"

#include <QObject>

#include <memory>

class RemoteOperationController final : public QObject {
    Q_OBJECT

    public:
    using JobId = openscp::remote_operation::JobId;
    using SessionGeneration = openscp::remote_operation::SessionGeneration;
    using JobKind = openscp::remote_operation::JobKind;
    using Outcome = openscp::remote_operation::Outcome;
    using DeleteKind = openscp::remote_operation::DeleteKind;
    using JobKey = openscp::remote_operation::JobKey;
    using ResultHeader = openscp::remote_operation::ResultHeader;
    using RemoteEntry = openscp::remote_operation::RemoteEntry;
    using TraversalOptions = openscp::remote_operation::TraversalOptions;
    using ListRequest = openscp::remote_operation::ListRequest;
    using StatRequest = openscp::remote_operation::StatRequest;
    using MkdirRequest = openscp::remote_operation::MkdirRequest;
    using CreateFileRequest = openscp::remote_operation::CreateFileRequest;
    using RenameRequest = openscp::remote_operation::RenameRequest;
    using DeleteRequest = openscp::remote_operation::DeleteRequest;
    using ChmodRequest = openscp::remote_operation::ChmodRequest;
    using HealthCheckRequest = openscp::remote_operation::HealthCheckRequest;
    using SearchRequest = openscp::remote_operation::SearchRequest;
    using TraverseRequest = openscp::remote_operation::TraverseRequest;
    using ChecksumRequest = openscp::remote_operation::ChecksumRequest;
    using SessionState = openscp::remote_operation::SessionState;
    using Progress = openscp::remote_operation::Progress;
    using ListResult = openscp::remote_operation::ListResult;
    using StatResult = openscp::remote_operation::StatResult;
    using MutationResult = openscp::remote_operation::MutationResult;
    using HealthResult = openscp::remote_operation::HealthResult;
    using ChecksumResult = openscp::remote_operation::ChecksumResult;
    using EntryBatch = openscp::remote_operation::EntryBatch;
    using Completion = openscp::remote_operation::Completion;

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
