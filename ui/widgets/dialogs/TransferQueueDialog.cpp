// Table with per-task state and actions (pause/resume/retry/clear).
#include "widgets/dialogs/TransferQueueDialog.hpp"

#include "logic/common/AppSettings.hpp"
#include "logic/common/UiAlerts.hpp"
#include "logic/common/UiFormatters.hpp"
#include "widgets/platform/PlatformPathActions.hpp"

#include <QAbstractItemView>
#include <QAbstractTableModel>
#include <QButtonGroup>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QEasingCurve>
#include <QFileInfo>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QParallelAnimationGroup>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QSpinBox>
#include <QTableView>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

namespace {

constexpr int kProgressColumnWidthPx = 84;
constexpr int kWindowTransitionDurationMs = 190;

bool supportsTopLevelWindowTransitions() {
    const QString platformName = QGuiApplication::platformName();
    return platformName != QStringLiteral("offscreen") &&
           platformName != QStringLiteral("minimal");
}

QRect compactWindowRect(const QRect &restingGeometry) {
    QRect compact = restingGeometry;
    compact.setWidth(qMax(220, (restingGeometry.width() * 96) / 100));
    compact.setHeight(qMax(140, (restingGeometry.height() * 96) / 100));
    compact.moveCenter(restingGeometry.center() + QPoint(0, 10));
    return compact;
}

QString statusText(TransferTask::Status s) {
    switch (s) {
    case TransferTask::Status::Queued:
        return TransferQueueDialog::tr("Queued");
    case TransferTask::Status::Running:
        return TransferQueueDialog::tr("Running");
    case TransferTask::Status::Paused:
        return TransferQueueDialog::tr("Paused");
    case TransferTask::Status::Done:
        return TransferQueueDialog::tr("Completed");
    case TransferTask::Status::Error:
        return TransferQueueDialog::tr("Error");
    case TransferTask::Status::Canceled:
        return TransferQueueDialog::tr("Canceled");
    case TransferTask::Status::WaitingForConnection:
        return TransferQueueDialog::tr("Waiting for connection");
    case TransferTask::Status::RetryWaiting:
        return TransferQueueDialog::tr("Retrying");
    case TransferTask::Status::Skipped:
        return TransferQueueDialog::tr("Skipped");
    case TransferTask::Status::Warning:
        return TransferQueueDialog::tr("Warning");
    }
    return {};
}

QString displayNameForTask(const TransferTask &task) {
    const QString path =
        (task.type == TransferTask::Type::Upload) ? task.src : task.dst;
    QFileInfo taskFileInfo(path);
    if (!taskFileInfo.fileName().isEmpty())
        return taskFileInfo.fileName();

    const QString alternatePath =
        (task.type == TransferTask::Type::Upload) ? task.dst : task.src;
    QString trimmed = alternatePath;
    while (trimmed.endsWith('/'))
        trimmed.chop(1);
    const qsizetype slash = trimmed.lastIndexOf('/');
    if (slash >= 0 && slash + 1 < trimmed.size())
        return trimmed.mid(slash + 1);
    if (!trimmed.isEmpty())
        return trimmed;
    return TransferQueueDialog::tr("(unnamed)");
}

QString formatEta(int sec) {
    if (sec < 0)
        return QString::fromUtf8("—");
    const int hours = sec / 3600;
    const int minutes = (sec % 3600) / 60;
    const int seconds = sec % 60;
    if (hours > 0)
        return QString("%1h %2m").arg(hours).arg(minutes, 2, 10, QChar('0'));
    if (minutes > 0)
        return QString("%1m %2s").arg(minutes).arg(seconds, 2, 10, QChar('0'));
    return QString("%1s").arg(seconds);
}

bool canPauseStatus(TransferTask::Status s) {
    return s == TransferTask::Status::Queued ||
           s == TransferTask::Status::Running ||
           s == TransferTask::Status::RetryWaiting ||
           s == TransferTask::Status::WaitingForConnection;
}

bool canResumeStatus(TransferTask::Status s) {
    return s == TransferTask::Status::Paused ||
           s == TransferTask::Status::WaitingForConnection;
}

bool canActOnStatus(TransferTask::Status s) {
    return s == TransferTask::Status::Queued ||
           s == TransferTask::Status::Running ||
           s == TransferTask::Status::Paused ||
           s == TransferTask::Status::RetryWaiting ||
           s == TransferTask::Status::WaitingForConnection;
}

bool canRetryTask(const TransferTask &task) {
    return !task.commitUncertain &&
           (task.status == TransferTask::Status::Error ||
            task.status == TransferTask::Status::Canceled ||
            task.status == TransferTask::Status::Warning);
}

struct SelectedActionsState {
    bool hasSelection = false;
    bool canPause = false;
    bool canResume = false;
    bool canLimit = false;
    bool canCancel = false;
    bool canRetry = false;
    bool canShowDestination = false;
};

using TaskIndexById = QHash<quint64, const TransferTask *>;

TaskIndexById buildTaskIndexById(const QVector<TransferTask> &snapshot) {
    TaskIndexById index;
    index.reserve(snapshot.size());
    for (const auto &task : snapshot)
        index.insert(task.taskId, &task);
    return index;
}

template <typename Callback>
void forEachSelectedTask(const QVector<quint64> &ids,
                         const TaskIndexById &taskIndex, Callback &&callback) {
    for (quint64 taskId : ids) {
        const auto taskIt = taskIndex.constFind(taskId);
        if (taskIt == taskIndex.cend())
            continue;
        callback(taskId, *taskIt.value());
    }
}

template <typename Callback>
void withSelectedTasks(TransferManager *manager, const QVector<quint64> &ids,
                       Callback callback) {
    if (ids.isEmpty())
        return;
    // Use one immutable, ID-scoped snapshot so selected operations see the
    // same state without copying the complete queue.
    const auto taskIndex = buildTaskIndexById(manager->tasksSnapshot(ids));
    forEachSelectedTask(ids, taskIndex, callback);
}

SelectedActionsState buildSelectedActionsState(const QVector<quint64> &ids,
                                               const TaskIndexById &taskIndex) {
    SelectedActionsState out;
    out.hasSelection = !ids.isEmpty();
    if (!out.hasSelection)
        return out;

    forEachSelectedTask(ids, taskIndex, [&](quint64, const TransferTask &task) {
        out.canPause = out.canPause || canPauseStatus(task.status);
        out.canResume = out.canResume || canResumeStatus(task.status);
        out.canLimit = out.canLimit || canActOnStatus(task.status);
        out.canCancel = out.canCancel || canActOnStatus(task.status);
        out.canRetry = out.canRetry || canRetryTask(task);
        out.canShowDestination =
            out.canShowDestination ||
            task.type == TransferTask::Type::Download ||
            task.type == TransferTask::Type::CreateLocalDirectory;
    });
    return out;
}

} // namespace

class TransferTaskTableModel final : public QAbstractTableModel {
    public:
    enum Roles {
        StatusRole = Qt::UserRole + 1,
        TaskIdRole,
        ProgressRole,
        TypeRole,
        SourceRole,
        DestinationRole
    };

    enum Column {
        // Keep logical ids stable to avoid corrupting saved header states
        // across versions.
        ColType = 0,
        ColName = 1,
        ColSource = 2,
        ColDestination = 3,
        ColStatus = 4,
        ColProgress = 5,
        ColTransferred = 6,
        ColSpeed = 7,
        ColEta = 8,
        ColAttempts = 9,
        ColError = 10,
        ColCount = 11
    };

    explicit TransferTaskTableModel(QObject *parent = nullptr)
        : QAbstractTableModel(parent) {}

