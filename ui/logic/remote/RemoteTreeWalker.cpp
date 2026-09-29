#include "logic/remote/RemoteTreeWalker.hpp"

#include "logic/navigation/RemotePath.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace {

int boundedDepth(int requestedDepth) {
    return std::clamp(requestedDepth, 1, 1024);
}

RemoteTreeWalker::Control
invokeCallback(const std::function<RemoteTreeWalker::Control(
                   const RemoteTreeWalker::Entry &)> &callback,
               const RemoteTreeWalker::Entry &entry) {
    return callback ? callback(entry) : RemoteTreeWalker::Control::Continue;
}

bool shouldAbort(RemoteTreeWalker::Control control,
                 RemoteTreeWalker::Result &result) {
    if (control != RemoteTreeWalker::Control::Abort)
        return false;
    result.aborted = true;
    return true;
}

// Reports the entries of one listed directory and hands subdirectories to
// pushDirectory. Returns false when the walk must stop (canceled or aborted).
bool visitEntries(
    const RemoteTreeWalker::Entry &directory,
    const std::vector<openscp::FileInfo> &entries,
    const RemoteTreeWalker::Options &options,
    const RemoteTreeWalker::Callbacks &callbacks,
    RemoteTreeWalker::Result &result,
    const std::function<void(RemoteTreeWalker::Entry)> &pushDirectory) {
    using Control = RemoteTreeWalker::Control;
    const int maxDepth = boundedDepth(options.maxDepth);
    for (const openscp::FileInfo &info : entries) {
        if (callbacks.waitUntilReady && !callbacks.waitUntilReady()) {
            result.canceled = true;
            return false;
        }

        const std::optional<QString> decodedName =
            decodeRemoteEntryName(info.name);
        if (!decodedName) {
            ++result.statistics.invalidNames;
            const Control invalidControl =
                callbacks.onInvalidName
                    ? callbacks.onInvalidName(directory, info)
                    : Control::Continue;
            if (shouldAbort(invalidControl, result))
                return false;
            continue;
        }
        if (!options.includeHidden &&
            decodedName->startsWith(QLatin1Char('.'))) {
            continue;
        }

        RemoteTreeWalker::Entry entry;
        entry.path = joinRemotePath(directory.path, *decodedName);
        entry.relativePath =
            directory.relativePath.isEmpty()
                ? *decodedName
                : directory.relativePath + QLatin1Char('/') + *decodedName;
        if (!isSafeRemoteRelativePath(entry.relativePath)) {
            ++result.statistics.invalidNames;
            const Control invalidControl =
                callbacks.onInvalidName
                    ? callbacks.onInvalidName(directory, info)
                    : Control::Continue;
            if (shouldAbort(invalidControl, result))
                return false;
            continue;
        }
        entry.info = info;
        entry.depth = directory.depth + 1;
        entry.isSymlink = isRemoteSymlink(info.mode);
        ++result.statistics.entriesSeen;
        if (!info.is_dir && !info.has_size)
            ++result.statistics.unknownSizes;

        if (entry.isSymlink && options.skipSymlinks) {
            ++result.statistics.skippedSymlinks;
            if (shouldAbort(invokeCallback(callbacks.onSkippedSymlink, entry),
                            result)) {
                return false;
            }
            continue;
        }

        const Control entryControl = invokeCallback(callbacks.onEntry, entry);
        if (shouldAbort(entryControl, result))
            return false;
        if (!info.is_dir || entry.isSymlink ||
            entryControl == Control::SkipChildren) {
            continue;
        }

        const bool depthReached =
            options.depthPolicy == RemoteTreeWalker::DepthPolicy::IncludeLimit
                ? entry.depth > maxDepth
                : entry.depth >= maxDepth;
        if (depthReached) {
            ++result.statistics.depthLimits;
            if (shouldAbort(invokeCallback(callbacks.onDepthLimit, entry),
                            result))
                return false;
            continue;
        }
        pushDirectory(std::move(entry));
    }
    return true;
}

} // namespace

