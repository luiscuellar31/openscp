// Table with per-task state and actions (pause/resume/retry/clear).
#include "TransferQueueDialog.hpp"
#include <QAbstractItemView>
#include <QAbstractTableModel>
#include <QApplication>
#include <QButtonGroup>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QSet>
#include <QSettings>
#include <QSortFilterProxyModel>
#include <QSpinBox>
#include <QTableView>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

static constexpr int kProgressColumnWidthPx = 84;

static QString statusText(TransferTask::Status s) {
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
    }
    return {};
}

static QString displayNameForTask(const TransferTask &t) {
    const QString path = (t.type == TransferTask::Type::Upload) ? t.src : t.dst;
    QFileInfo fi(path);
    if (!fi.fileName().isEmpty())
        return fi.fileName();

    const QString alt = (t.type == TransferTask::Type::Upload) ? t.dst : t.src;
    QString trimmed = alt;
    while (trimmed.endsWith('/'))
        trimmed.chop(1);
    const int slash = trimmed.lastIndexOf('/');
    if (slash >= 0 && slash + 1 < trimmed.size())
        return trimmed.mid(slash + 1);
    if (!trimmed.isEmpty())
        return trimmed;
    return TransferQueueDialog::tr("(unnamed)");
}

static QString formatBytes(quint64 bytes) {
    static const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    const int precision = (value < 10.0 && unit > 0) ? 1 : 0;
    return QString::number(value, 'f', precision) + " " + units[unit];
}

static QString formatSpeed(double kbps) {
    if (kbps <= 0.0)
        return QString::fromUtf8("—");
    const double bps = kbps * 1024.0;
    return formatBytes(static_cast<quint64>(bps)) + "/s";
}

static QString formatEta(int sec) {
    if (sec < 0)
        return QString::fromUtf8("—");
    const int h = sec / 3600;
    const int m = (sec % 3600) / 60;
    const int s = sec % 60;
    if (h > 0)
        return QString("%1h %2m").arg(h).arg(m, 2, 10, QChar('0'));
    if (m > 0)
        return QString("%1m %2s").arg(m).arg(s, 2, 10, QChar('0'));
    return QString("%1s").arg(s);
}

static bool canPauseStatus(TransferTask::Status s) {
    return s == TransferTask::Status::Queued ||
           s == TransferTask::Status::Running;
}

static bool canResumeStatus(TransferTask::Status s) {
    return s == TransferTask::Status::Paused;
}

static bool canLimitStatus(TransferTask::Status s) {
    return s == TransferTask::Status::Queued ||
           s == TransferTask::Status::Running ||
           s == TransferTask::Status::Paused;
}

static bool canCancelStatus(TransferTask::Status s) {
    return s == TransferTask::Status::Queued ||
           s == TransferTask::Status::Running ||
           s == TransferTask::Status::Paused;
}

static bool canRetryStatus(TransferTask::Status s) {
    return s == TransferTask::Status::Error ||
           s == TransferTask::Status::Canceled;
}

struct SelectedActionsState {
    bool hasSelection = false;
    bool canPause = false;
    bool canResume = false;
    bool canLimit = false;
    bool canCancel = false;
    bool canRetry = false;
    bool canOpenDestination = false;
};

