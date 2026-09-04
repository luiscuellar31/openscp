// Serialized, cancelable execution lane for remote filesystem operations.
#include "logic/remote/RemoteOperationController.hpp"

#include "logic/navigation/RemotePath.hpp"
#include "logic/remote/RemoteTreeWalker.hpp"

#include <QCoreApplication>
#include <QFileInfo>
#include <QMetaObject>
#include <QTemporaryFile>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <iterator>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <unordered_map>
#include <utility>
#include <variant>

namespace {

int safeBatchSize(int requested) {
    return std::clamp(requested, 1, 1000);
}

RemoteTreeWalker::Options
walkerOptionsFrom(const openscp::remote_operation::TraversalOptions &traversal,
                  RemoteTreeWalker::DepthPolicy depthPolicy =
                      RemoteTreeWalker::DepthPolicy::StopBeforeLimit) {
    RemoteTreeWalker::Options options;
    options.includeHidden = traversal.includeHidden;
    options.skipSymlinks = traversal.skipSymlinks;
    options.maxDepth = traversal.maxDepth;
    options.depthPolicy = depthPolicy;
    return options;
}

QString operationError(const std::string &raw, const QString &fallback) {
    if (!raw.empty())
        return QString::fromStdString(raw);
    return fallback;
}

} // namespace

class RemoteOperationController::Impl {
    public:
    using Request =
        std::variant<ListRequest, StatRequest, MkdirRequest, CreateFileRequest,
                     RenameRequest, DeleteRequest, ChmodRequest,
                     HealthCheckRequest, SearchRequest, TraverseRequest,
                     ChecksumRequest>;

    struct Job {
        JobKey key;
        Request request;
        std::shared_ptr<std::atomic_bool> canceled =
            std::make_shared<std::atomic_bool>(false);
        std::shared_ptr<std::atomic_bool> paused =
            std::make_shared<std::atomic_bool>(false);
    };

    struct InstallSessionCommand {
        SessionGeneration generation = 0;
        std::unique_ptr<openscp::RemoteClient> client;
    };

    struct JobRegistration {
        SessionGeneration generation = 0;
        std::shared_ptr<std::atomic_bool> canceled;
        std::shared_ptr<std::atomic_bool> paused;
    };

    using Command = std::variant<InstallSessionCommand, Job>;

    struct RunSummary {
        Outcome outcome = Outcome::Failed;
        QString error;
        openscp::RemoteError remoteError;
        bool partial = false;
        quint64 visitedEntries = 0;
        quint64 matchedEntries = 0;
        quint64 affectedEntries = 0;
        quint64 failedEntries = 0;
        quint64 skippedSymlinks = 0;
        quint64 depthLimits = 0;
        quint64 invalidNames = 0;
        quint64 unknownSizes = 0;
    };

    explicit Impl(RemoteOperationController *owner) : owner_(owner) {
        worker_ =
            std::jthread([this](std::stop_token stopToken) { run(stopToken); });
    }

    ~Impl() { shutdown(); }

    SessionGeneration
    installSession(std::unique_ptr<openscp::RemoteClient> client) {
        std::unique_lock lock(mutex_);
        if (stopping_)
            return desiredGeneration_.load(std::memory_order_relaxed);

        const SessionGeneration generation =
            desiredGeneration_.fetch_add(1, std::memory_order_relaxed) + 1;
        requestedSession_.store(client != nullptr, std::memory_order_relaxed);

        for (const auto &[id, registration] : jobs_) {
            Q_UNUSED(id);
            registration.canceled->store(true, std::memory_order_relaxed);
        }
        interruptActiveLocked();

        commands_.push_back(
            InstallSessionCommand{generation, std::move(client)});
        lock.unlock();
        wake_.notify_one();
        return generation;
    }

    JobId enqueue(Request request, JobKind kind) {
        std::unique_lock lock(mutex_);
        if (stopping_)
            return 0;
        const JobId id = nextJobId_++;
        const SessionGeneration generation =
            desiredGeneration_.load(std::memory_order_relaxed);
        auto canceled = std::make_shared<std::atomic_bool>(false);
        auto paused = std::make_shared<std::atomic_bool>(false);
        jobs_.emplace(id, JobRegistration{generation, canceled, paused});
        Job job{JobKey{id, generation, kind}, std::move(request),
                std::move(canceled), std::move(paused)};
        if (kind == JobKind::HealthCheck) {
            commands_.push_back(std::move(job));
        } else {
            // Health probes are deliberately low priority. New user work for
            // the same installed session may pass queued probes, but never an
            // InstallSession command (which is an ordering barrier).
            auto insertion = commands_.end();
            while (insertion != commands_.begin()) {
                auto previous = std::prev(insertion);
                const auto *queued = std::get_if<Job>(&*previous);
                if (!queued || queued->key.kind != JobKind::HealthCheck ||
                    queued->key.generation != generation) {
                    break;
                }
                insertion = previous;
            }
            commands_.insert(insertion, std::move(job));
        }
        lock.unlock();
        wake_.notify_one();
        return id;
    }

    bool cancel(JobId jobId) {
        if (jobId == 0)
            return false;

        std::lock_guard lock(mutex_);
        const auto registration = jobs_.find(jobId);
        if (registration == jobs_.end())
            return false;
        registration->second.canceled->store(true, std::memory_order_relaxed);
        if (activeJobId_ == jobId && activeCancel_) {
            activeCancel_->store(true, std::memory_order_relaxed);
            interruptActiveLocked();
        }
        wake_.notify_one();
        return true;
    }

    bool setPaused(JobId jobId, bool paused) {
        if (jobId == 0)
            return false;
        std::lock_guard lock(mutex_);
        const auto registration = jobs_.find(jobId);
        if (registration == jobs_.end())
            return false;
        registration->second.paused->store(paused, std::memory_order_relaxed);
        wake_.notify_all();
        return true;
    }

    int cancelGeneration(SessionGeneration generation) {
        std::lock_guard lock(mutex_);
        int canceledCount = 0;
        for (const auto &[id, registration] : jobs_) {
            Q_UNUSED(id);
            if (registration.generation != generation ||
                registration.canceled->exchange(true,
                                                std::memory_order_relaxed)) {
                continue;
            }
            ++canceledCount;
        }
        if (activeGeneration_ == generation && activeCancel_ &&
            !activeCancel_->exchange(true, std::memory_order_relaxed)) {
            ++canceledCount;
            interruptActiveLocked();
        }
        wake_.notify_one();
        return canceledCount;
    }

