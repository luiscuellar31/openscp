#pragma once

#include "logic/transfers/TransferTypes.hpp"

#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

namespace openscpui {

struct TransferUiUpdate {
    // An upload added a folder to the visible remote folder, which has to be
    // listed again to show it.
    bool scheduleRemoteRefresh = false;
    // Uploads that finished directly in the visible remote folder, whose
    // rows can be updated one by one.
    QStringList uploadsInRemoteRoot;
    QString completionMessage;
    QStringList completedDownloadPathsToOpen;
};

class TransferUiController {
    public:
    void initialize(const QVector<TransferTask> &snapshot);
    // `isListed` tells whether the visible remote folder lists an entry by a
    // name; an upload into one of its folders that it lists changes nothing
    // it shows.
    TransferUiUpdate
    observe(const QVector<TransferTask> &upserts,
            const QVector<quint64> &removedIds, bool remotePanelActive,
            const QString &remoteRoot,
            const std::function<bool(const QString &name)> &isListed = {});
    // Opens the download where it was saved, which "Rename" on a conflict
    // makes another path than the one requested.
    void openDownloadWhenCompleted(quint64 taskId);
    void reset();

    private:
    // The path of `candidatePath` below `rootPath`, or a null string when it
    // is not inside it.
    static QString pathBelowRemoteRoot(const QString &candidatePath,
                                       const QString &rootPath);

    QSet<quint64> completedUploadIds_;
    QSet<quint64> notifiedTaskIds_;
    QSet<quint64> downloadsToOpen_;
};

} // namespace openscpui
