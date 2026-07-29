// Value types exchanged with the serialized remote-operation worker.
#pragma once

#include "openscp/RemoteClient.hpp"

#include <QByteArray>
#include <QMetaType>
#include <QString>
#include <QVector>
#include <QtGlobal>

#include <cstdint>

namespace openscp::remote_operation {
Q_NAMESPACE

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
Q_ENUM_NS(JobKind)

enum class Outcome : quint8 {
    Succeeded,
    Failed,
    Canceled,
    Superseded,
};
Q_ENUM_NS(Outcome)

enum class DeleteKind : quint8 {
    File,
    Directory,
};
Q_ENUM_NS(DeleteKind)

struct JobKey {
    JobId id = 0;
    SessionGeneration generation = 0;
    JobKind kind = JobKind::List;
};

struct ResultHeader {
    JobKey job;
    Outcome outcome = Outcome::Failed;
    QString error;
    RemoteError remoteError;
    bool partial = false;
};

struct RemoteEntry {
    QString path;
    QString relativePath;
    FileInfo info;
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
    Protocol protocol = Protocol::Sftp;
    ProtocolCapabilities capabilities;
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
    FileInfo info;
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

} // namespace openscp::remote_operation

Q_DECLARE_METATYPE(openscp::remote_operation::JobKey)
Q_DECLARE_METATYPE(openscp::remote_operation::ResultHeader)
Q_DECLARE_METATYPE(openscp::remote_operation::RemoteEntry)
Q_DECLARE_METATYPE(openscp::remote_operation::SessionState)
Q_DECLARE_METATYPE(openscp::remote_operation::Progress)
Q_DECLARE_METATYPE(openscp::remote_operation::ListResult)
Q_DECLARE_METATYPE(openscp::remote_operation::StatResult)
Q_DECLARE_METATYPE(openscp::remote_operation::MutationResult)
Q_DECLARE_METATYPE(openscp::remote_operation::HealthResult)
Q_DECLARE_METATYPE(openscp::remote_operation::ChecksumResult)
Q_DECLARE_METATYPE(openscp::remote_operation::EntryBatch)
Q_DECLARE_METATYPE(openscp::remote_operation::Completion)