    int rowCount(const QModelIndex &parent = QModelIndex()) const override {
        if (parent.isValid())
            return 0;
        return static_cast<int>(tasks_.size());
    }

    int columnCount(const QModelIndex &parent = QModelIndex()) const override {
        if (parent.isValid())
            return 0;
        return ColCount;
    }

    QVariant headerData(int section, Qt::Orientation orientation,
                        int role) const override {
        if (role != Qt::DisplayRole)
            return {};
        if (orientation == Qt::Horizontal) {
            switch (section) {
            case ColName:
                return TransferQueueDialog::tr("Name");
            case ColStatus:
                return TransferQueueDialog::tr("Status");
            case ColProgress:
                return TransferQueueDialog::tr("Progress");
            case ColTransferred:
                return TransferQueueDialog::tr("Transferred");
            case ColSpeed:
                return TransferQueueDialog::tr("Speed");
            case ColEta:
                return TransferQueueDialog::tr("ETA");
            case ColType:
                return TransferQueueDialog::tr("Type");
            case ColSource:
                return TransferQueueDialog::tr("Source");
            case ColDestination:
                return TransferQueueDialog::tr("Destination");
            case ColAttempts:
                return TransferQueueDialog::tr("Attempts");
            case ColError:
                return TransferQueueDialog::tr("Error");
            default:
                return {};
            }
        }
        return section + 1;
    }

    QVariant data(const QModelIndex &index, int role) const override {
        if (!index.isValid())
            return {};
        if (index.row() < 0 || index.row() >= tasks_.size())
            return {};
        const auto &task = tasks_[index.row()];

        if (role == StatusRole)
            return static_cast<int>(task.status);
        if (role == TaskIdRole)
            return static_cast<qulonglong>(task.taskId);
        if (role == ProgressRole)
            return task.progress;
        if (role == TypeRole)
            return static_cast<int>(task.type);
        if (role == SourceRole)
            return task.src;
        if (role == DestinationRole)
            return task.dst;

        if (role == Qt::TextAlignmentRole) {
            if (index.column() == ColStatus || index.column() == ColProgress ||
                index.column() == ColTransferred ||
                index.column() == ColSpeed || index.column() == ColEta ||
                index.column() == ColType || index.column() == ColAttempts) {
                return static_cast<int>(Qt::AlignCenter);
            }
            return static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter);
        }

        if (role == Qt::ToolTipRole) {
            if (index.column() == ColSource)
                return task.src;
            if (index.column() == ColDestination)
                return task.dst;
            if (index.column() == ColError && !task.error.isEmpty())
                return task.error;
            if (index.column() == ColName)
                return displayNameForTask(task);
        }

        if (role != Qt::DisplayRole)
            return {};

        switch (index.column()) {
        case ColName:
            return displayNameForTask(task);
        case ColStatus:
            return statusText(task.status);
        case ColProgress:
            return QString::number(task.progress) + "%";
        case ColTransferred:
            if (task.bytesTotal > 0)
                return QString("%1 / %2").arg(formatByteSize(task.bytesDone),
                                              formatByteSize(task.bytesTotal));
            return formatByteSize(task.bytesDone);
        case ColSpeed:
            return formatTransferRate(task.currentSpeedKBps);
        case ColEta:
            return formatEta(task.etaSeconds);
        case ColType:
            switch (task.type) {
            case TransferTask::Type::Upload:
                return TransferQueueDialog::tr("Upload");
            case TransferTask::Type::Download:
                return TransferQueueDialog::tr("Download");
            case TransferTask::Type::CreateLocalDirectory:
                return TransferQueueDialog::tr("Create local folder");
            case TransferTask::Type::CreateRemoteDirectory:
                return TransferQueueDialog::tr("Create remote folder");
            case TransferTask::Type::DeleteLocalFile:
                return TransferQueueDialog::tr("Delete local file");
            case TransferTask::Type::DeleteLocalDirectory:
                return TransferQueueDialog::tr("Delete local folder");
            case TransferTask::Type::DeleteRemoteFile:
                return TransferQueueDialog::tr("Delete remote file");
            case TransferTask::Type::DeleteRemoteDirectory:
                return TransferQueueDialog::tr("Delete remote folder");
            }
            return {};
        case ColSource:
            return task.src;
        case ColDestination:
            return task.dst;
        case ColAttempts:
            return QString("%1/%2").arg(task.attempts).arg(task.maxAttempts);
        case ColError:
            return task.error;
        default:
            return {};
        }
    }

    void sync(const QVector<TransferTask> &incoming) {
        if (tasks_.size() != incoming.size()) {
            beginResetModel();
            tasks_ = incoming;
            endResetModel();
            rebuildIndex();
            return;
        }

        bool orderChanged = false;
        for (int rowIndex = 0; rowIndex < tasks_.size(); ++rowIndex) {
            if (tasks_[rowIndex].taskId != incoming[rowIndex].taskId) {
                orderChanged = true;
                break;
            }
        }
        if (orderChanged) {
            beginResetModel();
            tasks_ = incoming;
            endResetModel();
            rebuildIndex();
            return;
        }

        for (int row = 0; row < tasks_.size(); ++row) {
            updateRow(row, incoming[row]);
        }
        rebuildIndex();
    }

    void upsert(const QVector<TransferTask> &incoming) {
        QVector<TransferTask> missing;
        missing.reserve(incoming.size());
        for (const auto &task : incoming) {
            const auto found = rowById_.constFind(task.taskId);
            if (found == rowById_.cend())
                missing.push_back(task);
            else
                updateRow(found.value(), task);
        }
        if (!missing.isEmpty()) {
            const int first = static_cast<int>(tasks_.size());
            const int last = first + static_cast<int>(missing.size()) - 1;
            beginInsertRows({}, first, last);
            for (const auto &task : missing)
                tasks_.push_back(task);
            endInsertRows();
            rebuildIndex();
        }
    }

    void removeTaskIds(const QVector<quint64> &taskIds) {
        QVector<int> rows;
        rows.reserve(taskIds.size());
        for (quint64 taskId : taskIds) {
            const auto found = rowById_.constFind(taskId);
            if (found != rowById_.cend())
                rows.push_back(found.value());
        }
        std::sort(rows.begin(), rows.end(), std::greater<int>());
        rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
        // Adjacent rows go out as one range, so removing a contiguous
        // selection shifts the rows below it only once.
        for (qsizetype index = 0; index < rows.size();) {
            const int last = rows[index];
            int first = last;
            while (++index < rows.size() && rows[index] == first - 1)
                first = rows[index];
            beginRemoveRows({}, first, last);
            tasks_.remove(first, last - first + 1);
            endRemoveRows();
        }
        if (!rows.isEmpty())
            rebuildIndex();
    }

    const QVector<TransferTask> &tasks() const { return tasks_; }

    private:
    static bool differs(const TransferTask &prev, const TransferTask &next) {
        return prev.type != next.type || prev.src != next.src ||
               prev.dst != next.dst || prev.status != next.status ||
               prev.progress != next.progress ||
               prev.bytesDone != next.bytesDone ||
               prev.bytesTotal != next.bytesTotal ||
               prev.currentSpeedKBps != next.currentSpeedKBps ||
               prev.etaSeconds != next.etaSeconds ||
               prev.attempts != next.attempts ||
               prev.maxAttempts != next.maxAttempts ||
               prev.error != next.error || prev.phase != next.phase;
    }

    void updateRow(int row, const TransferTask &next) {
        if (row < 0 || row >= tasks_.size())
            return;
        const bool changed = differs(tasks_[row], next);
        tasks_[row] = next;
        if (changed) {
            emit dataChanged(index(row, 0), index(row, ColCount - 1),
                             {Qt::DisplayRole, Qt::TextAlignmentRole,
                              Qt::ToolTipRole, StatusRole, TaskIdRole,
                              ProgressRole, TypeRole, SourceRole,
                              DestinationRole});
        }
    }

    void rebuildIndex() {
        rowById_.clear();
        rowById_.reserve(tasks_.size());
        for (int row = 0; row < tasks_.size(); ++row)
            rowById_.insert(tasks_[row].taskId, row);
    }

    QVector<TransferTask> tasks_;
    QHash<quint64, int> rowById_;
};