static SelectedActionsState
buildSelectedActionsState(const QVector<quint64> &ids,
                          const QVector<TransferTask> &snapshot) {
    SelectedActionsState out;
    out.hasSelection = !ids.isEmpty();
    if (!out.hasSelection)
        return out;

    for (quint64 id : ids) {
        for (const auto &t : snapshot) {
            if (t.id != id)
                continue;
            out.canPause = out.canPause || canPauseStatus(t.status);
            out.canResume = out.canResume || canResumeStatus(t.status);
            out.canLimit = out.canLimit || canLimitStatus(t.status);
            out.canCancel = out.canCancel || canCancelStatus(t.status);
            out.canRetry = out.canRetry || canRetryStatus(t.status);
            out.canOpenDestination =
                out.canOpenDestination ||
                (t.type == TransferTask::Type::Download);
            break;
        }
    }
    return out;
}

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
        return tasks_.size();
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
        const auto &t = tasks_[index.row()];

        if (role == StatusRole)
            return static_cast<int>(t.status);
        if (role == TaskIdRole)
            return static_cast<qulonglong>(t.id);
        if (role == ProgressRole)
            return t.progress;
        if (role == TypeRole)
            return static_cast<int>(t.type);
        if (role == SourceRole)
            return t.src;
        if (role == DestinationRole)
            return t.dst;

        if (role == Qt::TextAlignmentRole) {
            if (index.column() == ColStatus || index.column() == ColProgress ||
                index.column() == ColTransferred ||
                index.column() == ColSpeed || index.column() == ColEta ||
                index.column() == ColType ||
                index.column() == ColAttempts) {
                return static_cast<int>(Qt::AlignCenter);
            }
            return static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter);
        }

        if (role == Qt::ToolTipRole) {
            if (index.column() == ColSource)
                return t.src;
            if (index.column() == ColDestination)
                return t.dst;
            if (index.column() == ColError && !t.error.isEmpty())
                return t.error;
            if (index.column() == ColName)
                return displayNameForTask(t);
        }

        if (role != Qt::DisplayRole)
            return {};

        switch (index.column()) {
        case ColName:
            return displayNameForTask(t);
        case ColStatus:
            return statusText(t.status);
        case ColProgress:
            return QString::number(t.progress) + "%";
        case ColTransferred:
            if (t.bytesTotal > 0)
                return QString("%1 / %2").arg(formatBytes(t.bytesDone),
                                              formatBytes(t.bytesTotal));
            return formatBytes(t.bytesDone);
        case ColSpeed:
            return formatSpeed(t.currentSpeedKBps);
        case ColEta:
            return formatEta(t.etaSeconds);
        case ColType:
            return t.type == TransferTask::Type::Upload
                       ? TransferQueueDialog::tr("Upload")
                       : TransferQueueDialog::tr("Download");
        case ColSource:
            return t.src;
        case ColDestination:
            return t.dst;
        case ColAttempts:
            return QString("%1/%2").arg(t.attempts).arg(t.maxAttempts);
        case ColError:
            return t.error;
        default:
            return {};
        }
    }

    void sync(const QVector<TransferTask> &incoming) {
        if (tasks_.size() != incoming.size()) {
            beginResetModel();
            tasks_ = incoming;
            endResetModel();
            return;
        }

        bool orderChanged = false;
        for (int i = 0; i < tasks_.size(); ++i) {
            if (tasks_[i].id != incoming[i].id) {
                orderChanged = true;
                break;
            }
        }
        if (orderChanged) {
            beginResetModel();
            tasks_ = incoming;
            endResetModel();
            return;
        }

        for (int row = 0; row < tasks_.size(); ++row) {
            const auto &prev = tasks_[row];
            const auto &next = incoming[row];
            const bool changed =
                prev.type != next.type || prev.src != next.src ||
                prev.dst != next.dst || prev.status != next.status ||
                prev.progress != next.progress ||
                prev.bytesDone != next.bytesDone ||
                prev.bytesTotal != next.bytesTotal ||
                prev.currentSpeedKBps != next.currentSpeedKBps ||
                prev.etaSeconds != next.etaSeconds ||
                prev.attempts != next.attempts ||
                prev.maxAttempts != next.maxAttempts ||
                prev.error != next.error;
            tasks_[row] = next;
            if (changed) {
                emit dataChanged(index(row, 0), index(row, ColCount - 1),
                                 {Qt::DisplayRole, Qt::TextAlignmentRole,
                                  Qt::ToolTipRole, StatusRole, TaskIdRole,
                                  ProgressRole, TypeRole, SourceRole,
                                  DestinationRole});
            }
        }
    }

    const QVector<TransferTask> &tasks() const { return tasks_; }

    private:
    QVector<TransferTask> tasks_;
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
                   status == TransferTask::Status::Paused;
        case 2: // Errors
            return status == TransferTask::Status::Error;
        case 3: // Completed
            return status == TransferTask::Status::Done;
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
    auto *hf = new QHBoxLayout(filters);
    hf->setContentsMargins(0, 0, 0, 0);
    hf->setSpacing(6);
    hf->addWidget(new QLabel(tr("Show:"), filters));
    auto makeChip = [filters](const QString &text) {
        auto *b = new QToolButton(filters);
        b->setText(text);
        b->setCheckable(true);
        b->setToolButtonStyle(Qt::ToolButtonTextOnly);
        return b;
    };
    filterAllBtn_ = makeChip(tr("All"));
    filterActiveBtn_ = makeChip(tr("Active"));
    filterErrorsBtn_ = makeChip(tr("Errors"));
    filterCompletedBtn_ = makeChip(tr("Completed"));
    filterCanceledBtn_ = makeChip(tr("Canceled"));
    filterGroup_ = new QButtonGroup(this);
    filterGroup_->setExclusive(true);
    filterGroup_->addButton(filterAllBtn_, FilterAll);
    filterGroup_->addButton(filterActiveBtn_, FilterActive);
    filterGroup_->addButton(filterErrorsBtn_, FilterErrors);
    filterGroup_->addButton(filterCompletedBtn_, FilterCompleted);
    filterGroup_->addButton(filterCanceledBtn_, FilterCanceled);
    filterAllBtn_->setChecked(true);
    hf->addWidget(filterAllBtn_);
    hf->addWidget(filterActiveBtn_);
    hf->addWidget(filterErrorsBtn_);
    hf->addWidget(filterCompletedBtn_);
    hf->addWidget(filterCanceledBtn_);
    hf->addStretch();
    lay->addWidget(filters);

    // Row 2: table
    model_ = new TransferTaskTableModel(this);
    proxy_ = new TransferTaskFilterProxyModel(this);
    proxy_->setSourceModel(model_);

    table_ = new QTableView(this);
    table_->setModel(proxy_);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setAlternatingRowColors(true);
    table_->setStyleSheet("QTableView::item:focus { outline: none; }");
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
    const QList<int> desiredOrder = {
        TransferTaskTableModel::ColName,
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

    // Row 3: summary badges
    auto *badges = new QWidget(this);
    auto *hbBadges = new QHBoxLayout(badges);
    hbBadges->setContentsMargins(0, 0, 0, 0);
    hbBadges->setSpacing(6);
    auto makeBadge = [badges](const QString &text, const QString &color) {
        auto *lb = new QLabel(text, badges);
        lb->setStyleSheet(
            QString("QLabel{border:1px solid %1;border-radius:10px;padding:3px "
                    "8px;background:palette(base);}")
                .arg(color));
        return lb;
    };
    badgeTotal_ = makeBadge(tr("Total: 0"), "#607D8B");
    badgeActive_ = makeBadge(tr("Active: 0"), "#2E7D32");
    badgeRunning_ = makeBadge(tr("Running: 0"), "#0277BD");
    badgePaused_ = makeBadge(tr("Paused: 0"), "#795548");
    badgeErrors_ = makeBadge(tr("Errors: 0"), "#C62828");
    badgeCompleted_ = makeBadge(tr("Completed: 0"), "#2E7D32");
    badgeCanceled_ = makeBadge(tr("Canceled: 0"), "#5D4037");
    badgeLimit_ = makeBadge(tr("Global limit: off"), "#616161");
    hbBadges->addWidget(badgeTotal_);
    hbBadges->addWidget(badgeActive_);
    hbBadges->addWidget(badgeRunning_);
    hbBadges->addWidget(badgePaused_);
    hbBadges->addWidget(badgeErrors_);
    hbBadges->addWidget(badgeCompleted_);
    hbBadges->addWidget(badgeCanceled_);
    hbBadges->addStretch();
    hbBadges->addWidget(badgeLimit_);
    lay->addWidget(badges);

    // Row 4: controls
    auto *controls = new QWidget(this);
    auto *hb = new QHBoxLayout(controls);
    hb->setContentsMargins(0, 0, 0, 0);

    pauseBtn_ = new QPushButton(tr("Pause"), controls);
    resumeBtn_ = new QPushButton(tr("Resume"), controls);
    pauseSelBtn_ = new QPushButton(tr("Pause selected"), controls);
    resumeSelBtn_ = new QPushButton(tr("Resume selected"), controls);
    stopSelBtn_ = new QPushButton(tr("Cancel selected"), controls);
    stopAllBtn_ = new QPushButton(tr("Cancel all"), controls);
    retryBtn_ = new QPushButton(tr("Retry"), controls);
    clearBtn_ = new QPushButton(tr("Clear completed"), controls);
    clearFailedBtn_ = new QPushButton(tr("Clear failed/canceled"), controls);
    closeBtn_ = new QPushButton(tr("Close"), controls);

    pauseBtn_->setToolTip(tr("Pause all queued and running transfers"));
    resumeBtn_->setToolTip(tr("Resume the paused queue and paused tasks"));
    pauseSelBtn_->setToolTip(tr("Pause the selected transfers"));
    resumeSelBtn_->setToolTip(tr("Resume the selected transfers"));
    stopSelBtn_->setToolTip(tr("Cancel the selected transfers"));
    stopAllBtn_->setToolTip(
        tr("Cancel all queued, running, and paused transfers"));
    retryBtn_->setToolTip(tr("Retry transfers with Error or Canceled status"));
    clearBtn_->setToolTip(tr("Remove completed transfers from the list"));
    clearFailedBtn_->setToolTip(
        tr("Remove failed and canceled transfers from the list"));

    hb->addWidget(pauseBtn_);
    hb->addWidget(resumeBtn_);
    hb->addWidget(pauseSelBtn_);
    hb->addWidget(resumeSelBtn_);
    hb->addWidget(stopAllBtn_);
    hb->addWidget(stopSelBtn_);
    hb->addWidget(retryBtn_);
    hb->addWidget(clearBtn_);
    hb->addWidget(clearFailedBtn_);
    hb->addWidget(closeBtn_);
    hb->addStretch();
    lay->addWidget(controls);

    // Row 5: limits + auto clear
    auto *speedRow = new QWidget(this);
    auto *hs2 = new QHBoxLayout(speedRow);
    hs2->setContentsMargins(0, 0, 0, 0);

    speedSpin_ = new QSpinBox(speedRow);
    speedSpin_->setRange(0, 1'000'000);
    speedSpin_->setValue(mgr_->globalSpeedLimitKBps());
    speedSpin_->setSuffix(" KB/s");
    applySpeedBtn_ = new QPushButton(tr("Apply limit"), speedRow);
    limitSelBtn_ = new QPushButton(tr("Limit selected"), speedRow);

    autoClearModeCombo_ = new QComboBox(speedRow);
    autoClearModeCombo_->addItem(tr("Off"), AutoClearOff);
    autoClearModeCombo_->addItem(tr("Completed"), AutoClearCompleted);
    autoClearModeCombo_->addItem(tr("Failed/Canceled"),
                                 AutoClearFailedCanceled);
    autoClearModeCombo_->addItem(tr("All finished"), AutoClearFinished);
    autoClearMinutesSpin_ = new QSpinBox(speedRow);
    autoClearMinutesSpin_->setRange(1, 1440);
    autoClearMinutesSpin_->setSuffix(tr(" min"));

    hs2->addWidget(new QLabel(tr("Speed:"), speedRow));
    hs2->addWidget(speedSpin_);
    hs2->addWidget(applySpeedBtn_);
    hs2->addWidget(limitSelBtn_);
    hs2->addSpacing(16);
    hs2->addWidget(new QLabel(tr("Auto clear:"), speedRow));
    hs2->addWidget(autoClearModeCombo_);
    hs2->addWidget(autoClearMinutesSpin_);
    hs2->addStretch();
    lay->addWidget(speedRow);

    // Connections
    connect(applySpeedBtn_, &QPushButton::clicked, this,
            &TransferQueueDialog::onApplyGlobalSpeed);
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
    connect(clearBtn_, &QPushButton::clicked, this,
            &TransferQueueDialog::onClearDone);
    connect(clearFailedBtn_, &QPushButton::clicked, this,
            &TransferQueueDialog::onClearFailedCanceled);
    connect(closeBtn_, &QPushButton::clicked, this, &QDialog::reject);
    connect(filterGroup_, &QButtonGroup::idClicked, this,
            &TransferQueueDialog::onFilterChanged);
    connect(autoClearModeCombo_, &QComboBox::currentIndexChanged, this,
            &TransferQueueDialog::onAutoClearChanged);
    connect(autoClearMinutesSpin_, &QSpinBox::valueChanged, this,
            &TransferQueueDialog::onAutoClearChanged);

    connect(mgr_, &TransferManager::tasksChanged, this,
            &TransferQueueDialog::refresh);
    connect(table_->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, &TransferQueueDialog::updateSummary);
    connect(table_, &QTableView::customContextMenuRequested, this,
            &TransferQueueDialog::showContextMenu);
    connect(this, &QDialog::finished, this, [this] { saveUiState(); });

    loadUiState();
    refresh();
}