    int cancelAll() {
        std::lock_guard lock(mutex_);
        int canceledCount = 0;
        for (const auto &[id, registration] : jobs_) {
            Q_UNUSED(id);
            if (registration.canceled->exchange(true,
                                                std::memory_order_relaxed)) {
                continue;
            }
            ++canceledCount;
        }
        if (activeCancel_ &&
            !activeCancel_->exchange(true, std::memory_order_relaxed)) {
            ++canceledCount;
            interruptActiveLocked();
        }
        wake_.notify_one();
        return canceledCount;
    }

    void shutdown() {
        {
            std::lock_guard lock(mutex_);
            if (stopping_)
                return;
            stopping_ = true;
            for (const auto &[id, registration] : jobs_) {
                Q_UNUSED(id);
                registration.canceled->store(true, std::memory_order_relaxed);
            }
            interruptActiveLocked();
        }
        worker_.request_stop();
        wake_.notify_all();
        if (worker_.joinable())
            worker_.join();
    }

    SessionGeneration currentGeneration() const {
        return desiredGeneration_.load(std::memory_order_relaxed);
    }

    bool hasRequestedSession() const {
        return requestedSession_.load(std::memory_order_relaxed);
    }

    private:
    RemoteOperationController *owner_ = nullptr;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Command> commands_;
    std::unordered_map<JobId, JobRegistration> jobs_;
    bool stopping_ = false;
    JobId nextJobId_ = 1;
    std::atomic<SessionGeneration> desiredGeneration_{0};
    std::atomic_bool requestedSession_{false};

    // client_ is created, used, disconnected, and destroyed by worker_.
    std::unique_ptr<openscp::RemoteClient> client_;
    SessionGeneration installedGeneration_ = 0;

    // The following active-operation fields are guarded by mutex_. A backend's
    // interrupt() is explicitly allowed to run concurrently with its operation.
    openscp::RemoteClient *interruptTarget_ = nullptr;
    std::shared_ptr<std::atomic_bool> activeCancel_;
    JobId activeJobId_ = 0;
    SessionGeneration activeGeneration_ = 0;

    std::jthread worker_;

    template <typename Callback> void postToUi(Callback callback) {
        auto *receiver = owner_;
        QMetaObject::invokeMethod(
            receiver,
            [receiver, callback = std::move(callback)]() mutable {
                callback(receiver);
            },
            Qt::QueuedConnection);
    }

    void interruptActiveLocked() {
        if (interruptTarget_)
            interruptTarget_->interrupt();
    }

    static ResultHeader makeHeader(const JobKey &job,
                                   const RunSummary &summary) {
        return ResultHeader{job, summary.outcome, summary.error,
                            summary.remoteError, summary.partial};
    }

    void populateRemoteError(RunSummary &summary,
                             openscp::RemoteErrorKind fallbackKind =
                                 openscp::RemoteErrorKind::Unknown) const {
        if (summary.outcome == Outcome::Succeeded)
            return;
        if (summary.remoteError) {
            if (summary.error.isEmpty())
                summary.error =
                    QString::fromStdString(summary.remoteError.message);
            return;
        }
        if (client_) {
            const openscp::RemoteError backendError =
                client_->lastOperationError();
            if (backendError) {
                summary.remoteError = backendError;
                if (summary.error.isEmpty())
                    summary.error =
                        QString::fromStdString(backendError.message);
                return;
            }
        }
        summary.remoteError.kind = summary.outcome == Outcome::Canceled
                                       ? openscp::RemoteErrorKind::Canceled
                                       : fallbackKind;
        summary.remoteError.message = summary.error.toStdString();
    }

    bool canceled(const Job &job, std::stop_token stopToken) const {
        return stopToken.stop_requested() ||
               job.canceled->load(std::memory_order_relaxed);
    }

    bool waitWhilePaused(const Job &job, std::stop_token stopToken) {
        if (!job.paused->load(std::memory_order_relaxed))
            return !canceled(job, stopToken);
        std::unique_lock lock(mutex_);
        wake_.wait(lock, [this, &job, stopToken] {
            return stopToken.stop_requested() ||
                   job.canceled->load(std::memory_order_relaxed) ||
                   !job.paused->load(std::memory_order_relaxed) || stopping_;
        });
        return !canceled(job, stopToken) && !stopping_;
    }

    void postStarted(const JobKey &job) {
        postToUi([job](RemoteOperationController *controller) {
            emit controller->jobStarted(job);
        });
    }

    void postProgress(const Progress &progress) {
        postToUi([progress](RemoteOperationController *controller) {
            emit controller->jobProgress(progress);
        });
    }

    void postCompletion(const JobKey &job, const RunSummary &summary) {
        const Completion completion{
            makeHeader(job, summary), summary.visitedEntries,
            summary.matchedEntries,   summary.affectedEntries,
            summary.failedEntries,    summary.skippedSymlinks,
            summary.depthLimits,      summary.invalidNames,
            summary.unknownSizes,
        };
        postToUi([completion](RemoteOperationController *controller) {
            emit controller->jobFinished(completion);
        });
    }

    void postSessionState(SessionGeneration generation, bool available,
                          openscp::Protocol protocol,
                          openscp::ProtocolCapabilities capabilities) {
        const SessionState state{generation, available, protocol, capabilities};
        postToUi([state](RemoteOperationController *controller) {
            emit controller->sessionChanged(state);
        });
    }

    void publishEmptyTypedResult(const Job &job, const RunSummary &summary) {
        const ResultHeader header = makeHeader(job.key, summary);
        std::visit(
            [this, &job, &header, &summary](const auto &request) {
                using T = std::decay_t<decltype(request)>;
                if constexpr (std::is_same_v<T, ListRequest>) {
                    const ListResult result{
                        header, normalizeRemotePath(request.path), {}};
                    postToUi([result](RemoteOperationController *controller) {
                        emit controller->listCompleted(result);
                    });
                } else if constexpr (std::is_same_v<T, StatRequest>) {
                    const StatResult result{
                        header, normalizeRemotePath(request.path), false, {}};
                    postToUi([result](RemoteOperationController *controller) {
                        emit controller->statCompleted(result);
                    });
                } else if constexpr (std::is_same_v<T, HealthCheckRequest>) {
                    const HealthResult result{header, false, false};
                    postToUi([result](RemoteOperationController *controller) {
                        emit controller->healthCheckCompleted(result);
                    });
                } else if constexpr (std::is_same_v<T, ChecksumRequest>) {
                    const ChecksumResult result{
                        header,
                        normalizeRemotePath(request.path),
                        request.algorithm,
                        {},
                        0,
                        0};
                    postToUi([result](RemoteOperationController *controller) {
                        emit controller->checksumCompleted(result);
                    });
                } else if constexpr (std::is_same_v<T, SearchRequest> ||
                                     std::is_same_v<T, TraverseRequest>) {
                    const EntryBatch batch{job.key, {}, true};
                    postToUi([batch](RemoteOperationController *controller) {
                        emit controller->entriesBatchReady(batch);
                    });
                } else {
                    QString source;
                    if constexpr (std::is_same_v<T, RenameRequest>) {
                        source = normalizeRemotePath(request.from);
                    } else {
                        source = normalizeRemotePath(request.path);
                    }
                    const MutationResult result{header, source,
                                                summary.affectedEntries,
                                                summary.failedEntries};
                    postToUi([result](RemoteOperationController *controller) {
                        emit controller->mutationCompleted(result);
                    });
                }
            },
            job.request);
    }

