// Shared remote recursive walker for SFTP-like directory trees.
#pragma once

#include "openscp/SftpClient.hpp"

#include <QString>
#include <QtGlobal>

#include <atomic>
#include <functional>

namespace RemoteWalker {

struct Entry {
    QString name;
    QString remotePath;
    QString relativePath;
    openscp::FileInfo fileInfo;
    bool isSymlink = false;
    int depth = 0;
};

struct Options {
    bool includeHidden = true;
    bool skipSymlinks = false;
    bool sanitizeRelativePath = false;
    int maxDepth = 32;
    std::atomic_bool *cancel = nullptr;
    std::function<bool(const QString &name, QString *why)> validateName;
    std::function<void(const QString &remotePath, const QString &relativePath,
                       int depth)>
        onDirectoryEnter;
    std::function<void(const QString &remotePath, const QString &errorText)>
        onListError;
};

struct Stats {
    quint64 visitedDirectoryCount = 0;
    quint64 listFailureCount = 0;
    quint64 skippedInvalidNameCount = 0;
    quint64 skippedSymlinkCount = 0;
    quint64 unknownSizeCount = 0;
    bool partialError = false;
    bool canceled = false;
    QString lastError;
};

bool walk(openscp::SftpClient *client, const QString &baseRemotePath,
          const Options &options,
          const std::function<void(const Entry &fileEntry)> &onFile,
          Stats *statsOut = nullptr);

} // namespace RemoteWalker