void TransferQueueDialog::refresh() {
    if (!model_)
        return;
    const auto snapshot = mgr_->tasksSnapshot();
    model_->sync(snapshot);
    maybeAutoClear(snapshot);
    updateSummary();
}

void TransferQueueDialog::onPause() { mgr_->pauseAll(); }
void TransferQueueDialog::onResume() { mgr_->resumeAll(); }
void TransferQueueDialog::onRetry() { mgr_->retryFailed(); }
void TransferQueueDialog::onClearDone() { mgr_->clearCompleted(); }

void TransferQueueDialog::onPauseSelected() {
    const auto ids = selectedTaskIds();
    if (ids.isEmpty())
        return;
    const auto snapshot = mgr_->tasksSnapshot();
    for (quint64 id : ids) {
        for (const auto &t : snapshot) {
            if (t.id != id)
                continue;
            if (canPauseStatus(t.status))
                mgr_->pauseTask(id);
            break;
        }
    }
}

void TransferQueueDialog::onResumeSelected() {
    const auto ids = selectedTaskIds();
    if (ids.isEmpty())
        return;
    const auto snapshot = mgr_->tasksSnapshot();
    for (quint64 id : ids) {
        for (const auto &t : snapshot) {
            if (t.id != id)
                continue;
            if (canResumeStatus(t.status))
                mgr_->resumeTask(id);
            break;
        }
    }
}