    void run(std::stop_token stopToken) {
        while (!stopToken.stop_requested()) {
            Command command;
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, [this, stopToken] {
                    return stopToken.stop_requested() || !commands_.empty();
                });
                if (stopToken.stop_requested())
                    break;
                command = std::move(commands_.front());
                commands_.pop_front();
            }

            if (auto *install = std::get_if<InstallSessionCommand>(&command)) {
                processInstall(std::move(*install));
                continue;
            }
            Job job = std::move(std::get<Job>(command));
            const JobId completedJobId = job.key.id;
            processJob(std::move(job), stopToken);
            {
                std::lock_guard lock(mutex_);
                jobs_.erase(completedJobId);
            }
        }

        // Pending session commands are moved to this thread before destruction
        // so ownership never bounces back to the Qt/UI thread during shutdown.
        std::deque<Command> abandoned;
        {
            std::lock_guard lock(mutex_);
            abandoned.swap(commands_);
            interruptTarget_ = nullptr;
            activeCancel_.reset();
            activeJobId_ = 0;
            activeGeneration_ = 0;
            jobs_.clear();
        }
        for (Command &command : abandoned) {
            if (auto *install = std::get_if<InstallSessionCommand>(&command)) {
                if (install->client)
                    install->client->disconnect();
                install->client.reset();
            }
        }
        disconnectClient();
    }

    void disconnectClient() {
        {
            std::lock_guard lock(mutex_);
            interruptTarget_ = nullptr;
        }
        if (client_)
            client_->disconnect();
        client_.reset();
        installedGeneration_ = 0;
    }

    void processInstall(InstallSessionCommand install) {
        bool initiallyDesired = false;
        {
            std::lock_guard lock(mutex_);
            initiallyDesired =
                install.generation ==
                desiredGeneration_.load(std::memory_order_relaxed);
        }
        if (!initiallyDesired) {
            if (install.client)
                install.client->disconnect();
            return;
        }

        disconnectClient();
        client_ = std::move(install.client);
        const bool available = client_ && client_->isConnected();
        const openscp::Protocol protocol =
            client_ ? client_->protocol() : openscp::Protocol::Sftp;
        const openscp::ProtocolCapabilities capabilities =
            client_ ? client_->capabilities() : openscp::ProtocolCapabilities{};

        bool stillDesired = false;
        {
            // installSession() advances the desired generation under this same
            // mutex, so an accepted state cannot be published as current after
            // a newer replacement has already been requested.
            std::lock_guard lock(mutex_);
            stillDesired = install.generation ==
                           desiredGeneration_.load(std::memory_order_relaxed);
            if (stillDesired) {
                installedGeneration_ = install.generation;
                requestedSession_.store(available, std::memory_order_relaxed);
                postSessionState(installedGeneration_, available, protocol,
                                 capabilities);
            }
        }
        if (!stillDesired)
            disconnectClient();
    }

    void processJob(Job job, std::stop_token stopToken) {
        const SessionGeneration desiredGeneration =
            desiredGeneration_.load(std::memory_order_relaxed);
        if (job.key.generation != desiredGeneration) {
            RunSummary summary;
            summary.outcome = Outcome::Superseded;
            summary.error = QCoreApplication::translate(
                "RemoteOperationController", "Remote session was replaced");
            populateRemoteError(summary, openscp::RemoteErrorKind::Canceled);
            publishEmptyTypedResult(job, summary);
            postCompletion(job.key, summary);
            return;
        }
        if (job.key.generation != installedGeneration_) {
            RunSummary summary;
            summary.outcome = Outcome::Failed;
            summary.error = QCoreApplication::translate(
                "RemoteOperationController", "No active remote session");
            populateRemoteError(summary, openscp::RemoteErrorKind::Connection);
            publishEmptyTypedResult(job, summary);
            postCompletion(job.key, summary);
            return;
        }
        if (canceled(job, stopToken)) {
            RunSummary summary;
            summary.outcome = Outcome::Canceled;
            summary.error = QCoreApplication::translate(
                "RemoteOperationController", "Operation canceled");
            populateRemoteError(summary, openscp::RemoteErrorKind::Canceled);
            publishEmptyTypedResult(job, summary);
            postCompletion(job.key, summary);
            return;
        }
        if (!client_ || !client_->isConnected()) {
            RunSummary summary;
            summary.outcome = Outcome::Failed;
            summary.error = QCoreApplication::translate(
                "RemoteOperationController", "Remote session is not connected");
            populateRemoteError(summary, openscp::RemoteErrorKind::Connection);
            publishEmptyTypedResult(job, summary);
            postCompletion(job.key, summary);
            return;
        }

        {
            std::lock_guard lock(mutex_);
            interruptTarget_ = client_.get();
            activeCancel_ = job.canceled;
            activeJobId_ = job.key.id;
            activeGeneration_ = job.key.generation;
        }
        postStarted(job.key);

        RunSummary summary;
        try {
            summary = std::visit(
                [this, &job, stopToken](const auto &request) {
                    return execute(job, request, stopToken);
                },
                job.request);
        } catch (const std::exception &ex) {
            summary.outcome = Outcome::Failed;
            summary.error =
                QCoreApplication::translate("RemoteOperationController",
                                            "Remote operation failed: %1")
                    .arg(QString::fromUtf8(ex.what()));
            populateRemoteError(summary);
            publishEmptyTypedResult(job, summary);
        } catch (...) {
            summary.outcome = Outcome::Failed;
            summary.error = QCoreApplication::translate(
                "RemoteOperationController",
                "Remote operation failed unexpectedly");
            populateRemoteError(summary);
            publishEmptyTypedResult(job, summary);
        }

        {
            std::lock_guard lock(mutex_);
            interruptTarget_ = nullptr;
            activeCancel_.reset();
            activeJobId_ = 0;
            activeGeneration_ = 0;
        }
        populateRemoteError(summary);
        postCompletion(job.key, summary);
    }

    RunSummary execute(const Job &job, const ListRequest &request,
                       std::stop_token stopToken) {
        RunSummary summary;
        const QString path = normalizeRemotePath(request.path);
        std::vector<openscp::FileInfo> rawEntries;
        std::string rawError;
        const bool ok = client_->list(path.toStdString(), rawEntries, rawError);
        if (!ok) {
            summary.outcome =
                canceled(job, stopToken) ? Outcome::Canceled : Outcome::Failed;
            summary.error = operationError(
                rawError,
                summary.outcome == Outcome::Canceled
                    ? QCoreApplication::translate("RemoteOperationController",
                                                  "Operation canceled")
                    : QCoreApplication::translate(
                          "RemoteOperationController",
                          "Could not list remote path"));
        } else if (canceled(job, stopToken)) {
            summary.outcome = Outcome::Canceled;
            summary.error = QCoreApplication::translate(
                "RemoteOperationController", "Operation canceled");
        } else {
            summary.outcome = Outcome::Succeeded;
        }
        populateRemoteError(summary, openscp::RemoteErrorKind::RemoteIo);

        QVector<RemoteEntry> entries;
        if (summary.outcome == Outcome::Succeeded) {
            entries.reserve(static_cast<qsizetype>(rawEntries.size()));
            for (const auto &info : rawEntries) {
                const auto decodedName = decodeRemoteEntryName(info.name);
                if (!decodedName) {
                    ++summary.invalidNames;
                    continue;
                }
                const QString &name = *decodedName;
                if (!request.includeHidden &&
                    name.startsWith(QLatin1Char('.'))) {
                    continue;
                }
                entries.push_back(RemoteEntry{joinRemotePath(path, name), name,
                                              info, 1,
                                              isRemoteSymlink(info.mode)});
            }
            summary.visitedEntries = static_cast<quint64>(entries.size());
        }

        const ListResult result{makeHeader(job.key, summary), path,
                                std::move(entries)};
        postToUi([result](RemoteOperationController *controller) {
            emit controller->listCompleted(result);
        });
        return summary;
    }

    RunSummary execute(const Job &job, const StatRequest &request,
                       std::stop_token stopToken) {
        RunSummary summary;
        const QString path = normalizeRemotePath(request.path);
        openscp::FileInfo info;
        std::string rawError;
        const bool found = client_->stat(path.toStdString(), info, rawError);
        bool resultFound = found;
        if (found) {
            summary.outcome = Outcome::Succeeded;
            summary.visitedEntries = 1;
        } else if (canceled(job, stopToken)) {
            summary.outcome = Outcome::Canceled;
            summary.error = operationError(
                rawError,
                QCoreApplication::translate("RemoteOperationController",
                                            "Operation canceled"));
        } else if (rawError.empty()) {
            // RemoteClient::stat() uses false + empty error for not found.
            summary.outcome = Outcome::Succeeded;
            resultFound = false;
        } else {
            summary.outcome = Outcome::Failed;
            summary.error = QString::fromStdString(rawError);
        }
        populateRemoteError(summary, openscp::RemoteErrorKind::RemoteIo);

        const StatResult result{makeHeader(job.key, summary), path, resultFound,
                                info};
        postToUi([result](RemoteOperationController *controller) {
            emit controller->statCompleted(result);
        });
        return summary;
    }

    RunSummary execute(const Job &job, const MkdirRequest &request,
                       std::stop_token stopToken) {
        const QString path = normalizeRemotePath(request.path);
        std::string rawError;
        bool ok = true;
        if (!request.recursive) {
            ok = client_->mkdir(path.toStdString(), rawError, request.mode);
        } else {
            QString current = QStringLiteral("/");
            const QStringList parts =
                path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
            for (const QString &part : parts) {
                if (canceled(job, stopToken)) {
                    ok = false;
                    break;
                }
                current = joinRemotePath(current, part);
                bool isDirectory = false;
                std::string existsError;
                if (client_->exists(current.toStdString(), isDirectory,
                                    existsError)) {
                    if (!isDirectory) {
                        rawError = QCoreApplication::translate(
                                       "RemoteOperationController",
                                       "A non-directory item blocks %1")
                                       .arg(current)
                                       .toStdString();
                        ok = false;
                        break;
                    }
                    continue;
                }
                if (!existsError.empty()) {
                    rawError = std::move(existsError);
                    ok = false;
                    break;
                }
                if (!client_->mkdir(current.toStdString(), rawError,
                                    request.mode)) {
                    ok = false;
                    break;
                }
            }
        }
        RunSummary summary = mutationSummary(
            job, stopToken, ok, rawError,
            QCoreApplication::translate("RemoteOperationController",
                                        "Could not create remote directory"));
        if (ok)
            summary.affectedEntries = 1;
        postMutation(job, summary, path);
        return summary;
    }

    RunSummary execute(const Job &job, const CreateFileRequest &request,
                       std::stop_token stopToken) {
        const QString path = normalizeRemotePath(request.path);
        std::string rawError;
        bool ok = true;
        if (!request.overwrite) {
            openscp::FileInfo ignored;
            if (client_->stat(path.toStdString(), ignored, rawError)) {
                rawError =
                    QCoreApplication::translate("RemoteOperationController",
                                                "Remote item already exists")
                        .toStdString();
                ok = false;
            } else if (!rawError.empty()) {
                ok = false;
            }
        }

        QTemporaryFile emptyFile;
        if (ok && !emptyFile.open()) {
            rawError =
                QCoreApplication::translate("RemoteOperationController",
                                            "Could not create temporary file")
                    .toStdString();
            ok = false;
        }
        if (ok && canceled(job, stopToken))
            ok = false;
        if (ok) {
            emptyFile.close();
            ok = client_->put(emptyFile.fileName().toStdString(),
                              path.toStdString(), rawError);
        }

        RunSummary summary = mutationSummary(
            job, stopToken, ok, rawError,
            QCoreApplication::translate("RemoteOperationController",
                                        "Could not create remote file"));
        if (ok)
            summary.affectedEntries = 1;
        postMutation(job, summary, path);
        return summary;
    }

    RunSummary execute(const Job &job, const RenameRequest &request,
                       std::stop_token stopToken) {
        const QString from = normalizeRemotePath(request.from);
        const QString to = normalizeRemotePath(request.to);
        std::string rawError;
        const bool ok = client_->rename(from.toStdString(), to.toStdString(),
                                        rawError, request.overwrite);
        RunSummary summary = mutationSummary(
            job, stopToken, ok, rawError,
            QCoreApplication::translate("RemoteOperationController",
                                        "Could not rename remote item"));
        if (ok)
            summary.affectedEntries = 1;
        postMutation(job, summary, from);
        return summary;
    }

    RunSummary execute(const Job &job, const DeleteRequest &request,
                       std::stop_token stopToken) {
        const QString path = normalizeRemotePath(request.path);
        if (request.kind == DeleteKind::File || !request.recursive) {
            std::string rawError;
            const bool ok =
                request.kind == DeleteKind::File
                    ? client_->removeFile(path.toStdString(), rawError)
                    : client_->removeDir(path.toStdString(), rawError);
            RunSummary summary = mutationSummary(
                job, stopToken, ok, rawError,
                QCoreApplication::translate("RemoteOperationController",
                                            "Could not delete remote item"));
            if (ok)
                summary.affectedEntries = 1;
            postMutation(job, summary, path);
            return summary;
        }
        return executeRecursiveDelete(job, request, path, stopToken);
    }

    RunSummary execute(const Job &job, const ChmodRequest &request,
                       std::stop_token stopToken) {
        const QString path = normalizeRemotePath(request.path);
        if (!request.recursive) {
            std::string rawError;
            const bool ok =
                client_->chmod(path.toStdString(), request.mode, rawError);
            RunSummary summary =
                mutationSummary(job, stopToken, ok, rawError,
                                QCoreApplication::translate(
                                    "RemoteOperationController",
                                    "Could not change remote permissions"));
            if (ok)
                summary.affectedEntries = 1;
            postMutation(job, summary, path);
            return summary;
        }
        return executeRecursiveChmod(job, request, path, stopToken);
    }

    RunSummary execute(const Job &job, const HealthCheckRequest &request,
                       std::stop_token stopToken) {
        RunSummary summary;
        const bool connected = client_->isConnected();
        bool roundTripSucceeded = false;
        if (!connected) {
            summary.outcome = Outcome::Failed;
            summary.error = QCoreApplication::translate(
                "RemoteOperationController", "Remote session is not connected");
        } else if (canceled(job, stopToken)) {
            summary.outcome = Outcome::Canceled;
            summary.error = QCoreApplication::translate(
                "RemoteOperationController", "Operation canceled");
        } else {
            const QString path = normalizeRemotePath(request.path);
            std::string rawError;
            const openscp::ProtocolCapabilities capabilities =
                client_->capabilities();
            if (capabilities.can_stat) {
                openscp::FileInfo ignored;
                roundTripSucceeded =
                    client_->stat(path.toStdString(), ignored, rawError);
            } else if (capabilities.can_list) {
                std::vector<openscp::FileInfo> ignored;
                roundTripSucceeded =
                    client_->list(path.toStdString(), ignored, rawError);
            } else {
                // Some transfer-only protocols expose no harmless round trip.
                roundTripSucceeded = client_->isConnected();
            }
            if (roundTripSucceeded) {
                summary.outcome = Outcome::Succeeded;
            } else if (canceled(job, stopToken)) {
                summary.outcome = Outcome::Canceled;
                summary.error = operationError(
                    rawError,
                    QCoreApplication::translate("RemoteOperationController",
                                                "Operation canceled"));
            } else {
                summary.outcome = Outcome::Failed;
                summary.error = operationError(
                    rawError,
                    QCoreApplication::translate("RemoteOperationController",
                                                "Remote health check failed"));
            }
        }
        populateRemoteError(summary, openscp::RemoteErrorKind::Connection);

        const HealthResult result{makeHeader(job.key, summary), connected,
                                  roundTripSucceeded};
        postToUi([result](RemoteOperationController *controller) {
            emit controller->healthCheckCompleted(result);
        });
        return summary;
    }

    RunSummary execute(const Job &job, const SearchRequest &request,
                       std::stop_token stopToken) {
        return executeDiscovery(job, request.rootPath, request.traversal,
                                request.includeDirectories, request.query,
                                request.caseSensitivity, true, stopToken);
    }

    RunSummary execute(const Job &job, const TraverseRequest &request,
                       std::stop_token stopToken) {
        return executeDiscovery(job, request.rootPath, request.traversal,
                                request.includeDirectories, {},
                                Qt::CaseInsensitive, false, stopToken);
    }

    RunSummary execute(const Job &job, const ChecksumRequest &request,
                       std::stop_token stopToken) {
        RunSummary summary;
        const QString path = normalizeRemotePath(request.path);
        const QString algorithm = request.algorithm.trimmed().isEmpty()
                                      ? QStringLiteral("SHA-256")
                                      : request.algorithm.trimmed();
        std::vector<std::uint8_t> rawDigest;
        std::string rawError;
        quint64 processedBytes = 0;
        quint64 totalBytes = 0;
        auto lastProgress =
            std::chrono::steady_clock::now() - std::chrono::seconds(1);

        bool ok = false;
        const bool supported = client_->capabilities().can_checksum;
        if (!supported) {
            rawError =
                QCoreApplication::translate(
                    "RemoteOperationController",
                    "Remote checksums are not supported by this protocol")
                    .toStdString();
            summary.remoteError.kind = openscp::RemoteErrorKind::Unsupported;
            summary.remoteError.message = rawError;
        } else {
            ok = client_->checksum(
                path.toStdString(), algorithm.toStdString(), rawDigest,
                rawError,
                [this, &job, &path, &processedBytes, &totalBytes,
                 &lastProgress](std::size_t done, std::size_t total) {
                    processedBytes = static_cast<quint64>(done);
                    totalBytes = static_cast<quint64>(total);
                    const auto now = std::chrono::steady_clock::now();
                    if (done != total &&
                        now - lastProgress < std::chrono::milliseconds(100)) {
                        return;
                    }
                    lastProgress = now;
                    Progress progress;
                    progress.job = job.key;
                    progress.currentPath = path;
                    progress.processedBytes = processedBytes;
                    progress.totalBytes = totalBytes;
                    postProgress(progress);
                },
                [this, &job, stopToken] { return canceled(job, stopToken); });
        }

        if (ok && !canceled(job, stopToken)) {
            summary.outcome = Outcome::Succeeded;
            summary.visitedEntries = 1;
            summary.affectedEntries = 1;
        } else if (canceled(job, stopToken) ||
                   (supported && client_->lastOperationError().kind ==
                                     openscp::RemoteErrorKind::Canceled)) {
            summary.outcome = Outcome::Canceled;
            summary.error = operationError(
                rawError,
                QCoreApplication::translate("RemoteOperationController",
                                            "Checksum calculation canceled"));
        } else {
            summary.outcome = Outcome::Failed;
            summary.error = operationError(
                rawError, QCoreApplication::translate(
                              "RemoteOperationController",
                              "Could not calculate remote checksum"));
            summary.failedEntries = 1;
        }
        populateRemoteError(summary, openscp::RemoteErrorKind::RemoteIo);

        if (summary.outcome == Outcome::Succeeded) {
            if (totalBytes == 0)
                totalBytes = processedBytes;
            Progress progress;
            progress.job = job.key;
            progress.currentPath = path;
            progress.processedBytes = processedBytes;
            progress.totalBytes = totalBytes;
            postProgress(progress);
        }

        const QByteArray digest(
            reinterpret_cast<const char *>(rawDigest.data()),
            static_cast<qsizetype>(rawDigest.size()));
        const ChecksumResult result{
            makeHeader(job.key, summary),
            path,
            algorithm,
            summary.outcome == Outcome::Succeeded ? digest : QByteArray(),
            processedBytes,
            totalBytes};
        postToUi([result](RemoteOperationController *controller) {
            emit controller->checksumCompleted(result);
        });
        return summary;
    }

    RunSummary mutationSummary(const Job &job, std::stop_token stopToken,
                               bool ok, const std::string &rawError,
                               const QString &fallback) const {
        RunSummary summary;
        if (ok) {
            // A successful mutating call wins a race with a late cancel
            // request: the remote side has already committed the operation.
            summary.outcome = Outcome::Succeeded;
            return summary;
        }
        if (canceled(job, stopToken)) {
            summary.outcome = Outcome::Canceled;
            summary.error = operationError(
                rawError,
                QCoreApplication::translate("RemoteOperationController",
                                            "Operation canceled"));
            populateRemoteError(summary, openscp::RemoteErrorKind::Canceled);
            return summary;
        }
        summary.outcome = Outcome::Failed;
        summary.error = operationError(rawError, fallback);
        summary.failedEntries = 1;
        populateRemoteError(summary, openscp::RemoteErrorKind::RemoteIo);
        return summary;
    }

    void postMutation(const Job &job, const RunSummary &summary,
                      const QString &source) {
        const MutationResult result{makeHeader(job.key, summary), source,
                                    summary.affectedEntries,
                                    summary.failedEntries};
        postToUi([result](RemoteOperationController *controller) {
            emit controller->mutationCompleted(result);
        });
    }

    void postMutationProgress(const Job &job, const RunSummary &summary,
                              const QString &path) {
        postProgress(Progress{job.key, path, summary.visitedEntries,
                              summary.matchedEntries, summary.affectedEntries,
                              summary.failedEntries});
    }

    RunSummary executeRecursiveDelete(const Job &job,
                                      const DeleteRequest &request,
                                      const QString &root,
                                      std::stop_token stopToken) {
        RunSummary summary;
        RemoteTreeWalker walker(*client_);
        const RemoteTreeWalker::Options options = walkerOptionsFrom(
            request.traversal, RemoteTreeWalker::DepthPolicy::IncludeLimit);

        RemoteTreeWalker::Callbacks callbacks;
        callbacks.waitUntilReady = [this, &job, stopToken] {
            return !canceled(job, stopToken);
        };
        callbacks.onDirectoryListed =
            [&summary](const RemoteTreeWalker::Entry &) {
                ++summary.visitedEntries;
                return RemoteTreeWalker::Control::Continue;
            };
        callbacks.onEntry = [this, &job, &request,
                             &summary](const RemoteTreeWalker::Entry &entry) {
            ++summary.visitedEntries;
            if (entry.info.is_dir && !entry.isSymlink)
                return RemoteTreeWalker::Control::Continue;
            if (request.emptyDirectoriesOnly)
                return RemoteTreeWalker::Control::Continue;

            std::string removeError;
            if (!client_->removeFile(entry.path.toStdString(), removeError)) {
                summary.outcome = Outcome::Failed;
                summary.error = operationError(
                    removeError, QCoreApplication::translate(
                                     "RemoteOperationController",
                                     "Could not delete remote file"));
                ++summary.failedEntries;
                summary.partial = summary.affectedEntries > 0;
                return RemoteTreeWalker::Control::Abort;
            }
            ++summary.affectedEntries;
            postMutationProgress(job, summary, entry.path);
            return RemoteTreeWalker::Control::Continue;
        };
        callbacks.onSkippedSymlink =
            [&summary](const RemoteTreeWalker::Entry &) {
                ++summary.visitedEntries;
                ++summary.skippedSymlinks;
                return RemoteTreeWalker::Control::Continue;
            };
        callbacks.onInvalidName = [&summary](const RemoteTreeWalker::Entry &,
                                             const openscp::FileInfo &) {
            ++summary.invalidNames;
            ++summary.failedEntries;
            summary.error = QCoreApplication::translate(
                "RemoteOperationController",
                "An unsafe remote name was skipped during recursive "
                "deletion");
            summary.partial = summary.affectedEntries > 0;
            return RemoteTreeWalker::Control::Continue;
        };
        callbacks.onDepthLimit =
            [&summary](const RemoteTreeWalker::Entry &entry) {
                ++summary.depthLimits;
                ++summary.failedEntries;
                summary.outcome = Outcome::Failed;
                summary.error = QCoreApplication::translate(
                                    "RemoteOperationController",
                                    "Maximum traversal depth reached while "
                                    "deleting %1")
                                    .arg(entry.path);
                summary.partial = summary.affectedEntries > 0;
                return RemoteTreeWalker::Control::Abort;
            };
        callbacks.onListError = [this, &job, stopToken,
                                 &summary](const RemoteTreeWalker::Entry &,
                                           const std::string &listError) {
            summary.outcome =
                canceled(job, stopToken) ? Outcome::Canceled : Outcome::Failed;
            summary.error = operationError(
                listError, QCoreApplication::translate(
                               "RemoteOperationController",
                               "Could not enumerate remote directory before "
                               "deletion"));
            ++summary.failedEntries;
            summary.partial = summary.affectedEntries > 0;
            return RemoteTreeWalker::Control::Abort;
        };
        callbacks.onLeaveDirectory = [this, &job, &request, &summary](
                                         const RemoteTreeWalker::Entry &entry) {
            std::string removeError;
            if (!client_->removeDir(entry.path.toStdString(), removeError)) {
                ++summary.failedEntries;
                summary.partial = summary.affectedEntries > 0;
                if (request.emptyDirectoriesOnly)
                    return RemoteTreeWalker::Control::Continue;
                summary.outcome = Outcome::Failed;
                summary.error = operationError(
                    removeError, QCoreApplication::translate(
                                     "RemoteOperationController",
                                     "Could not delete remote directory"));
                return RemoteTreeWalker::Control::Abort;
            }
            ++summary.affectedEntries;
            postMutationProgress(job, summary, entry.path);
            return RemoteTreeWalker::Control::Continue;
        };

        const RemoteTreeWalker::Result walkResult =
            walker.walk(root, options, callbacks);
        if (walkResult.canceled) {
            summary.outcome = Outcome::Canceled;
            summary.error = QCoreApplication::translate(
                "RemoteOperationController", "Operation canceled");
            summary.partial = summary.affectedEntries > 0;
        } else if (!walkResult.aborted) {
            summary.outcome = summary.failedEntries == 0 ? Outcome::Succeeded
                                                         : Outcome::Failed;
        }
        if (summary.failedEntries > 0 && summary.affectedEntries > 0)
            summary.partial = true;
        populateRemoteError(summary, openscp::RemoteErrorKind::RemoteIo);
        postMutation(job, summary, root);
        return summary;
    }

    RunSummary executeRecursiveChmod(const Job &job,
                                     const ChmodRequest &request,
                                     const QString &root,
                                     std::stop_token stopToken) {
        RunSummary summary;
        RemoteTreeWalker walker(*client_);
        const RemoteTreeWalker::Options options = walkerOptionsFrom(
            request.traversal, RemoteTreeWalker::DepthPolicy::IncludeLimit);

        const auto applyMode = [this, &job, &request, &summary](
                                   const RemoteTreeWalker::Entry &entry) {
            std::string chmodError;
            if (!client_->chmod(entry.path.toStdString(), request.mode,
                                chmodError)) {
                ++summary.failedEntries;
                summary.error = operationError(
                    chmodError, QCoreApplication::translate(
                                    "RemoteOperationController",
                                    "Could not change remote permissions"));
            } else {
                ++summary.affectedEntries;
            }
            ++summary.visitedEntries;
            postMutationProgress(job, summary, entry.path);
            return RemoteTreeWalker::Control::Continue;
        };

        RemoteTreeWalker::Callbacks callbacks;
        callbacks.waitUntilReady = [this, &job, stopToken] {
            return !canceled(job, stopToken);
        };
        callbacks.onEnterDirectory = applyMode;
        callbacks.onEntry = [applyMode](const RemoteTreeWalker::Entry &entry) {
            if (entry.info.is_dir && !entry.isSymlink)
                return RemoteTreeWalker::Control::Continue;
            return applyMode(entry);
        };
        callbacks.onSkippedSymlink =
            [&summary](const RemoteTreeWalker::Entry &) {
                ++summary.visitedEntries;
                ++summary.skippedSymlinks;
                return RemoteTreeWalker::Control::Continue;
            };
        callbacks.onInvalidName = [&summary](const RemoteTreeWalker::Entry &,
                                             const openscp::FileInfo &) {
            ++summary.invalidNames;
            ++summary.failedEntries;
            summary.error = QCoreApplication::translate(
                "RemoteOperationController",
                "An unsafe remote name was skipped while changing "
                "permissions");
            return RemoteTreeWalker::Control::Continue;
        };
        callbacks.onDepthLimit = [&summary](const RemoteTreeWalker::Entry &) {
            ++summary.depthLimits;
            ++summary.failedEntries;
            summary.error = QCoreApplication::translate(
                "RemoteOperationController",
                "Maximum traversal depth reached while changing permissions");
            return RemoteTreeWalker::Control::Continue;
        };
        callbacks.onListError = [&summary](const RemoteTreeWalker::Entry &,
                                           const std::string &listError) {
            ++summary.failedEntries;
            summary.error = operationError(
                listError, QCoreApplication::translate(
                               "RemoteOperationController",
                               "Could not enumerate remote directory"));
            return RemoteTreeWalker::Control::Continue;
        };

        const RemoteTreeWalker::Result walkResult =
            walker.walk(root, options, callbacks);
        if (walkResult.canceled) {
            summary.outcome = Outcome::Canceled;
            summary.error = QCoreApplication::translate(
                "RemoteOperationController", "Operation canceled");
            summary.partial = summary.affectedEntries > 0;
        }

        if (summary.outcome != Outcome::Canceled) {
            summary.outcome = summary.failedEntries == 0 ? Outcome::Succeeded
                                                         : Outcome::Failed;
            summary.partial =
                summary.failedEntries > 0 && summary.affectedEntries > 0;
        }
        populateRemoteError(summary, openscp::RemoteErrorKind::RemoteIo);
        postMutation(job, summary, root);
        return summary;
    }

    RunSummary executeDiscovery(const Job &job, const QString &requestedRoot,
                                const TraversalOptions &options,
                                bool includeDirectories, const QString &query,
                                Qt::CaseSensitivity caseSensitivity,
                                bool searchMode, std::stop_token stopToken) {
        RunSummary summary;
        summary.outcome = Outcome::Succeeded;
        const QString root = normalizeRemotePath(requestedRoot);
        const int batchSize = safeBatchSize(options.batchSize);
        QVector<RemoteEntry> batch;
        batch.reserve(batchSize);
        bool rootListed = false;
        auto lastProgress = std::chrono::steady_clock::now();

        auto flushBatch = [this, &job, &batch](bool finalBatch) {
            QVector<RemoteEntry> entries;
            entries.swap(batch);
            EntryBatch payload{job.key, std::move(entries), finalBatch};
            postToUi([payload](RemoteOperationController *controller) {
                emit controller->entriesBatchReady(payload);
            });
        };
        auto maybePostProgress = [this, &job, &summary, &lastProgress](
                                     const QString &path, bool force) {
            const auto now = std::chrono::steady_clock::now();
            if (!force && now - lastProgress < std::chrono::milliseconds(100)) {
                return;
            }
            lastProgress = now;
            postProgress(Progress{
                job.key, path, summary.visitedEntries, summary.matchedEntries,
                summary.affectedEntries, summary.failedEntries});
        };

        RemoteTreeWalker walker(*client_);
        const RemoteTreeWalker::Options walkerOptions =
            walkerOptionsFrom(options);

        RemoteTreeWalker::Callbacks callbacks;
        callbacks.waitUntilReady = [this, &job, stopToken] {
            return waitWhilePaused(job, stopToken);
        };
        callbacks.onDirectoryListed =
            [&rootListed,
             &maybePostProgress](const RemoteTreeWalker::Entry &directory) {
                rootListed = true;
                maybePostProgress(directory.path, false);
                return RemoteTreeWalker::Control::Continue;
            };
        callbacks.onListError = [this, &job, stopToken, &summary,
                                 &rootListed](const RemoteTreeWalker::Entry &,
                                              const std::string &listError) {
            ++summary.failedEntries;
            summary.partial = rootListed;
            summary.error = operationError(
                listError, QCoreApplication::translate(
                               "RemoteOperationController",
                               "Could not enumerate remote directory"));
            if (!rootListed) {
                summary.outcome = canceled(job, stopToken) ? Outcome::Canceled
                                                           : Outcome::Failed;
                return RemoteTreeWalker::Control::Abort;
            }
            return RemoteTreeWalker::Control::Continue;
        };
        callbacks.onInvalidName = [&summary](const RemoteTreeWalker::Entry &,
                                             const openscp::FileInfo &) {
            ++summary.invalidNames;
            return RemoteTreeWalker::Control::Continue;
        };
        callbacks.onSkippedSymlink =
            [&summary](const RemoteTreeWalker::Entry &) {
                ++summary.visitedEntries;
                ++summary.skippedSymlinks;
                return RemoteTreeWalker::Control::Continue;
            };
        callbacks.onDepthLimit = [&summary](const RemoteTreeWalker::Entry &) {
            ++summary.depthLimits;
            return RemoteTreeWalker::Control::Continue;
        };
        callbacks.onEntry =
            [&summary, &batch, &flushBatch, batchSize, includeDirectories,
             searchMode, &query,
             caseSensitivity](const RemoteTreeWalker::Entry &entry) {
                ++summary.visitedEntries;
                if (!entry.info.is_dir && !entry.info.has_size)
                    ++summary.unknownSizes;
                const bool matches =
                    !searchMode || QFileInfo(entry.path)
                                       .fileName()
                                       .contains(query, caseSensitivity);
                const bool emitEntry =
                    matches && (includeDirectories || !entry.info.is_dir);
                if (emitEntry) {
                    ++summary.matchedEntries;
                    batch.push_back(RemoteEntry{entry.path, entry.relativePath,
                                                entry.info, entry.depth,
                                                entry.isSymlink});
                    if (batch.size() >= batchSize)
                        flushBatch(false);
                }
                return RemoteTreeWalker::Control::Continue;
            };

        const RemoteTreeWalker::Result walkResult =
            walker.walk(root, walkerOptions, callbacks);
        if (walkResult.canceled) {
            summary.outcome = Outcome::Canceled;
            summary.error = QCoreApplication::translate(
                "RemoteOperationController", "Operation canceled");
            summary.partial = summary.visitedEntries > 0;
        }

        if (summary.outcome != Outcome::Canceled &&
            summary.outcome != Outcome::Failed) {
            summary.outcome = rootListed ? Outcome::Succeeded : Outcome::Failed;
            if (!rootListed && summary.error.isEmpty())
                summary.error = QCoreApplication::translate(
                    "RemoteOperationController",
                    "Could not enumerate remote directory");
        }
        if (summary.failedEntries > 0 && rootListed)
            summary.partial = true;
        flushBatch(true);
        maybePostProgress(root, true);
        return summary;
    }
};