RemoteTreeWalker::RemoteTreeWalker(openscp::RemoteClient &client)
    : client_(client) {
}

RemoteTreeWalker::Result
RemoteTreeWalker::walk(const QString &rootPath, const Options &options,
                       const Callbacks &callbacks) const {
    Entry root;
    root.path = normalizeRemotePath(rootPath);
    root.info.is_dir = true;
    root.isRoot = true;

    const bool canListInParallel =
        options.openListingConnection && options.maxListingConnections > 0 &&
        !callbacks.onEnterDirectory && !callbacks.onLeaveDirectory;
    return canListInParallel ? walkParallel(std::move(root), options, callbacks)
                             : walkSerial(std::move(root), options, callbacks);
}

RemoteTreeWalker::Result
RemoteTreeWalker::walkSerial(Entry root, const Options &options,
                             const Callbacks &callbacks) const {
    struct Frame {
        Entry directory;
        bool leave = false;
    };

    Result result;
    std::vector<Frame> stack{{std::move(root), false}};
    while (!stack.empty()) {
        if (callbacks.waitUntilReady && !callbacks.waitUntilReady()) {
            result.canceled = true;
            return result;
        }

        Frame frame = std::move(stack.back());
        stack.pop_back();
        if (frame.leave) {
            if (shouldAbort(
                    invokeCallback(callbacks.onLeaveDirectory, frame.directory),
                    result)) {
                return result;
            }
            continue;
        }

        const Control enterControl =
            invokeCallback(callbacks.onEnterDirectory, frame.directory);
        if (shouldAbort(enterControl, result))
            return result;
        if (enterControl == Control::SkipChildren)
            continue;

        std::vector<openscp::FileInfo> entries;
        std::string listError;
        if (!client_.list(frame.directory.path.toStdString(), entries,
                          listError)) {
            const Control errorControl =
                callbacks.onListError
                    ? callbacks.onListError(frame.directory, listError)
                    : Control::Abort;
            if (shouldAbort(errorControl, result))
                return result;
            continue;
        }

        ++result.statistics.directoriesListed;
        if (frame.directory.isRoot)
            result.rootListed = true;
        if (shouldAbort(
                invokeCallback(callbacks.onDirectoryListed, frame.directory),
                result)) {
            return result;
        }

        if (callbacks.onLeaveDirectory)
            stack.push_back({frame.directory, true});

        if (!visitEntries(frame.directory, entries, options, callbacks, result,
                          [&stack](Entry directory) {
                              stack.push_back({std::move(directory), false});
                          })) {
            return result;
        }
    }

    result.completed = true;
    return result;
}