class TransferTaskFilterProxyModel final : public QSortFilterProxyModel {
    public:
    explicit TransferTaskFilterProxyModel(QObject *parent = nullptr)
        : QSortFilterProxyModel(parent) {
        setDynamicSortFilter(true);
    }

    void setFilterMode(int mode) {
        if (mode_ == mode)
            return;
        mode_ = mode;
        invalidate();
    }

    protected:
    bool filterAcceptsRow(int sourceRow,
                          const QModelIndex &sourceParent) const override {
        if (mode_ == 0)
            return true; // All

        const QModelIndex statusIdx = sourceModel()->index(
            sourceRow, TransferTaskTableModel::ColStatus, sourceParent);
        const QVariant raw =
            sourceModel()->data(statusIdx, TransferTaskTableModel::StatusRole);
        if (!raw.isValid())
            return true;
        const auto status = static_cast<TransferTask::Status>(raw.toInt());

        switch (mode_) {
        case 1: // Active
            return status == TransferTask::Status::Queued ||
                   status == TransferTask::Status::Running ||
                   status == TransferTask::Status::Paused ||
                   status == TransferTask::Status::WaitingForConnection ||
                   status == TransferTask::Status::RetryWaiting;
        case 2: // Errors
            return status == TransferTask::Status::Error ||
                   status == TransferTask::Status::Warning;
        case 3: // Completed
            return status == TransferTask::Status::Done ||
                   status == TransferTask::Status::Skipped;
        case 4: // Canceled
            return status == TransferTask::Status::Canceled;
        default:
            return true;
        }
    }

    private:
    int mode_ = 0;
};