RemoteOperationController::RemoteOperationController(QObject *parent)
    : QObject(parent), impl_(std::make_unique<Impl>(this)) {
}

RemoteOperationController::~RemoteOperationController() {
    shutdown();
}

RemoteOperationController::SessionGeneration
RemoteOperationController::installSession(
    std::unique_ptr<openscp::RemoteClient> connectedClient) {
    return impl_->installSession(std::move(connectedClient));
}

RemoteOperationController::SessionGeneration
RemoteOperationController::clearSession() {
    return impl_->installSession(nullptr);
}

RemoteOperationController::SessionGeneration
RemoteOperationController::currentGeneration() const {
    return impl_->currentGeneration();
}

bool RemoteOperationController::hasRequestedSession() const {
    return impl_->hasRequestedSession();
}

RemoteOperationController::JobId
RemoteOperationController::submit(const ListRequest &request) {
    return impl_->enqueue(request, JobKind::List);
}

RemoteOperationController::JobId
RemoteOperationController::submit(const StatRequest &request) {
    return impl_->enqueue(request, JobKind::Stat);
}

RemoteOperationController::JobId
RemoteOperationController::submit(const MkdirRequest &request) {
    return impl_->enqueue(request, JobKind::Mkdir);
}

RemoteOperationController::JobId
RemoteOperationController::submit(const CreateFileRequest &request) {
    return impl_->enqueue(request, JobKind::CreateFile);
}

