// Drag-out staging batches: their names, the check before deleting one, and
// the deletions themselves. The deletions can take long on big batches, so
// callers run them outside the UI thread.
#pragma once

#include <QDateTime>
#include <QString>

class QSettings;

namespace openscpui {

// A drag-out staging batch is a folder in the staging root named after the
// UTC time it was created plus a random suffix. Only folders with such a name
// are ever deleted.
QString newStagingBatchName();
bool isStagingBatchName(const QString &name);

// The canonical path of `path` when it is a staging batch folder directly in
// the staging root, and not a symbolic link; empty otherwise. A drop names
// the batch it copies from, and only such a folder may be deleted after it.
QString stagingBatchPathForCleanup(const QSettings &settings,
                                   const QString &path);

// Deletes `batchPath`, then its staging root if nothing else is left in it.
void removeStagingBatch(const QString &batchPath);

// Deletes the batches directly in `root` last modified before `cutoff`,
// without following symbolic links. Returns how many it deleted.
int removeStagingBatchesOlderThan(const QString &root, const QDateTime &cutoff);

} // namespace openscpui
