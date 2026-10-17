// Shared helper utilities for MainWindow split implementation files.
#pragma once

#include "logic/navigation/RemotePath.hpp"
#include "logic/transfers/TransferTypes.hpp"

#include <QDir>
#include <QFileInfo>
#include <QPair>
#include <QString>
#include <QVector>
#include <QWidget>

#include <functional>

inline constexpr int kNameColumn = 0;

struct PathDepthComparator {
    bool deepestFirst = false;

    bool operator()(const QString &left, const QString &right) const;
};

enum class LocalRenameResult { Moved, CrossDevice, Failed };

// Attempts only the filesystem's native rename operation. Callers can choose
// whether a cross-volume move should fall back to copying.
LocalRenameResult renameLocalEntry(const QString &sourcePath,
                                   const QString &targetPath,
                                   QString *error = nullptr);

// Removes a file, a directory tree, or a symbolic link. A link is removed
// itself, never through its target. A missing entry counts as removed.
bool removeLocalEntry(const QString &path);

// Copies a file, folder tree, or link through a staged sibling and replaces an
// existing destination only once the copy is complete. Hidden entries are
// included and links are recreated, not followed. Special files and copying a
// folder into itself fail. `shouldCancel` is asked before each entry; a
// canceled copy removes what it staged and leaves the destination as it was.
bool copyLocalEntry(const QString &srcPath, const QString &dstPath,
                    QString &error,
                    const std::function<bool()> &shouldCancel = {});

// Renames the entry, replacing an existing destination. Across volumes it
// falls back to copyLocalEntry() and removes the source only after the copy
// succeeds.
bool moveLocalEntry(const QString &srcPath, const QString &dstPath,
                    QString &error,
                    const std::function<bool()> &shouldCancel = {});

// Whether a local panel showing `path` can go up. Windows has no single root
// above its drives, so there a drive or share root goes up to a choice of
// drives (chooseLocalDrive()).
bool canGoUpLocally(const QString &path);

// Shows the mounted drives below `anchor` and returns the chosen one's root,
// or an empty string. Windows only; elsewhere it returns an empty string.
QString chooseLocalDrive(QWidget *anchor, const QString &currentPath);

bool isValidEntryName(const QString &name, QString *why = nullptr);
// isValidEntryName(), and also a valid name for a local file or folder on this
// computer: Windows refuses some names that servers accept, such as "a:b",
// "CON" or one ending in a dot. Use it wherever a name becomes local.
bool isValidLocalEntryName(const QString &name, QString *why = nullptr);

// Where a name typed in promptValidEntryName() will be used.
enum class EntryNameTarget { Remote, Local };
bool promptValidEntryName(QWidget *parent, const QString &dialogTitle,
                          const QString &labelText, const QString &initialValue,
                          QString &nameOut,
                          EntryNameTarget target = EntryNameTarget::Remote);
QString shortRemoteError(const QString &raw, const QString &fallback);

QVector<QPair<QString, QString>> buildLocalDestinationPairsWithOverwritePrompt(
    QWidget *parent, const QVector<QFileInfo> &sources,
    const QDir &destinationDir, int *skippedCount = nullptr);
