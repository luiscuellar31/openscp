#pragma once

#include "TransferTypes.hpp"

#include <QSet>
#include <QString>
#include <QVector>

namespace openscpui {

struct TransferUiUpdate {
    bool scheduleRemoteRefresh = false;
    QString completionMessage;
};

class TransferUiController {
    public:
    void initialize(const QVector<TransferTask> &snapshot);
    TransferUiUpdate observe(const QVector<TransferTask> &upserts,
                             const QVector<quint64> &removedIds,
                             bool remotePanelActive, const QString &remoteRoot);
    void completeScheduledRefresh();
    void reset();

    bool hasScheduledRefresh() const { return refreshScheduled_; }

    private:
    static bool pathIsInsideRemoteRoot(const QString &candidatePath,
                                       const QString &rootPath);

    bool refreshScheduled_ = false;
    QSet<quint64> completedUploadIds_;
    QSet<quint64> notifiedTaskIds_;
};

} // namespace openscpui