RemoteTreeWalker::Result
RemoteTreeWalker::walkParallel(Entry root, const Options &options,
                               const Callbacks &callbacks) const {
    struct Listing {
        Entry directory;
        bool ok = false;
        std::vector<openscp::FileInfo> entries;
        std::string error;
    };

    // Directories wait in pending until the walking thread or a helper lists
    // them; helpers leave results in completed for the walking thread, which
    // alone runs callbacks.
    struct SharedState {
        std::mutex mutex;
        std::condition_variable changed;
        std::deque<Entry> pending;
        std::deque<Listing> completed;
        std::size_t inFlight = 0;
        bool stopping = false;
        std::vector<openscp::RemoteClient *> connectedHelpers;
    };

    // Helpers stop taking work while this many results wait, so a paused
    // walk also pauses the network.
    constexpr std::size_t kMaxWaitingListings = 16;

    SharedState shared;
    shared.pending.push_back(std::move(root));

    const auto helperLoop = [&shared, &options] {
        std::string connectError;
        std::unique_ptr<openscp::RemoteClient> client =
            options.openListingConnection(connectError);
        if (!client)
            return;
        {
            std::lock_guard lock(shared.mutex);
            if (shared.stopping) {
                client->disconnect();
                return;
            }
            shared.connectedHelpers.push_back(client.get());
        }

        while (true) {
            Listing listing;
            {
                std::unique_lock lock(shared.mutex);
                shared.changed.wait(lock, [&shared] {
                    return shared.stopping ||
                           (!shared.pending.empty() &&
                            shared.completed.size() < kMaxWaitingListings);
                });
                if (shared.stopping)
                    break;
                listing.directory = std::move(shared.pending.front());
                shared.pending.pop_front();
                ++shared.inFlight;
            }
            listing.ok = client->list(listing.directory.path.toStdString(),
                                      listing.entries, listing.error);
            const bool connectionLost = !listing.ok && !client->isConnected();
            {
                std::lock_guard lock(shared.mutex);
                --shared.inFlight;
                if (connectionLost) {
                    // Not the directory's fault: give it back for another
                    // connection to list.
                    shared.pending.push_front(std::move(listing.directory));
                } else if (!shared.stopping) {
                    shared.completed.push_back(std::move(listing));
                }
            }
            shared.changed.notify_all();
            if (connectionLost)
                break;
        }

        {
            std::lock_guard lock(shared.mutex);
            std::erase(shared.connectedHelpers, client.get());
        }
        client->disconnect();
    };

    std::vector<std::thread> helpers;
    // Joins the helpers on every return path, interrupting their I/O first.
    struct HelperGuard {
        SharedState &shared;
        std::vector<std::thread> &helpers;
        ~HelperGuard() {
            {
                std::lock_guard lock(shared.mutex);
                shared.stopping = true;
                for (openscp::RemoteClient *client : shared.connectedHelpers)
                    client->interrupt();
            }
            shared.changed.notify_all();
            for (std::thread &helper : helpers)
                helper.join();
        }
    } helperGuard{shared, helpers};

    const int helperCount = std::clamp(options.maxListingConnections, 0, 8);
    const auto startedAt = std::chrono::steady_clock::now();
    Result result;
    while (true) {
        if (callbacks.waitUntilReady && !callbacks.waitUntilReady()) {
            result.canceled = true;
            return result;
        }

        std::optional<Listing> listing;
        {
            std::unique_lock lock(shared.mutex);
            // Short walks never pay for extra connections.
            if (helpers.empty() && !shared.pending.empty() &&
                std::chrono::steady_clock::now() - startedAt >=
                    options.parallelDelay) {
                for (int index = 0; index < helperCount; ++index)
                    helpers.emplace_back(helperLoop);
            }

            if (!shared.completed.empty()) {
                listing = std::move(shared.completed.front());
                shared.completed.pop_front();
            } else if (!shared.pending.empty()) {
                // The walking thread lists too, so the walk still progresses
                // while helpers connect or if none can.
                Listing own;
                own.directory = std::move(shared.pending.front());
                shared.pending.pop_front();
                ++shared.inFlight;
                lock.unlock();
                own.ok = client_.list(own.directory.path.toStdString(),
                                      own.entries, own.error);
                lock.lock();
                --shared.inFlight;
                listing = std::move(own);
            } else if (shared.inFlight == 0) {
                break;
            } else {
                shared.changed.wait_for(lock, std::chrono::milliseconds(100));
                continue;
            }
        }
        shared.changed.notify_all();

        if (!listing->ok) {
            const Control errorControl =
                callbacks.onListError
                    ? callbacks.onListError(listing->directory, listing->error)
                    : Control::Abort;
            if (shouldAbort(errorControl, result))
                return result;
            continue;
        }

        ++result.statistics.directoriesListed;
        if (listing->directory.isRoot)
            result.rootListed = true;
        if (shouldAbort(
                invokeCallback(callbacks.onDirectoryListed, listing->directory),
                result)) {
            return result;
        }
        if (!visitEntries(listing->directory, listing->entries, options,
                          callbacks, result, [&shared](Entry directory) {
                              {
                                  std::lock_guard lock(shared.mutex);
                                  shared.pending.push_back(
                                      std::move(directory));
                              }
                              shared.changed.notify_one();
                          })) {
            return result;
        }
    }

    result.completed = true;
    return result;
}