void TransferQueueDialog::onApplyGlobalSpeed() {
    mgr_->setGlobalSpeedLimitKBps(speedSpin_->value());
    updateSummary();
}

void TransferQueueDialog::onLimitSelected() {
    const auto ids = selectedTaskIds();
    if (ids.isEmpty())
        return;
    const auto snapshot = mgr_->tasksSnapshot();
    QVector<quint64> eligible;
    eligible.reserve(ids.size());
    for (quint64 id : ids) {
        for (const auto &t : snapshot) {
            if (t.id != id)
                continue;
            if (canLimitStatus(t.status))
                eligible.push_back(id);
            break;
        }
    }
    if (eligible.isEmpty())
        return;

    bool ok = false;
    const int v = QInputDialog::getInt(this, tr("Limit for task(s)"),
                                       tr("KB/s (0 = no limit)"), 0, 0,
                                       1'000'000, 1, &ok);
    if (!ok)
        return;
    for (quint64 id : eligible)
        mgr_->setTaskSpeedLimit(id, v);
}

void TransferQueueDialog::onStopSelected() {
    const auto ids = selectedTaskIds();
    if (ids.isEmpty())
        return;
    const auto snapshot = mgr_->tasksSnapshot();
    for (quint64 id : ids) {
        for (const auto &t : snapshot) {
            if (t.id != id)
                continue;
            if (canCancelStatus(t.status))
                mgr_->cancelTask(id);
            break;
        }
    }
}