TransferQueueDialog::TransferQueueDialog(TransferManager *mgr, QWidget *parent)
    : QDialog(parent), mgr_(mgr) {
    setWindowTitle(tr("Transfer queue"));
    resize(1020, 560);
    setMinimumSize(820, 420);
    setSizeGripEnabled(true);

    auto *lay = new QVBoxLayout(this);

    // Row 1: quick filters
    auto *filters = new QWidget(this);
    auto *filterLayout = new QHBoxLayout(filters);
    filterLayout->setContentsMargins(0, 0, 0, 0);
    filterLayout->setSpacing(6);
    filterLayout->addWidget(new QLabel(tr("Show:"), filters));
    auto makeChip = [filters](const QString &text,
                              const QString &accessibleDescription) {
        auto *chipButton = new QToolButton(filters);
        chipButton->setText(text);
        chipButton->setCheckable(true);
        chipButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
        chipButton->setAccessibleName(text);
        chipButton->setAccessibleDescription(accessibleDescription);
        return chipButton;
    };
    filterAllBtn_ = makeChip(tr("All"), tr("Show all transfers"));
    filterActiveBtn_ = makeChip(tr("Active"), tr("Show active transfers"));
    filterErrorsBtn_ =
        makeChip(tr("Errors"), tr("Show transfers with errors or warnings"));
    filterCompletedBtn_ =
        makeChip(tr("Completed"), tr("Show completed transfers"));
    filterCanceledBtn_ =
        makeChip(tr("Canceled"), tr("Show canceled transfers"));
    filterGroup_ = new QButtonGroup(this);
    filterGroup_->setExclusive(true);
    filterGroup_->addButton(filterAllBtn_, FilterAll);
    filterGroup_->addButton(filterActiveBtn_, FilterActive);
    filterGroup_->addButton(filterErrorsBtn_, FilterErrors);
    filterGroup_->addButton(filterCompletedBtn_, FilterCompleted);
    filterGroup_->addButton(filterCanceledBtn_, FilterCanceled);
    filterAllBtn_->setChecked(true);
    filterLayout->addWidget(filterAllBtn_);
    filterLayout->addWidget(filterActiveBtn_);
    filterLayout->addWidget(filterErrorsBtn_);
    filterLayout->addWidget(filterCompletedBtn_);
    filterLayout->addWidget(filterCanceledBtn_);
    filterLayout->addStretch();
    lay->addWidget(filters);

    // Row 2: table
    model_ = new TransferTaskTableModel(this);
    proxy_ = new TransferTaskFilterProxyModel(this);
    proxy_->setSourceModel(model_);

    table_ = new QTableView(this);
    table_->setObjectName(QStringLiteral("transferQueueTable"));
    table_->setAccessibleName(tr("Transfers"));
    table_->setAccessibleDescription(
        tr("Transfer queue. Select one or more rows to manage them."));
    table_->setModel(proxy_);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setAlternatingRowColors(true);
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    table_->setWordWrap(false);
    table_->setTextElideMode(Qt::ElideMiddle);
    table_->verticalHeader()->setVisible(false);
    table_->horizontalHeader()->setSectionsMovable(true);
    table_->horizontalHeader()->setSectionResizeMode(
        TransferTaskTableModel::ColName, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(
        TransferTaskTableModel::ColStatus, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(
        TransferTaskTableModel::ColProgress, QHeaderView::Fixed);
    table_->horizontalHeader()->setSectionResizeMode(
        TransferTaskTableModel::ColTransferred, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(
        TransferTaskTableModel::ColSpeed, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(
        TransferTaskTableModel::ColEta, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(
        TransferTaskTableModel::ColType, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(
        TransferTaskTableModel::ColSource, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(
        TransferTaskTableModel::ColDestination, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(
        TransferTaskTableModel::ColAttempts, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(
        TransferTaskTableModel::ColError, QHeaderView::Stretch);
    table_->setColumnWidth(TransferTaskTableModel::ColName, 190);
    table_->setColumnWidth(TransferTaskTableModel::ColProgress,
                           kProgressColumnWidthPx);

    // Default visual order (without changing logical column ids).
    auto *header = table_->horizontalHeader();
    const QList<int> desiredOrder = {TransferTaskTableModel::ColName,
                                     TransferTaskTableModel::ColStatus,
                                     TransferTaskTableModel::ColProgress,
                                     TransferTaskTableModel::ColTransferred,
                                     TransferTaskTableModel::ColSpeed,
                                     TransferTaskTableModel::ColEta,
                                     TransferTaskTableModel::ColType,
                                     TransferTaskTableModel::ColSource,
                                     TransferTaskTableModel::ColDestination,
                                     TransferTaskTableModel::ColAttempts,
                                     TransferTaskTableModel::ColError};
    for (int visual = 0; visual < desiredOrder.size(); ++visual) {
        const int logical = desiredOrder[visual];
        const int currentVisual = header->visualIndex(logical);
        if (currentVisual >= 0 && currentVisual != visual)
            header->moveSection(currentVisual, visual);
    }
    lay->addWidget(table_, 1);

    emptyStateLabel_ = new QLabel(table_->viewport());
    emptyStateLabel_->setObjectName(QStringLiteral("transferQueueEmptyState"));
    emptyStateLabel_->setAlignment(Qt::AlignCenter);
    emptyStateLabel_->setWordWrap(true);
    emptyStateLabel_->setForegroundRole(QPalette::PlaceholderText);
    emptyStateLabel_->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto *emptyStateLayout = new QVBoxLayout(table_->viewport());
    emptyStateLayout->setContentsMargins(24, 24, 24, 24);
    emptyStateLayout->addWidget(emptyStateLabel_, 0, Qt::AlignCenter);

    // Row 3: actions for the current selection
    auto *selectionActions = new QWidget(this);
    selectionActions->setObjectName(
        QStringLiteral("transferQueueSelectionActions"));
    selectionActions->setAccessibleName(tr("Selected transfers"));
    auto *selectionActionsLayout = new QHBoxLayout(selectionActions);
    selectionActionsLayout->setContentsMargins(0, 0, 0, 0);
    selectionActionsLayout->setSpacing(6);

    auto *selectionLabel = new QLabel(tr("Selected:"), selectionActions);
    pauseSelBtn_ = new QPushButton(tr("Pause"), selectionActions);
    resumeSelBtn_ = new QPushButton(tr("Resume"), selectionActions);
    stopSelBtn_ = new QPushButton(tr("Cancel"), selectionActions);
    limitSelBtn_ = new QPushButton(tr("Limit…"), selectionActions);
    selectionLabel->setBuddy(pauseSelBtn_);

    pauseSelBtn_->setToolTip(tr("Pause the selected transfers"));
    resumeSelBtn_->setToolTip(tr("Resume the selected transfers"));
    stopSelBtn_->setToolTip(tr("Cancel the selected transfers"));
    limitSelBtn_->setToolTip(
        tr("Set a speed limit for the selected transfers"));
    for (QPushButton *button :
         {pauseSelBtn_, resumeSelBtn_, stopSelBtn_, limitSelBtn_}) {
        button->setAccessibleDescription(button->toolTip());
    }

    selectionActionsLayout->addWidget(selectionLabel);
    selectionActionsLayout->addWidget(pauseSelBtn_);
    selectionActionsLayout->addWidget(resumeSelBtn_);
    selectionActionsLayout->addWidget(stopSelBtn_);
    selectionActionsLayout->addWidget(limitSelBtn_);
    selectionActionsLayout->addStretch();
    lay->addWidget(selectionActions);

    // Row 4: summary badges
    auto *badges = new QWidget(this);
    badges->setObjectName(QStringLiteral("transferQueueSummary"));
    badges->setAccessibleName(tr("Transfer summary"));
    auto *hbBadges = new QHBoxLayout(badges);
    hbBadges->setContentsMargins(0, 0, 0, 0);
    hbBadges->setSpacing(6);
    auto makeBadge = [badges](const QString &text, const QString &objectName) {
        auto *badgeLabel = new QLabel(text, badges);
        badgeLabel->setObjectName(objectName);
        badgeLabel->setMargin(4);
        return badgeLabel;
    };
    badgeTotal_ =
        makeBadge(tr("Total: 0"), QStringLiteral("transferBadgeTotal"));
    badgeActive_ =
        makeBadge(tr("Active: 0"), QStringLiteral("transferBadgeActive"));
    badgeRunning_ =
        makeBadge(tr("Running: 0"), QStringLiteral("transferBadgeRunning"));
    badgePaused_ =
        makeBadge(tr("Paused: 0"), QStringLiteral("transferBadgePaused"));
    badgeErrors_ =
        makeBadge(tr("Errors: 0"), QStringLiteral("transferBadgeErrors"));
    badgeCompleted_ =
        makeBadge(tr("Completed: 0"), QStringLiteral("transferBadgeCompleted"));
    badgeCanceled_ =
        makeBadge(tr("Canceled: 0"), QStringLiteral("transferBadgeCanceled"));
    badgeParallel_ = makeBadge(tr("Parallel: %1").arg(mgr_->maxConcurrent()),
                               QStringLiteral("transferBadgeParallel"));
    badgeLimit_ = makeBadge(tr("Global limit: off"),
                            QStringLiteral("transferBadgeLimit"));
    hbBadges->addWidget(badgeTotal_);
    hbBadges->addWidget(badgeActive_);
    hbBadges->addWidget(badgeRunning_);
    hbBadges->addWidget(badgePaused_);
    hbBadges->addWidget(badgeErrors_);
    hbBadges->addWidget(badgeCompleted_);
    hbBadges->addWidget(badgeCanceled_);
    hbBadges->addStretch();
    hbBadges->addWidget(badgeParallel_);
    hbBadges->addWidget(badgeLimit_);
    lay->addWidget(badges);

    // Row 5: queue-wide and maintenance actions
    auto *queueActions = new QWidget(this);
    queueActions->setObjectName(QStringLiteral("transferQueueGlobalActions"));
    queueActions->setAccessibleName(tr("Transfer queue actions"));
    auto *queueActionsLayout = new QHBoxLayout(queueActions);
    queueActionsLayout->setContentsMargins(0, 0, 0, 0);
    queueActionsLayout->setSpacing(6);

    auto *queueLabel = new QLabel(tr("Queue:"), queueActions);
    pauseBtn_ = new QPushButton(tr("Pause"), queueActions);
    resumeBtn_ = new QPushButton(tr("Resume"), queueActions);
    retryBtn_ = new QPushButton(tr("Retry"), queueActions);
    stopAllBtn_ = new QPushButton(tr("Cancel all"), queueActions);
    clearMenuBtn_ = new QPushButton(tr("Clear"), queueActions);
    clearMenuBtn_->setObjectName(QStringLiteral("transferQueueClearButton"));
    auto *queueOptionsButton = new QPushButton(tr("Options…"), queueActions);
    queueOptionsButton->setObjectName(
        QStringLiteral("transferQueueOptionsButton"));
    closeBtn_ = new QPushButton(tr("Close"), queueActions);
    closeBtn_->setObjectName(QStringLiteral("transferQueueCloseButton"));
    queueLabel->setBuddy(pauseBtn_);

    pauseBtn_->setToolTip(tr("Pause all queued and running transfers"));
    resumeBtn_->setToolTip(tr("Resume the paused queue and paused tasks"));
    stopAllBtn_->setToolTip(
        tr("Cancel all queued, running, and paused transfers"));
    retryBtn_->setToolTip(tr("Retry transfers with Error or Canceled status"));
    clearMenuBtn_->setToolTip(tr("Remove finished transfers from the list"));
    clearMenuBtn_->setAccessibleName(tr("Clear transfers"));
    clearMenuBtn_->setAccessibleDescription(clearMenuBtn_->toolTip());
    queueOptionsButton->setToolTip(
        tr("Configure speed limits and automatic cleanup"));
    closeBtn_->setToolTip(tr("Close the transfer queue"));

    for (QPushButton *button : {pauseBtn_, resumeBtn_, retryBtn_, stopAllBtn_,
                                queueOptionsButton, closeBtn_}) {
        button->setAccessibleDescription(button->toolTip());
    }

    auto *clearMenu = new QMenu(clearMenuBtn_);
    clearCompletedAction_ = clearMenu->addAction(tr("Clear completed"));
    clearFailedAction_ = clearMenu->addAction(tr("Clear failed/canceled"));
    clearMenuBtn_->setMenu(clearMenu);

    queueActionsLayout->addWidget(queueLabel);
    queueActionsLayout->addWidget(pauseBtn_);
    queueActionsLayout->addWidget(resumeBtn_);
    queueActionsLayout->addWidget(retryBtn_);
    queueActionsLayout->addWidget(stopAllBtn_);
    queueActionsLayout->addWidget(clearMenuBtn_);
    queueActionsLayout->addWidget(queueOptionsButton);
    queueActionsLayout->addStretch();
    queueActionsLayout->addWidget(closeBtn_);
    lay->addWidget(queueActions);

    buildQueueOptionsDialog();

    // Connections
    connect(pauseBtn_, &QPushButton::clicked, this,
            &TransferQueueDialog::onPause);
    connect(resumeBtn_, &QPushButton::clicked, this,
            &TransferQueueDialog::onResume);
    connect(pauseSelBtn_, &QPushButton::clicked, this,
            &TransferQueueDialog::onPauseSelected);
    connect(resumeSelBtn_, &QPushButton::clicked, this,
            &TransferQueueDialog::onResumeSelected);
    connect(limitSelBtn_, &QPushButton::clicked, this,
            &TransferQueueDialog::onLimitSelected);
    connect(stopSelBtn_, &QPushButton::clicked, this,
            &TransferQueueDialog::onStopSelected);
    connect(stopAllBtn_, &QPushButton::clicked, this,
            &TransferQueueDialog::onStopAll);
    connect(retryBtn_, &QPushButton::clicked, this,
            &TransferQueueDialog::onRetry);
    connect(clearCompletedAction_, &QAction::triggered, this,
            &TransferQueueDialog::onClearDone);
    connect(clearFailedAction_, &QAction::triggered, this,
            &TransferQueueDialog::onClearFailedCanceled);
    connect(queueOptionsButton, &QPushButton::clicked, this,
            &TransferQueueDialog::onOpenQueueOptions);
    connect(closeBtn_, &QPushButton::clicked, this,
            &TransferQueueDialog::reject);
    connect(filterGroup_, &QButtonGroup::idClicked, this,
            &TransferQueueDialog::onFilterChanged);
    connect(mgr_, &TransferManager::tasksAdded, this,
            &TransferQueueDialog::onTasksAdded);
    connect(mgr_, &TransferManager::tasksUpdated, this,
            &TransferQueueDialog::onTasksUpdated);
    connect(mgr_, &TransferManager::tasksRemoved, this,
            &TransferQueueDialog::onTasksRemoved);
    connect(mgr_, &TransferManager::queueSettingsChanged, this,
            &TransferQueueDialog::onQueueSettingsChanged);
    connect(table_->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, &TransferQueueDialog::updateSummary);
    connect(table_, &QTableView::customContextMenuRequested, this,
            &TransferQueueDialog::showContextMenu);
    connect(this, &QDialog::finished, this, [this] { saveUiState(); });

    loadUiState();
    refresh();
}

void TransferQueueDialog::buildQueueOptionsDialog() {
    queueOptionsDialog_ = new QDialog(this);
    queueOptionsDialog_->setObjectName(
        QStringLiteral("transferQueueOptionsDialog"));
    queueOptionsDialog_->setWindowTitle(tr("Queue options"));
    queueOptionsDialog_->setModal(true);
    queueOptionsDialog_->setSizeGripEnabled(false);

    auto *optionsLayout = new QVBoxLayout(queueOptionsDialog_);
    optionsLayout->setSizeConstraint(QLayout::SetFixedSize);
    auto *optionsForm = new QFormLayout;
    optionsForm->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);
    optionsForm->setVerticalSpacing(10);

    auto *speedField = new QWidget(queueOptionsDialog_);
    auto *speedFieldLayout = new QHBoxLayout(speedField);
    speedFieldLayout->setContentsMargins(0, 0, 0, 0);
    speedFieldLayout->setSpacing(6);
    speedSpin_ = new QSpinBox(speedField);
    speedSpin_->setObjectName(QStringLiteral("transferQueueSpeedLimit"));
    speedSpin_->setRange(0, 1'000'000);
    speedSpin_->setValue(mgr_->globalSpeedLimitKBps());
    speedSpin_->setSuffix(QStringLiteral(" KB/s"));
    speedSpin_->setAccessibleName(tr("Global speed limit"));
    auto *applySpeedButton = new QPushButton(tr("Apply limit"), speedField);
    applySpeedButton->setObjectName(QStringLiteral("transferQueueApplyLimit"));
    applySpeedButton->setAccessibleDescription(
        tr("Apply the global speed limit to the transfer queue"));
    speedFieldLayout->addWidget(speedSpin_);
    speedFieldLayout->addWidget(applySpeedButton);
    optionsForm->addRow(tr("Global speed limit:"), speedField);

    autoClearModeCombo_ = new QComboBox(queueOptionsDialog_);
    autoClearModeCombo_->setObjectName(
        QStringLiteral("transferQueueAutoClearMode"));
    autoClearModeCombo_->addItem(tr("Off"), AutoClearOff);
    autoClearModeCombo_->addItem(tr("Completed"), AutoClearCompleted);
    autoClearModeCombo_->addItem(tr("Failed/Canceled"),
                                 AutoClearFailedCanceled);
    autoClearModeCombo_->addItem(tr("All finished"), AutoClearFinished);
    autoClearModeCombo_->setAccessibleName(tr("Automatic queue cleanup"));
    optionsForm->addRow(tr("Automatic cleanup:"), autoClearModeCombo_);

    autoClearMinutesSpin_ = new QSpinBox(queueOptionsDialog_);
    autoClearMinutesSpin_->setObjectName(
        QStringLiteral("transferQueueAutoClearDelay"));
    autoClearMinutesSpin_->setRange(1, 1440);
    autoClearMinutesSpin_->setSuffix(tr(" min"));
    autoClearMinutesSpin_->setAccessibleName(tr("Automatic cleanup delay"));
    optionsForm->addRow(tr("Cleanup delay:"), autoClearMinutesSpin_);
    optionsLayout->addLayout(optionsForm);

    auto *buttons =
        new QDialogButtonBox(QDialogButtonBox::Close, queueOptionsDialog_);
    connect(buttons, &QDialogButtonBox::rejected, queueOptionsDialog_,
            &QDialog::reject);
    optionsLayout->addWidget(buttons);

    connect(applySpeedButton, &QPushButton::clicked, this,
            &TransferQueueDialog::onApplyGlobalSpeed);
    connect(autoClearModeCombo_, &QComboBox::currentIndexChanged, this,
            &TransferQueueDialog::onAutoClearChanged);
    connect(autoClearMinutesSpin_, &QSpinBox::valueChanged, this,
            &TransferQueueDialog::onAutoClearChanged);
}

void TransferQueueDialog::presentAnimated() {
    if (!supportsTopLevelWindowTransitions()) {
        stopWindowTransition();
        show();
        return;
    }

    if (isVisible() && windowTransition_ != WindowTransition::Hiding) {
        raise();
        activateWindow();
        return;
    }
    startWindowTransition(WindowTransition::Showing);
}

void TransferQueueDialog::reject() {
    if (!supportsTopLevelWindowTransitions()) {
        stopWindowTransition();
        QDialog::reject();
        return;
    }

    if (!isVisible()) {
        stopWindowTransition();
        QDialog::reject();
        return;
    }
    startWindowTransition(WindowTransition::Hiding);
}

void TransferQueueDialog::startWindowTransition(WindowTransition transition) {
    const bool showing = transition == WindowTransition::Showing;
    if (!showing && windowTransition_ == WindowTransition::Hiding)
        return;

    const bool wasVisible = isVisible();
    if ((showing && !wasVisible) ||
        (!showing && windowTransition_ == WindowTransition::None) ||
        !restingGeometry_.isValid()) {
        restingGeometry_ = geometry();
    }

    if (showing && !wasVisible) {
        setGeometry(compactWindowRect(restingGeometry_));
        setWindowOpacity(0.0);
        show();
    }
    if (showing) {
        raise();
        activateWindow();
    }

    const QRect startGeometry = geometry();
    const qreal startOpacity = windowOpacity();
    const QRect endGeometry =
        showing ? restingGeometry_ : compactWindowRect(restingGeometry_);
    const qreal endOpacity = showing ? 1.0 : 0.0;

    stopWindowTransition();
    windowTransition_ = transition;

    auto *group = new QParallelAnimationGroup(this);
    auto *fade = new QPropertyAnimation(this, "windowOpacity", group);
    fade->setDuration(kWindowTransitionDurationMs);
    fade->setStartValue(startOpacity);
    fade->setEndValue(endOpacity);
    fade->setEasingCurve(QEasingCurve::OutCubic);

    auto *geometryAnimation = new QPropertyAnimation(this, "geometry", group);
    geometryAnimation->setDuration(kWindowTransitionDurationMs);
    geometryAnimation->setStartValue(startGeometry);
    geometryAnimation->setEndValue(endGeometry);
    geometryAnimation->setEasingCurve(QEasingCurve::OutCubic);

    windowTransitionAnimation_ = group;
    connect(group, &QParallelAnimationGroup::finished, this,
            [this, group, transition] {
                if (windowTransitionAnimation_ != group) {
                    group->deleteLater();
                    return;
                }
                windowTransitionAnimation_.clear();
                windowTransition_ = WindowTransition::None;
                setGeometry(restingGeometry_);
                setWindowOpacity(1.0);
                group->deleteLater();
                if (transition == WindowTransition::Hiding)
                    QDialog::reject();
            });
    group->start();
}

void TransferQueueDialog::stopWindowTransition() {
    QParallelAnimationGroup *animation = windowTransitionAnimation_.data();
    windowTransitionAnimation_.clear();
    windowTransition_ = WindowTransition::None;
    if (!animation)
        return;
    animation->stop();
    animation->deleteLater();
}

void TransferQueueDialog::refresh() {
    if (!model_)
        return;
    // Refresh always works from manager snapshot to avoid stale view state.
    const auto snapshot = mgr_->tasksSnapshot();
    model_->sync(snapshot);
    maybeAutoClear(snapshot);
    updateSummary();
}

void TransferQueueDialog::onTasksAdded(const QVector<quint64> &taskIds) {
    if (!model_)
        return;
    model_->upsert(mgr_->tasksSnapshot(taskIds));
    updateSummary();
}

void TransferQueueDialog::onTasksUpdated(const QVector<quint64> &taskIds) {
    if (!model_)
        return;
    const auto updated = mgr_->tasksSnapshot(taskIds);
    model_->upsert(updated);
    const bool hasTerminalUpdate = std::any_of(
        updated.cbegin(), updated.cend(), [](const TransferTask &task) {
            return task.status == TransferTask::Status::Done ||
                   task.status == TransferTask::Status::Error ||
                   task.status == TransferTask::Status::Canceled ||
                   task.status == TransferTask::Status::Skipped ||
                   task.status == TransferTask::Status::Warning;
        });
    if (hasTerminalUpdate)
        maybeAutoClear(model_->tasks());
    updateSummary();
}

void TransferQueueDialog::onTasksRemoved(const QVector<quint64> &taskIds) {
    if (!model_)
        return;
    model_->removeTaskIds(taskIds);
    updateSummary();
}

void TransferQueueDialog::onQueueSettingsChanged() {
    if (speedSpin_ && queueOptionsDialog_ &&
        !queueOptionsDialog_->isVisible()) {
        speedSpin_->setValue(mgr_->globalSpeedLimitKBps());
    }
    updateSummary();
}

void TransferQueueDialog::onPause() {
    mgr_->pauseAll();
}
void TransferQueueDialog::onResume() {
    mgr_->resumeAll();
}
void TransferQueueDialog::onRetry() {
    mgr_->retryFailed();
}
void TransferQueueDialog::onClearDone() {
    mgr_->clearCompleted();
}

void TransferQueueDialog::onPauseSelected() {
    const auto ids = selectedTaskIds();
    withSelectedTasks(mgr_, ids,
                      [this](quint64 taskId, const TransferTask &task) {
                          if (canPauseStatus(task.status))
                              mgr_->pauseTask(taskId);
                      });
}

void TransferQueueDialog::onResumeSelected() {
    const auto ids = selectedTaskIds();
    withSelectedTasks(mgr_, ids,
                      [this](quint64 taskId, const TransferTask &task) {
                          if (canResumeStatus(task.status))
                              mgr_->resumeTask(taskId);
                      });
}

void TransferQueueDialog::onApplyGlobalSpeed() {
    mgr_->setGlobalSpeedLimitKBps(speedSpin_->value());
    updateSummary();
}

void TransferQueueDialog::onLimitSelected() {
    const auto ids = selectedTaskIds();
    if (ids.isEmpty())
        return;
    QVector<quint64> eligible;
    eligible.reserve(ids.size());
    withSelectedTasks(mgr_, ids, [&](quint64 taskId, const TransferTask &task) {
        if (canActOnStatus(task.status))
            eligible.push_back(taskId);
    });
    if (eligible.isEmpty())
        return;

    bool inputAccepted = false;
    const int speedLimitKbps = QInputDialog::getInt(
        this, tr("Limit for task(s)"), tr("KB/s (0 = no limit)"), 0, 0,
        1'000'000, 1, &inputAccepted);
    if (!inputAccepted)
        return;
    for (quint64 taskId : eligible)
        mgr_->setTaskSpeedLimit(taskId, speedLimitKbps);
}

void TransferQueueDialog::onStopSelected() {
    const auto ids = selectedTaskIds();
    withSelectedTasks(mgr_, ids,
                      [this](quint64 taskId, const TransferTask &task) {
                          if (canActOnStatus(task.status))
                              mgr_->cancelTask(taskId);
                      });
}

void TransferQueueDialog::onStopAll() {
    mgr_->cancelAll();
}

void TransferQueueDialog::onFilterChanged(int filterId) {
    if (!proxy_)
        return;
    proxy_->setFilterMode(filterId);
    if (table_)
        table_->clearSelection();
    updateSummary();
    saveUiState();
}

void TransferQueueDialog::onRetrySelected() {
    const auto ids = selectedTaskIds();
    withSelectedTasks(mgr_, ids,
                      [this](quint64 taskId, const TransferTask &task) {
                          if (canRetryTask(task))
                              mgr_->retryTask(taskId);
                      });
}

void TransferQueueDialog::onShowDestinationFolder() {
    const auto ids = selectedTaskIds();
    if (ids.isEmpty())
        return;

    QStringList destinations;
    withSelectedTasks(mgr_, ids, [&](quint64, const TransferTask &task) {
        if (task.type != TransferTask::Type::Download &&
            task.type != TransferTask::Type::CreateLocalDirectory)
            return;
        if (!task.dst.isEmpty())
            destinations.push_back(task.dst);
    });
    if (destinations.isEmpty())
        return;

    const openscpui::PathActionResult result =
        openscpui::PlatformPathActions::revealPaths(destinations);
    if (result.failed())
        UiAlerts::warning(this, tr("Show folder"), result.error);
}

void TransferQueueDialog::onCopySourcePath() {
    const auto ids = selectedTaskIds();
    if (ids.isEmpty())
        return;
    QStringList lines;
    withSelectedTasks(mgr_, ids, [&](quint64, const TransferTask &task) {
        lines << task.src;
    });
    if (!lines.isEmpty())
        QGuiApplication::clipboard()->setText(lines.join("\n"));
}

void TransferQueueDialog::onCopyDestinationPath() {
    const auto ids = selectedTaskIds();
    if (ids.isEmpty())
        return;
    QStringList lines;
    withSelectedTasks(mgr_, ids, [&](quint64, const TransferTask &task) {
        lines << task.dst;
    });
    if (!lines.isEmpty())
        QGuiApplication::clipboard()->setText(lines.join("\n"));
}

void TransferQueueDialog::onClearFinished() {
    mgr_->clearCompleted();
    mgr_->clearFailedCanceled();
}

void TransferQueueDialog::onClearFailedCanceled() {
    mgr_->clearFailedCanceled();
}

void TransferQueueDialog::onAutoClearChanged() {
    if (suppressAutoClearSignal_)
        return;
    saveUiState();
    updateSummary();
}

void TransferQueueDialog::onOpenQueueOptions() {
    if (!queueOptionsDialog_ || !speedSpin_)
        return;
    speedSpin_->setValue(mgr_->globalSpeedLimitKBps());
    queueOptionsDialog_->open();
}

void TransferQueueDialog::updateSummary() {
    if (!model_)
        return;

    // Summary counts drive both badges and action enablement.
    const auto &tasks = model_->tasks();
    int queued = 0, running = 0, paused = 0, waiting = 0, retrying = 0;
    int done = 0, skipped = 0, error = 0, warning = 0, canceled = 0;
    int retryable = 0;
    for (const auto &task : tasks) {
        if (canRetryTask(task))
            ++retryable;
        switch (task.status) {
        case TransferTask::Status::Queued:
            ++queued;
            break;
        case TransferTask::Status::Running:
            ++running;
            break;
        case TransferTask::Status::Paused:
            ++paused;
            break;
        case TransferTask::Status::Done:
            ++done;
            break;
        case TransferTask::Status::Error:
            ++error;
            break;
        case TransferTask::Status::Canceled:
            ++canceled;
            break;
        case TransferTask::Status::WaitingForConnection:
            ++waiting;
            break;
        case TransferTask::Status::RetryWaiting:
            ++retrying;
            break;
        case TransferTask::Status::Skipped:
            ++skipped;
            break;
        case TransferTask::Status::Warning:
            ++warning;
            break;
        }
    }

    const int active = queued + running + paused + waiting + retrying;
    if (emptyStateLabel_) {
        const bool hasVisibleTasks = proxy_ && proxy_->rowCount() > 0;
        const QString emptyText =
            tasks.isEmpty() ? tr("No transfers in the queue\nTransfers "
                                 "will appear here when they start.")
                            : tr("No transfers match this filter");
        emptyStateLabel_->setText(emptyText);
        emptyStateLabel_->setAccessibleName(emptyText);
        emptyStateLabel_->setVisible(!hasVisibleTasks);
    }
    if (badgeTotal_)
        badgeTotal_->setText(tr("Total: %1").arg(tasks.size()));
    if (badgeActive_)
        badgeActive_->setText(tr("Active: %1").arg(active));
    if (badgeRunning_)
        badgeRunning_->setText(tr("Running: %1").arg(running));
    if (badgePaused_)
        badgePaused_->setText(tr("Paused: %1").arg(paused));
    if (badgeErrors_)
        badgeErrors_->setText(tr("Errors: %1").arg(error + warning));
    if (badgeCompleted_)
        badgeCompleted_->setText(tr("Completed: %1").arg(done + skipped));
    if (badgeCanceled_)
        badgeCanceled_->setText(tr("Canceled: %1").arg(canceled));
    if (badgeParallel_)
        badgeParallel_->setText(tr("Parallel: %1").arg(mgr_->maxConcurrent()));

    const int gkb = mgr_->globalSpeedLimitKBps();
    if (badgeLimit_) {
        badgeLimit_->setText(gkb > 0 ? tr("Global limit: %1 KB/s").arg(gkb)
                                     : tr("Global limit: off"));
    }

    const bool hasAny = !tasks.isEmpty();
    const bool queuePaused = mgr_ && mgr_->isQueuePaused();
    const bool canPause =
        !queuePaused && (queued + running + retrying + waiting) > 0;
    const bool canResume = queuePaused || paused + waiting > 0;
    const bool canRetry = retryable > 0;
    const bool canClearDone = (done + skipped) > 0;
    const bool canClearFailed = (error + warning + canceled) > 0;
    const bool canCancelAll = active > 0;
    const auto selectedState =
        buildSelectedActionsState(selectedTaskIds(), buildTaskIndexById(tasks));

    if (pauseBtn_)
        pauseBtn_->setEnabled(hasAny && canPause);
    if (resumeBtn_)
        resumeBtn_->setEnabled(hasAny && canResume);
    if (retryBtn_)
        retryBtn_->setEnabled(hasAny && canRetry);
    if (clearCompletedAction_)
        clearCompletedAction_->setEnabled(hasAny && canClearDone);
    if (clearFailedAction_)
        clearFailedAction_->setEnabled(hasAny && canClearFailed);
    if (clearMenuBtn_)
        clearMenuBtn_->setEnabled(canClearDone || canClearFailed);
    if (pauseSelBtn_)
        pauseSelBtn_->setEnabled(selectedState.canPause);
    if (resumeSelBtn_)
        resumeSelBtn_->setEnabled(selectedState.canResume);
    if (limitSelBtn_)
        limitSelBtn_->setEnabled(selectedState.canLimit);
    if (stopSelBtn_)
        stopSelBtn_->setEnabled(selectedState.canCancel);
    if (stopAllBtn_)
        stopAllBtn_->setEnabled(hasAny && canCancelAll);

    const int autoMode = autoClearModeCombo_
                             ? autoClearModeCombo_->currentData().toInt()
                             : AutoClearOff;
    if (autoClearMinutesSpin_)
        autoClearMinutesSpin_->setEnabled(autoMode != AutoClearOff);
}

QVector<quint64> TransferQueueDialog::selectedTaskIds() const {
    QVector<quint64> ids;
    if (!table_ || !table_->selectionModel())
        return ids;

    const auto rows = table_->selectionModel()->selectedRows();
    ids.reserve(rows.size());
    for (const QModelIndex &idx : rows) {
        const QVariant raw = idx.data(TransferTaskTableModel::TaskIdRole);
        bool conversionOk = false;
        const quint64 taskId = raw.toULongLong(&conversionOk);
        if (conversionOk)
            ids.push_back(taskId);
    }
    return ids;
}

void TransferQueueDialog::showContextMenu(const QPoint &pos) {
    QModelIndex clickedIndex = table_->indexAt(pos);
    QPoint menuPosition = pos;
    // Keyboard context-menu requests may not carry a viewport position. Use
    // the current row so Menu/Shift+F10 exposes the same actions as a click.
    if (!clickedIndex.isValid() && table_->currentIndex().isValid()) {
        clickedIndex = table_->currentIndex();
        menuPosition = table_->visualRect(clickedIndex).center();
    }
    if (!clickedIndex.isValid())
        return;

    if (!table_->selectionModel()->isSelected(clickedIndex)) {
        table_->clearSelection();
        table_->selectRow(clickedIndex.row());
    }

    const auto ids = selectedTaskIds();
    const auto snapshot = mgr_->tasksSnapshot(ids);
    const auto selectedState =
        buildSelectedActionsState(ids, buildTaskIndexById(snapshot));

    QMenu menu(this);
    QAction *actPauseSel = menu.addAction(tr("Pause selected"));
    QAction *actResumeSel = menu.addAction(tr("Resume selected"));
    QAction *actLimitSel = menu.addAction(tr("Limit selected"));
    QAction *actCancelSel = menu.addAction(tr("Cancel selected"));
    QAction *actRetrySel = menu.addAction(tr("Retry selected"));
    menu.addSeparator();
    QAction *actShowDest = menu.addAction(tr("Show folder"));
    QAction *actCopySrc = menu.addAction(tr("Copy source path"));
    QAction *actCopyDst = menu.addAction(tr("Copy destination path"));
    menu.addSeparator();
    QAction *actClearFinished = menu.addAction(tr("Clear finished"));
    QAction *actRemove = menu.addAction(tr("Remove selected tasks"));
    QAction *actRemoveWithPartial =
        menu.addAction(tr("Remove tasks and partial data"));

    actPauseSel->setEnabled(selectedState.canPause);
    actResumeSel->setEnabled(selectedState.canResume);
    actLimitSel->setEnabled(selectedState.canLimit);
    actCancelSel->setEnabled(selectedState.canCancel);
    actRetrySel->setEnabled(selectedState.canRetry);
    actShowDest->setEnabled(selectedState.canShowDestination);
    actCopySrc->setEnabled(selectedState.hasSelection);
    actCopyDst->setEnabled(selectedState.hasSelection);
    actClearFinished->setEnabled(true);
    actRemove->setEnabled(selectedState.hasSelection);
    actRemoveWithPartial->setEnabled(selectedState.hasSelection);

    QAction *chosen = menu.exec(table_->viewport()->mapToGlobal(menuPosition));
    if (!chosen)
        return;
    if (chosen == actPauseSel)
        onPauseSelected();
    else if (chosen == actResumeSel)
        onResumeSelected();
    else if (chosen == actLimitSel)
        onLimitSelected();
    else if (chosen == actCancelSel)
        onStopSelected();
    else if (chosen == actRetrySel)
        onRetrySelected();
    else if (chosen == actShowDest)
        onShowDestinationFolder();
    else if (chosen == actCopySrc)
        onCopySourcePath();
    else if (chosen == actCopyDst)
        onCopyDestinationPath();
    else if (chosen == actClearFinished)
        onClearFinished();
    else if (chosen == actRemove)
        mgr_->removeTasks(ids, false);
    else if (chosen == actRemoveWithPartial)
        mgr_->removeTasks(ids, true);
}

void TransferQueueDialog::loadUiState() {
    openscpui::AppSettings settings;

    const QByteArray geom =
        settings.value(openscpui::settingskeys::kTransferQueueGeometry)
            .toByteArray();
    if (!geom.isEmpty())
        restoreGeometry(geom);

    if (table_ && table_->horizontalHeader()) {
        const QByteArray header =
            settings.value(openscpui::settingskeys::kTransferQueueHeaderState)
                .toByteArray();
        if (!header.isEmpty())
            table_->horizontalHeader()->restoreState(header);
        table_->horizontalHeader()->setSectionResizeMode(
            TransferTaskTableModel::ColProgress, QHeaderView::Fixed);
        table_->setColumnWidth(TransferTaskTableModel::ColProgress,
                               kProgressColumnWidthPx);
    }

    const int filter =
        settings
            .value(openscpui::settingskeys::kTransferQueueFilterMode, FilterAll)
            .toInt();
    if (filterGroup_ && filterGroup_->button(filter)) {
        filterGroup_->button(filter)->setChecked(true);
        if (proxy_)
            proxy_->setFilterMode(filter);
    }

    suppressAutoClearSignal_ = true;
    const int defaultAutoMode = qBound(
        static_cast<int>(AutoClearOff),
        settings
            .value(openscpui::settingskeys::kTransferDefaultQueueAutoClearMode,
                   static_cast<int>(AutoClearOff))
            .toInt(),
        static_cast<int>(AutoClearFinished));
    const int defaultAutoMin = qBound(
        1,
        settings
            .value(
                openscpui::settingskeys::kTransferDefaultQueueAutoClearMinutes,
                15)
            .toInt(),
        1440);
    const int autoMode =
        settings
            .value(openscpui::settingskeys::kTransferQueueAutoClearMode,
                   defaultAutoMode)
            .toInt();
    const int autoMin =
        settings
            .value(openscpui::settingskeys::kTransferQueueAutoClearMinutes,
                   defaultAutoMin)
            .toInt();
    if (autoClearModeCombo_) {
        int modeIndex = autoClearModeCombo_->findData(autoMode);
        if (modeIndex < 0)
            modeIndex = autoClearModeCombo_->findData(AutoClearOff);
        autoClearModeCombo_->setCurrentIndex(modeIndex);
    }
    if (autoClearMinutesSpin_)
        autoClearMinutesSpin_->setValue(qBound(1, autoMin, 1440));
    suppressAutoClearSignal_ = false;
}

void TransferQueueDialog::saveUiState() const {
    openscpui::AppSettings settings;
    settings.setValue(openscpui::settingskeys::kTransferQueueGeometry,
                      saveGeometry());

    if (table_ && table_->horizontalHeader()) {
        settings.setValue(openscpui::settingskeys::kTransferQueueHeaderState,
                          table_->horizontalHeader()->saveState());
    }

    int filterMode = FilterAll;
    if (filterGroup_ && filterGroup_->checkedButton()) {
        filterMode = filterGroup_->id(filterGroup_->checkedButton());
    }
    settings.setValue(openscpui::settingskeys::kTransferQueueFilterMode,
                      filterMode);

    const int autoMode = autoClearModeCombo_
                             ? autoClearModeCombo_->currentData().toInt()
                             : AutoClearOff;
    const int autoMin =
        autoClearMinutesSpin_ ? autoClearMinutesSpin_->value() : 15;
    settings.setValue(openscpui::settingskeys::kTransferQueueAutoClearMode,
                      autoMode);
    settings.setValue(openscpui::settingskeys::kTransferQueueAutoClearMinutes,
                      autoMin);
    settings.sync();
}

void TransferQueueDialog::maybeAutoClear(
    const QVector<TransferTask> &snapshot) {
    if (!autoClearModeCombo_ || !autoClearMinutesSpin_)
        return;

    const int mode = autoClearModeCombo_->currentData().toInt();
    if (mode == AutoClearOff)
        return;

    const int minutes = autoClearMinutesSpin_->value();
    if (minutes <= 0)
        return;

    const qint64 cutoff =
        QDateTime::currentMSecsSinceEpoch() - qint64(minutes) * 60 * 1000;
    bool needsCleanup = false;
    // Quick pre-scan avoids manager calls when nothing is eligible yet.
    for (const auto &task : snapshot) {
        const bool isDone = task.status == TransferTask::Status::Done ||
                            task.status == TransferTask::Status::Skipped;
        const bool isFailed = (task.status == TransferTask::Status::Error ||
                               task.status == TransferTask::Status::Canceled ||
                               task.status == TransferTask::Status::Warning);
        bool candidate = false;
        if (mode == AutoClearCompleted)
            candidate = isDone;
        else if (mode == AutoClearFailedCanceled)
            candidate = isFailed;
        else if (mode == AutoClearFinished)
            candidate = (isDone || isFailed);

        if (candidate && task.finishedAtMs > 0 && task.finishedAtMs <= cutoff) {
            needsCleanup = true;
            break;
        }
    }
    if (!needsCleanup)
        return;

    if (mode == AutoClearCompleted)
        mgr_->clearFinishedOlderThan(minutes, true, false);
    else if (mode == AutoClearFailedCanceled)
        mgr_->clearFinishedOlderThan(minutes, false, true);
    else if (mode == AutoClearFinished)
        mgr_->clearFinishedOlderThan(minutes, true, true);
}
