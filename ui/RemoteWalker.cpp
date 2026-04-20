// Shared remote recursive walker for SFTP-like directory trees.
#include "RemoteWalker.hpp"

#include "MainWindowSharedUtils.hpp"

#include <QSet>
#include <QStringList>
#include <QVector>

#include <string>
#include <vector>

namespace {

bool isCanceled(const RemoteWalker::Options &options) {
    return options.cancel && options.cancel->load(std::memory_order_relaxed);
}

QString sanitizeRelativePath(const QString &relativePath) {
    // Remove control chars and forbid ".." segments.
    QString filtered;
    filtered.reserve(relativePath.size());
    for (QChar ch : relativePath) {
        if (ch.unicode() < 0x20u)
            continue;
#ifdef Q_OS_WIN
        if (ch == QLatin1Char(':'))
            continue;
#endif
        QChar normalized = ch;
        if (normalized == QLatin1Char('\\'))
            normalized = QLatin1Char('/');
        filtered.append(normalized);
    }

    const QStringList parts = filtered.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QStringList safeParts;
    safeParts.reserve(parts.size());
    for (const QString &part : parts) {
        if (part == QLatin1String("."))
            continue;
        if (part == QLatin1String(".."))
            return {};
        safeParts.push_back(part);
    }
    return safeParts.join(QLatin1Char('/'));
}

} // namespace

bool RemoteWalker::walk(openscp::SftpClient *client, const QString &baseRemotePath,
                        const Options &options,
                        const std::function<void(const Entry &fileEntry)> &onFile,
                        Stats *statsOut) {
    Stats stats;
    if (!client) {
        if (statsOut)
            *statsOut = stats;
        return false;
    }

    const int maxDepth = options.maxDepth > 0 ? options.maxDepth : 32;
    struct Node {
        QString remotePath;
        QString relativePath;
        int depth = 0;
    };

    QVector<Node> stack;
    stack.push_back({normalizeRemotePath(baseRemotePath), QString(), 0});
    QSet<QString> visited;

    while (!stack.isEmpty()) {
        if (isCanceled(options)) {
            stats.canceled = true;
            break;
        }

        const Node node = stack.back();
        stack.pop_back();

        if (node.depth > maxDepth)
            continue;

        const QString normalizedDir = normalizeRemotePath(node.remotePath);
        if (visited.contains(normalizedDir))
            continue;
        visited.insert(normalizedDir);
        ++stats.visitedDirectoryCount;

        if (options.onDirectoryEnter)
            options.onDirectoryEnter(normalizedDir, node.relativePath, node.depth);

        std::vector<openscp::FileInfo> children;
        std::string listError;
        if (!client->list(normalizedDir.toStdString(), children, listError)) {
            stats.partialError = true;
            ++stats.listFailureCount;
            stats.lastError = QString::fromStdString(listError);
            if (options.onListError)
                options.onListError(normalizedDir, stats.lastError);
            continue;
        }

        for (const auto &childInfo : children) {
            if (isCanceled(options)) {
                stats.canceled = true;
                break;
            }

            const QString name = QString::fromStdString(childInfo.name);
            if (!options.includeHidden && name.startsWith(QLatin1Char('.')))
                continue;

            QString ignoredWhy;
            if (options.validateName &&
                !options.validateName(name, &ignoredWhy)) {
                ++stats.skippedInvalidNameCount;
                continue;
            }

            const bool isSymlink = (childInfo.mode & 0120000u) == 0120000u;
            if (isSymlink && options.skipSymlinks) {
                ++stats.skippedSymlinkCount;
                continue;
            }

            QString childRelativePath = node.relativePath.isEmpty()
                                            ? name
                                            : node.relativePath +
                                                  QStringLiteral("/") + name;
            if (options.sanitizeRelativePath) {
                childRelativePath = sanitizeRelativePath(childRelativePath);
                if (childRelativePath.isEmpty()) {
                    ++stats.skippedInvalidNameCount;
                    continue;
                }
            }

            const QString childRemotePath = joinRemotePath(normalizedDir, name);
            if (childInfo.is_dir) {
                stack.push_back(
                    {childRemotePath, childRelativePath, node.depth + 1});
                continue;
            }

            if (!childInfo.has_size)
                ++stats.unknownSizeCount;

            if (onFile) {
                onFile(Entry{
                    .name = name,
                    .remotePath = childRemotePath,
                    .relativePath = childRelativePath,
                    .fileInfo = childInfo,
                    .isSymlink = isSymlink,
                    .depth = node.depth + 1,
                });
            }
        }
    }

    if (statsOut)
        *statsOut = stats;
    return true;
}