void TransferQueueDialog::onStopAll() { mgr_->cancelAll(); }

void TransferQueueDialog::onFilterChanged(int id) {
    if (!proxy_)
        return;
    proxy_->setFilterMode(id);
    if (table_)
        table_->clearSelection();
    updateSummary();
    saveUiState();
}

void TransferQueueDialog::onRetrySelected() {
    const auto ids = selectedTaskIds();
    if (ids.isEmpty())
        return;
    const auto snapshot = mgr_->tasksSnapshot();
    for (quint64 id : ids) {
        for (const auto &t : snapshot) {
            if (t.id != id)
                continue;
            if (canRetryStatus(t.status))
                mgr_->retryTask(id);
            break;
        }
    }
}

void TransferQueueDialog::onOpenDestination() {
    const auto ids = selectedTaskIds();
    if (ids.isEmpty())
        return;

    const auto snapshot = mgr_->tasksSnapshot();
    QSet<QString> opened;
    for (quint64 id : ids) {
        for (const auto &t : snapshot) {
            if (t.id != id)
                continue;
            if (t.type != TransferTask::Type::Download)
                break;

            QString path = t.dst;
            QFileInfo fi(path);
            if (!fi.exists()) {
                const QString parent = fi.dir().absolutePath();
                if (!parent.isEmpty() && QDir(parent).exists())
                    path = parent;
            }
            const QString normalized = QFileInfo(path).absoluteFilePath();
            if (normalized.isEmpty() || opened.contains(normalized))
                break;
            opened.insert(normalized);
            QDesktopServices::openUrl(QUrl::fromLocalFile(normalized));
            break;
        }
    }
}