RemoteOperationController::JobId
RemoteOperationController::submit(const RenameRequest &request) {
    return impl_->enqueue(request, JobKind::Rename);
}

RemoteOperationController::JobId
RemoteOperationController::submit(const DeleteRequest &request) {
    return impl_->enqueue(request, JobKind::Delete);
}

RemoteOperationController::JobId
RemoteOperationController::submit(const ChmodRequest &request) {
    return impl_->enqueue(request, JobKind::Chmod);
}

RemoteOperationController::JobId
RemoteOperationController::submit(const HealthCheckRequest &request) {
    return impl_->enqueue(request, JobKind::HealthCheck);
}

RemoteOperationController::JobId
RemoteOperationController::submit(const SearchRequest &request) {
    return impl_->enqueue(request, JobKind::Search);
}

RemoteOperationController::JobId
RemoteOperationController::submit(const TraverseRequest &request) {
    return impl_->enqueue(request, JobKind::Traverse);
}

RemoteOperationController::JobId
RemoteOperationController::submit(const ChecksumRequest &request) {
    return impl_->enqueue(request, JobKind::Checksum);
}

bool RemoteOperationController::cancel(JobId jobId) {
    return impl_->cancel(jobId);
}

bool RemoteOperationController::setPaused(JobId jobId, bool paused) {
    return impl_->setPaused(jobId, paused);
}

int RemoteOperationController::cancelGeneration(SessionGeneration generation) {
    return impl_->cancelGeneration(generation);
}

int RemoteOperationController::cancelAll() {
    return impl_->cancelAll();
}

void RemoteOperationController::shutdown() {
    if (impl_)
        impl_->shutdown();
}
