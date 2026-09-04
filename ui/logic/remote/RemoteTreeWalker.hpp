// Shared, cancelable depth-first traversal for remote directory trees.
#pragma once

#include "openscp/RemoteClient.hpp"

#include <QString>

#include <functional>
#include <string>

class RemoteTreeWalker {
    public:
    enum class Control {
        Continue,
        SkipChildren,
        Abort,
    };

    enum class DepthPolicy {
        StopBeforeLimit,
        IncludeLimit,
    };

    struct Options {
        bool includeHidden = true;
        bool skipSymlinks = true;
        int maxDepth = 32;
        DepthPolicy depthPolicy = DepthPolicy::StopBeforeLimit;
    };

    struct Entry {
        QString path;
        QString relativePath;
        openscp::FileInfo info;
        int depth = 0;
        bool isSymlink = false;
        bool isRoot = false;
    };

    struct Statistics {
        quint64 entriesSeen = 0;
        quint64 directoriesListed = 0;
        quint64 skippedSymlinks = 0;
        quint64 depthLimits = 0;
        quint64 invalidNames = 0;
        quint64 unknownSizes = 0;
    };

    struct Result {
        bool completed = false;
        bool canceled = false;
        bool aborted = false;
        bool rootListed = false;
        Statistics statistics;
    };

    struct Callbacks {
        // May block while paused. False requests cooperative cancellation.
        std::function<bool()> waitUntilReady;
        std::function<Control(const Entry &)> onEnterDirectory;
        std::function<Control(const Entry &)> onDirectoryListed;
        std::function<Control(const Entry &)> onEntry;
        std::function<Control(const Entry &)> onLeaveDirectory;
        std::function<Control(const Entry &, const std::string &)> onListError;
        std::function<Control(const Entry &, const openscp::FileInfo &)>
            onInvalidName;
        std::function<Control(const Entry &)> onSkippedSymlink;
        std::function<Control(const Entry &)> onDepthLimit;
    };

    explicit RemoteTreeWalker(openscp::RemoteClient &client);

    [[nodiscard]] Result walk(const QString &rootPath, const Options &options,
                              const Callbacks &callbacks) const;

    private:
    openscp::RemoteClient &client_;
};