void TransferQueueDialog::onCopySourcePath() {
    const auto ids = selectedTaskIds();
    if (ids.isEmpty())
        return;
    const auto snapshot = mgr_->tasksSnapshot();

    QStringList lines;
    for (quint64 id : ids) {
        for (const auto &t : snapshot) {
            if (t.id == id) {
                lines << t.src;
                break;
            }
        }
    }
    if (!lines.isEmpty())
        QGuiApplication::clipboard()->setText(lines.join("\n"));
}

void TransferQueueDialog::onCopyDestinationPath() {
    const auto ids = selectedTaskIds();
    if (ids.isEmpty())
        return;
    const auto snapshot = mgr_->tasksSnapshot();

    QStringList lines;
    for (quint64 id : ids) {
        for (const auto &t : snapshot) {
            if (t.id == id) {
                lines << t.dst;
                break;
            }
        }
    }
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

void TransferQueueDialog::updateSummary() {
    if (!model_)
        return;

    const auto &tasks = model_->tasks();
    int queued = 0, running = 0, paused = 0, done = 0, error = 0, canceled = 0;
    for (const auto &t : tasks) {
        switch (t.status) {
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
        }
    }

    const int active = queued + running + paused;
    if (badgeTotal_)
        badgeTotal_->setText(tr("Total: %1").arg(tasks.size()));
    if (badgeActive_)
        badgeActive_->setText(tr("Active: %1").arg(active));
    if (badgeRunning_)
        badgeRunning_->setText(tr("Running: %1").arg(running));
    if (badgePaused_)
        badgePaused_->setText(tr("Paused: %1").arg(paused));
    if (badgeErrors_)
        badgeErrors_->setText(tr("Errors: %1").arg(error));
    if (badgeCompleted_)
        badgeCompleted_->setText(tr("Completed: %1").arg(done));
    if (badgeCanceled_)
        badgeCanceled_->setText(tr("Canceled: %1").arg(canceled));

    const int gkb = mgr_->globalSpeedLimitKBps();
    if (badgeLimit_) {
        badgeLimit_->setText(gkb > 0 ? tr("Global limit: %1 KB/s").arg(gkb)
                                     : tr("Global limit: off"));
    }

    const bool hasAny = !tasks.isEmpty();
    const bool queuePaused = mgr_ && mgr_->isQueuePaused();
    const bool canPause = !queuePaused && (queued + running) > 0;
    const bool canResume = queuePaused || paused > 0;
    const bool canRetry = (error + canceled) > 0;
    const bool canClearDone = done > 0;
    const bool canClearFailed = (error + canceled) > 0;
    const bool canCancelAll = active > 0;
    const auto selectedState =
        buildSelectedActionsState(selectedTaskIds(), tasks);

    if (pauseBtn_)
        pauseBtn_->setEnabled(hasAny && canPause);
    if (resumeBtn_)
        resumeBtn_->setEnabled(hasAny && canResume);
    if (retryBtn_)
        retryBtn_->setEnabled(hasAny && canRetry);
    if (clearBtn_)
        clearBtn_->setEnabled(hasAny && canClearDone);
    if (clearFailedBtn_)
        clearFailedBtn_->setEnabled(hasAny && canClearFailed);
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
        bool ok = false;
        const quint64 id = raw.toULongLong(&ok);
        if (ok)
            ids.push_back(id);
    }
    return ids;
}

void TransferQueueDialog::showContextMenu(const QPoint &pos) {
    const QModelIndex idx = table_->indexAt(pos);
    if (!idx.isValid())
        return;

    if (!table_->selectionModel()->isSelected(idx)) {
        table_->clearSelection();
        table_->selectRow(idx.row());
    }

    const auto ids = selectedTaskIds();
    const auto snapshot = mgr_->tasksSnapshot();
    const auto selectedState = buildSelectedActionsState(ids, snapshot);

    QMenu menu(this);
    QAction *actPauseSel = menu.addAction(tr("Pause selected"));
    QAction *actResumeSel = menu.addAction(tr("Resume selected"));
    QAction *actLimitSel = menu.addAction(tr("Limit selected"));
    QAction *actCancelSel = menu.addAction(tr("Cancel selected"));
    QAction *actRetrySel = menu.addAction(tr("Retry selected"));
    menu.addSeparator();
    QAction *actOpenDest = menu.addAction(tr("Open destination"));
    QAction *actCopySrc = menu.addAction(tr("Copy source path"));
    QAction *actCopyDst = menu.addAction(tr("Copy destination path"));
    menu.addSeparator();
    QAction *actClearFinished = menu.addAction(tr("Clear finished"));

    actPauseSel->setEnabled(selectedState.canPause);
    actResumeSel->setEnabled(selectedState.canResume);
    actLimitSel->setEnabled(selectedState.canLimit);
    actCancelSel->setEnabled(selectedState.canCancel);
    actRetrySel->setEnabled(selectedState.canRetry);
    actOpenDest->setEnabled(selectedState.canOpenDestination);
    actCopySrc->setEnabled(selectedState.hasSelection);
    actCopyDst->setEnabled(selectedState.hasSelection);
    actClearFinished->setEnabled(true);

    QAction *chosen = menu.exec(table_->viewport()->mapToGlobal(pos));
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
    else if (chosen == actOpenDest)
        onOpenDestination();
    else if (chosen == actCopySrc)
        onCopySourcePath();
    else if (chosen == actCopyDst)
        onCopyDestinationPath();
    else if (chosen == actClearFinished)
        onClearFinished();
}

