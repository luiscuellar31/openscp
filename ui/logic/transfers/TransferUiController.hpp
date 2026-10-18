#pragma once

#include "logic/transfers/TransferTypes.hpp"

#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

namespace openscpui {

struct TransferUiUpdate {
    bool scheduleRemoteRefresh = false;
    QString completionMessage;
    QStringList completedDownloadPathsToOpen;
};

class TransferUiController {
    public:
    void initialize(const QVector<TransferTask> &snapshot);
    TransferUiUpdate observe(const QVector<TransferTask> &upserts,
                             const QVector<quint64> &removedIds,
                             bool remotePanelActive, const QString &remoteRoot);
    // Opens the download where it was saved, which "Rename" on a conflict
    // makes another path than the one requested.
    void openDownloadWhenCompleted(quint64 taskId);
    void reset();

    private:
    static bool pathIsInsideRemoteRoot(const QString &candidatePath,
                                       const QString &rootPath);

    QSet<quint64> completedUploadIds_;
    QSet<quint64> notifiedTaskIds_;
    QSet<quint64> downloadsToOpen_;
};

} // namespace openscpui
