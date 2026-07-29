#include "RemoteTreeWalker.hpp"

#include "RemotePath.hpp"

#include <algorithm>
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

} // namespace

RemoteTreeWalker::RemoteTreeWalker(openscp::RemoteClient &client)
    : client_(client) {
}

RemoteTreeWalker::Result
RemoteTreeWalker::walk(const QString &rootPath, const Options &options,
                       const Callbacks &callbacks) const {
    struct Frame {
        Entry directory;
        bool leave = false;
    };

    Result result;
    const int maxDepth = boundedDepth(options.maxDepth);
    Entry root;
    root.path = normalizeRemotePath(rootPath);
    root.info.is_dir = true;
    root.isRoot = true;

    std::vector<Frame> stack{{root, false}};
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

        for (const openscp::FileInfo &info : entries) {
            if (callbacks.waitUntilReady && !callbacks.waitUntilReady()) {
                result.canceled = true;
                return result;
            }

            const std::optional<QString> decodedName =
                decodeRemoteEntryName(info.name);
            if (!decodedName) {
                ++result.statistics.invalidNames;
                const Control invalidControl =
                    callbacks.onInvalidName
                        ? callbacks.onInvalidName(frame.directory, info)
                        : Control::Continue;
                if (shouldAbort(invalidControl, result))
                    return result;
                continue;
            }
            if (!options.includeHidden &&
                decodedName->startsWith(QLatin1Char('.'))) {
                continue;
            }

            Entry entry;
            entry.path = joinRemotePath(frame.directory.path, *decodedName);
            entry.relativePath = frame.directory.relativePath.isEmpty()
                                     ? *decodedName
                                     : frame.directory.relativePath +
                                           QLatin1Char('/') + *decodedName;
            if (!isSafeRemoteRelativePath(entry.relativePath)) {
                ++result.statistics.invalidNames;
                const Control invalidControl =
                    callbacks.onInvalidName
                        ? callbacks.onInvalidName(frame.directory, info)
                        : Control::Continue;
                if (shouldAbort(invalidControl, result))
                    return result;
                continue;
            }
            entry.info = info;
            entry.depth = frame.directory.depth + 1;
            entry.isSymlink = isRemoteSymlink(info.mode);
            ++result.statistics.entriesSeen;
            if (!info.is_dir && !info.has_size)
                ++result.statistics.unknownSizes;

            if (entry.isSymlink && options.skipSymlinks) {
                ++result.statistics.skippedSymlinks;
                if (shouldAbort(
                        invokeCallback(callbacks.onSkippedSymlink, entry),
                        result)) {
                    return result;
                }
                continue;
            }

            const Control entryControl =
                invokeCallback(callbacks.onEntry, entry);
            if (shouldAbort(entryControl, result))
                return result;
            if (!info.is_dir || entry.isSymlink ||
                entryControl == Control::SkipChildren) {
                continue;
            }

            const bool depthReached =
                options.depthPolicy == DepthPolicy::IncludeLimit
                    ? entry.depth > maxDepth
                    : entry.depth >= maxDepth;
            if (depthReached) {
                ++result.statistics.depthLimits;
                if (shouldAbort(invokeCallback(callbacks.onDepthLimit, entry),
                                result))
                    return result;
                continue;
            }
            stack.push_back({std::move(entry), false});
        }
    }

    result.completed = true;
    return result;
}