void TransferQueueDialog::loadUiState() {
    QSettings s("OpenSCP", "OpenSCP");

    const QByteArray geom = s.value("UI/transferQueue/geometry").toByteArray();
    if (!geom.isEmpty())
        restoreGeometry(geom);

    if (table_ && table_->horizontalHeader()) {
        const QByteArray header =
            s.value("UI/transferQueue/headerStateV4").toByteArray();
        if (!header.isEmpty())
            table_->horizontalHeader()->restoreState(header);
        table_->horizontalHeader()->setSectionResizeMode(
            TransferTaskTableModel::ColProgress, QHeaderView::Fixed);
        table_->setColumnWidth(TransferTaskTableModel::ColProgress,
                               kProgressColumnWidthPx);
    }

    const int filter =
        s.value("UI/transferQueue/filterMode", FilterAll).toInt();
    if (filterGroup_ && filterGroup_->button(filter)) {
        filterGroup_->button(filter)->setChecked(true);
        if (proxy_)
            proxy_->setFilterMode(filter);
    }

    suppressAutoClearSignal_ = true;
    const int autoMode =
        s.value("UI/transferQueue/autoClearMode", AutoClearOff).toInt();
    const int autoMin =
        s.value("UI/transferQueue/autoClearMinutes", 15).toInt();
    if (autoClearModeCombo_) {
        int idx = autoClearModeCombo_->findData(autoMode);
        if (idx < 0)
            idx = autoClearModeCombo_->findData(AutoClearOff);
        autoClearModeCombo_->setCurrentIndex(idx);
    }
    if (autoClearMinutesSpin_)
        autoClearMinutesSpin_->setValue(qBound(1, autoMin, 1440));
    suppressAutoClearSignal_ = false;
}

void TransferQueueDialog::saveUiState() const {
    QSettings s("OpenSCP", "OpenSCP");
    s.setValue("UI/transferQueue/geometry", saveGeometry());

    if (table_ && table_->horizontalHeader()) {
        s.setValue("UI/transferQueue/headerStateV4",
                   table_->horizontalHeader()->saveState());
    }

    int filterMode = FilterAll;
    if (filterGroup_ && filterGroup_->checkedButton()) {
        filterMode = filterGroup_->id(filterGroup_->checkedButton());
    }
    s.setValue("UI/transferQueue/filterMode", filterMode);

    const int autoMode = autoClearModeCombo_
                             ? autoClearModeCombo_->currentData().toInt()
                             : AutoClearOff;
    const int autoMin =
        autoClearMinutesSpin_ ? autoClearMinutesSpin_->value() : 15;
    s.setValue("UI/transferQueue/autoClearMode", autoMode);
    s.setValue("UI/transferQueue/autoClearMinutes", autoMin);
    s.sync();
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
    for (const auto &t : snapshot) {
        const bool isDone = t.status == TransferTask::Status::Done;
        const bool isFailed = (t.status == TransferTask::Status::Error ||
                               t.status == TransferTask::Status::Canceled);
        bool candidate = false;
        if (mode == AutoClearCompleted)
            candidate = isDone;
        else if (mode == AutoClearFailedCanceled)
            candidate = isFailed;
        else if (mode == AutoClearFinished)
            candidate = (isDone || isFailed);

        if (candidate && t.finishedAtMs > 0 && t.finishedAtMs <= cutoff) {
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

void TransferQueueDialog::closeEvent(QCloseEvent *e) {
    saveUiState();
    QDialog::closeEvent(e);
}
