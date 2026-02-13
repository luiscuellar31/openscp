// Table with per-task state and actions (pause/resume/retry/clear).
#include "TransferQueueDialog.hpp"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QHeaderView>
#include <QAbstractItemView>
#include <QTableView>
#include <QAbstractTableModel>
#include <QSortFilterProxyModel>
#include <QStyledItemDelegate>
#include <QStyleOptionProgressBar>
#include <QStyle>
#include <QApplication>
#include <QSpinBox>
#include <QButtonGroup>
#include <QToolButton>
#include <QInputDialog>
#include <QMenu>

static QString statusText(TransferTask::Status s) {
  switch (s) {
    case TransferTask::Status::Queued: return TransferQueueDialog::tr("Queued");
    case TransferTask::Status::Running: return TransferQueueDialog::tr("Running");
    case TransferTask::Status::Paused: return TransferQueueDialog::tr("Paused");
    case TransferTask::Status::Done: return TransferQueueDialog::tr("Completed");
    case TransferTask::Status::Error: return TransferQueueDialog::tr("Error");
    case TransferTask::Status::Canceled: return TransferQueueDialog::tr("Canceled");
  }
  return {};
}

class TransferTaskTableModel final : public QAbstractTableModel {
public:
  enum Roles {
    StatusRole = Qt::UserRole + 1,
    TaskIdRole,
    ProgressRole
  };

  explicit TransferTaskTableModel(QObject* parent = nullptr) : QAbstractTableModel(parent) {}

  int rowCount(const QModelIndex& parent = QModelIndex()) const override {
    if (parent.isValid()) return 0;
    return tasks_.size();
  }

  int columnCount(const QModelIndex& parent = QModelIndex()) const override {
    if (parent.isValid()) return 0;
    return 6;
  }

  QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
    if (role != Qt::DisplayRole) return {};
    if (orientation == Qt::Horizontal) {
      switch (section) {
        case 0: return TransferQueueDialog::tr("Type");
        case 1: return TransferQueueDialog::tr("Source");
        case 2: return TransferQueueDialog::tr("Destination");
        case 3: return TransferQueueDialog::tr("Status");
        case 4: return TransferQueueDialog::tr("Progress");
        case 5: return TransferQueueDialog::tr("Attempts");
        default: return {};
      }
    }
    return section + 1;
  }

  QVariant data(const QModelIndex& index, int role) const override {
    if (!index.isValid()) return {};
    if (index.row() < 0 || index.row() >= tasks_.size()) return {};
    const auto& t = tasks_[index.row()];

    if (role == StatusRole) return static_cast<int>(t.status);
    if (role == TaskIdRole) return static_cast<qulonglong>(t.id);
    if (role == ProgressRole) return t.progress;

    if (role == Qt::TextAlignmentRole) {
      if (index.column() == 0 || index.column() == 3 || index.column() == 4 || index.column() == 5) {
        return static_cast<int>(Qt::AlignCenter);
      }
      return static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter);
    }

    if (role == Qt::ToolTipRole) {
      if (index.column() == 1) return t.src;
      if (index.column() == 2) return t.dst;
      if (index.column() == 3 && t.status == TransferTask::Status::Error && !t.error.isEmpty()) {
        return TransferQueueDialog::tr("Error details: %1").arg(t.error);
      }
    }

    if (role != Qt::DisplayRole) return {};
    switch (index.column()) {
      case 0: return t.type == TransferTask::Type::Upload ? TransferQueueDialog::tr("Upload") : TransferQueueDialog::tr("Download");
      case 1: return t.src;
      case 2: return t.dst;
      case 3: return statusText(t.status);
      case 4: return QString::number(t.progress) + "%";
      case 5: return QString("%1/%2").arg(t.attempts).arg(t.maxAttempts);
      default: return {};
    }
  }

  void sync(const QVector<TransferTask>& incoming) {
    // Structural changes are uncommon; reset only when row count/order changed.
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
      const auto& prev = tasks_[row];
      const auto& next = incoming[row];
      const bool changed = prev.type != next.type ||
                           prev.src != next.src ||
                           prev.dst != next.dst ||
                           prev.status != next.status ||
                           prev.progress != next.progress ||
                           prev.attempts != next.attempts ||
                           prev.maxAttempts != next.maxAttempts;
      if (changed) {
        tasks_[row] = next;
        emit dataChanged(index(row, 0), index(row, 5),
                         {Qt::DisplayRole, Qt::TextAlignmentRole, Qt::ToolTipRole, StatusRole, ProgressRole});
      } else {
        // Keep non-rendered fields in sync for action helpers.
        tasks_[row] = next;
      }
    }
  }

  const QVector<TransferTask>& tasks() const { return tasks_; }

  std::optional<quint64> taskIdAtRow(int row) const {
    if (row < 0 || row >= tasks_.size()) return std::nullopt;
    return tasks_[row].id;
  }

private:
  QVector<TransferTask> tasks_;
};

class TransferTaskFilterProxyModel final : public QSortFilterProxyModel {
public:
  explicit TransferTaskFilterProxyModel(QObject* parent = nullptr) : QSortFilterProxyModel(parent) {
    setDynamicSortFilter(true);
  }

  void setFilterMode(int mode) {
    if (mode_ == mode) return;
    mode_ = mode;
    invalidate();
  }

protected:
  bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override {
    if (mode_ == 0) return true; // All
    const QModelIndex statusIdx = sourceModel()->index(sourceRow, 3, sourceParent);
    const QVariant raw = sourceModel()->data(statusIdx, TransferTaskTableModel::StatusRole);
    if (!raw.isValid()) return true;
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

class ProgressBarDelegate final : public QStyledItemDelegate {
public:
  explicit ProgressBarDelegate(QObject* parent = nullptr) : QStyledItemDelegate(parent) {}

  void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
    const int progress = qBound(0, index.data(TransferTaskTableModel::ProgressRole).toInt(), 100);

    QStyleOptionProgressBar bar;
    bar.rect = option.rect.adjusted(4, 4, -4, -4);
    bar.minimum = 0;
    bar.maximum = 100;
    bar.progress = progress;
    bar.text = QString::number(progress) + "%";
    bar.textVisible = true;
    bar.textAlignment = Qt::AlignCenter;
    bar.state = option.state;
    bar.direction = option.direction;
    bar.fontMetrics = option.fontMetrics;

    if (option.widget) {
      option.widget->style()->drawControl(QStyle::CE_ProgressBar, &bar, painter, option.widget);
    } else {
      QApplication::style()->drawControl(QStyle::CE_ProgressBar, &bar, painter, nullptr);
    }
  }
};

TransferQueueDialog::TransferQueueDialog(TransferManager* mgr, QWidget* parent)
  : QDialog(parent), mgr_(mgr) {
  setWindowTitle(tr("Transfer queue"));
  resize(760, 380);
  setSizeGripEnabled(true);

  auto* lay = new QVBoxLayout(this);

  // Tasks table
  model_ = new TransferTaskTableModel(this);
  proxy_ = new TransferTaskFilterProxyModel(this);
  proxy_->setSourceModel(model_);
  table_ = new QTableView(this);
  table_->setModel(proxy_);
  table_->verticalHeader()->setVisible(false);
  table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
  table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
  table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
  table_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
  table_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
  table_->setItemDelegateForColumn(4, new ProgressBarDelegate(table_));
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
  table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table_->setAlternatingRowColors(true);
  table_->setContextMenuPolicy(Qt::CustomContextMenu);
  lay->addWidget(table_);

  // Row 1: quick filters
  auto* filters = new QWidget(this);
  auto* hf = new QHBoxLayout(filters);
  hf->setContentsMargins(0, 0, 0, 0);
  hf->setSpacing(6);
  hf->addWidget(new QLabel(tr("Show:"), filters));
  auto makeChip = [filters](const QString& text) {
    auto* b = new QToolButton(filters);
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

  // Row 2: controls (buttons in a single row)
  auto* controls = new QWidget(this);
  auto* hb = new QHBoxLayout(controls);
  hb->setContentsMargins(0,0,0,0);

  pauseBtn_  = new QPushButton(tr("Pause"), controls);
  resumeBtn_ = new QPushButton(tr("Resume"), controls);
  pauseSelBtn_  = new QPushButton(tr("Pause selected"), controls);
  resumeSelBtn_ = new QPushButton(tr("Resume selected"), controls);
  stopSelBtn_   = new QPushButton(tr("Cancel selected"), controls);
  stopAllBtn_   = new QPushButton(tr("Cancel all"), controls);
  retryBtn_  = new QPushButton(tr("Retry"), controls);
  clearBtn_  = new QPushButton(tr("Clear completed"), controls);
  closeBtn_  = new QPushButton(tr("Close"), controls);

  pauseBtn_->setToolTip(tr("Pause all queued and running transfers"));
  resumeBtn_->setToolTip(tr("Resume the paused queue and paused tasks"));
  pauseSelBtn_->setToolTip(tr("Pause the selected transfers"));
  resumeSelBtn_->setToolTip(tr("Resume the selected transfers"));
  stopSelBtn_->setToolTip(tr("Cancel the selected transfers"));
  stopAllBtn_->setToolTip(tr("Cancel all queued, running, and paused transfers"));
  retryBtn_->setToolTip(tr("Retry transfers with Error or Canceled status"));
  clearBtn_->setToolTip(tr("Remove completed transfers from the list"));
  closeBtn_->setToolTip(tr("Close this window"));

  hb->addWidget(pauseBtn_);
  hb->addWidget(resumeBtn_);
  hb->addWidget(pauseSelBtn_);
  hb->addWidget(resumeSelBtn_);
  hb->addWidget(stopAllBtn_);
  hb->addWidget(stopSelBtn_);
  hb->addWidget(retryBtn_);
  hb->addWidget(clearBtn_);
  hb->addWidget(closeBtn_);
  hb->addStretch();
  lay->addWidget(controls);

  // Row 3: global speed setting
  auto* speedRow = new QWidget(this);
  auto* hs2 = new QHBoxLayout(speedRow);
  hs2->setContentsMargins(0,0,0,0);
  speedSpin_ = new QSpinBox(speedRow);
  speedSpin_->setRange(0, 1'000'000);
  speedSpin_->setValue(mgr_->globalSpeedLimitKBps());
  speedSpin_->setSuffix(" KB/s");
  applySpeedBtn_ = new QPushButton(tr("Apply limit"), speedRow);
  limitSelBtn_  = new QPushButton(tr("Limit selected"), speedRow);
  applySpeedBtn_->setToolTip(tr("Set a global speed limit for all transfers"));
  limitSelBtn_->setToolTip(tr("Set a speed limit for selected transfers"));
  hs2->addWidget(new QLabel(tr("Speed:"), speedRow));
  hs2->addWidget(speedSpin_);
  hs2->addWidget(applySpeedBtn_);
  hs2->addWidget(limitSelBtn_);
  hs2->addStretch();
  lay->addWidget(speedRow);

  // Summary row (bottom)
  auto* summary = new QWidget(this);
  auto* hs = new QHBoxLayout(summary);
  hs->setContentsMargins(0,0,0,0);
  summaryLabel_ = new QLabel(tr(""), summary);
  summaryLabel_->setWordWrap(true);
  hs->addWidget(summaryLabel_);
  lay->addWidget(summary);

  // Connections
  connect(applySpeedBtn_, &QPushButton::clicked, this, &TransferQueueDialog::onApplyGlobalSpeed);
  connect(pauseBtn_,  &QPushButton::clicked, this, &TransferQueueDialog::onPause);
  connect(resumeBtn_, &QPushButton::clicked, this, &TransferQueueDialog::onResume);
  connect(pauseSelBtn_, &QPushButton::clicked, this, &TransferQueueDialog::onPauseSelected);
  connect(resumeSelBtn_, &QPushButton::clicked, this, &TransferQueueDialog::onResumeSelected);
  connect(limitSelBtn_, &QPushButton::clicked, this, &TransferQueueDialog::onLimitSelected);
  connect(stopSelBtn_, &QPushButton::clicked, this, &TransferQueueDialog::onStopSelected);
  connect(stopAllBtn_, &QPushButton::clicked, this, &TransferQueueDialog::onStopAll);
  connect(retryBtn_,  &QPushButton::clicked, this, &TransferQueueDialog::onRetry);
  connect(clearBtn_,  &QPushButton::clicked, this, &TransferQueueDialog::onClearDone);
  connect(closeBtn_,  &QPushButton::clicked, this, &QDialog::reject);
  connect(filterGroup_, &QButtonGroup::idClicked, this, &TransferQueueDialog::onFilterChanged);

  connect(mgr_, &TransferManager::tasksChanged, this, &TransferQueueDialog::refresh);
  // Keep selection-dependent button enablement up to date
  connect(table_->selectionModel(), &QItemSelectionModel::selectionChanged, this, &TransferQueueDialog::updateSummary);
  connect(table_, &QTableView::customContextMenuRequested, this, &TransferQueueDialog::showContextMenu);
  refresh();
}

void TransferQueueDialog::refresh() {
  if (!model_) return;
  model_->sync(mgr_->tasksSnapshot());
  updateSummary();
}

void TransferQueueDialog::onPause() { mgr_->pauseAll(); }
void TransferQueueDialog::onResume(){ mgr_->resumeAll(); }
void TransferQueueDialog::onRetry() { mgr_->retryFailed(); }
void TransferQueueDialog::onClearDone() { mgr_->clearCompleted(); }

void TransferQueueDialog::onPauseSelected() {
  const auto ids = selectedTaskIds();
  for (quint64 id : ids) mgr_->pauseTask(id);
}
void TransferQueueDialog::onResumeSelected() {
  const auto ids = selectedTaskIds();
  for (quint64 id : ids) mgr_->resumeTask(id);
}
void TransferQueueDialog::onApplyGlobalSpeed() {
  mgr_->setGlobalSpeedLimitKBps(speedSpin_->value());
  updateSummary();
}
void TransferQueueDialog::onLimitSelected() {
  const auto ids = selectedTaskIds();
  if (ids.isEmpty()) return;
  bool ok=false; int v = QInputDialog::getInt(this, tr("Limit for task(s)"), tr("KB/s (0 = no limit)"), 0, 0, 1'000'000, 1, &ok);
  if (!ok) return;
  for (quint64 id : ids) mgr_->setTaskSpeedLimit(id, v);
}

void TransferQueueDialog::onStopSelected() {
  const auto ids = selectedTaskIds();
  for (quint64 id : ids) mgr_->cancelTask(id);
}

void TransferQueueDialog::onStopAll() {
  mgr_->cancelAll();
}

void TransferQueueDialog::updateSummary() {
  if (!model_) return;
  const auto& tasks = model_->tasks();
  int queued = 0, running = 0, paused = 0, done = 0, error = 0, canceled = 0;
  for (const auto& t : tasks) {
    switch (t.status) {
      case TransferTask::Status::Queued: queued++; break;
      case TransferTask::Status::Running: running++; break;
      case TransferTask::Status::Paused: paused++; break;
      case TransferTask::Status::Done: done++; break;
      case TransferTask::Status::Error: error++; break;
      case TransferTask::Status::Canceled: canceled++; break;
    }
  }
  QString summary = tr("Total: %1  |  Queued: %2  |  Running: %3  |  Paused: %4  |  Error: %5  |  Done: %6  |  Canceled: %7")
                    .arg(tasks.size())
                    .arg(queued)
                    .arg(running)
                    .arg(paused)
                    .arg(error)
                    .arg(done)
                    .arg(canceled);
  const int gkb = mgr_->globalSpeedLimitKBps();
  if (gkb > 0) {
    summary += tr("  |  Global limit: %1 KB/s").arg(gkb);
  }
  summaryLabel_->setText(summary);

  // Enable/Disable actions based on state
  const bool hasAny = !tasks.isEmpty();
  const bool queuePaused = mgr_ && mgr_->isQueuePaused();
  const bool canPause = !queuePaused && (queued + running) > 0;
  const bool canResume = queuePaused || paused > 0;
  const bool canRetry = (error + canceled) > 0; // there are failed/canceled
  const bool canClear = done > 0;               // there are completed to clear
  const bool canCancelAll = (queued + running + paused) > 0;
  const bool hasSel = !selectedTaskIds().isEmpty();

  if (pauseBtn_)  pauseBtn_->setEnabled(hasAny && canPause);
  if (resumeBtn_) resumeBtn_->setEnabled(hasAny && canResume);
  if (retryBtn_)  retryBtn_->setEnabled(hasAny && canRetry);
  if (clearBtn_)  clearBtn_->setEnabled(hasAny && canClear);
  if (pauseSelBtn_) pauseSelBtn_->setEnabled(hasSel);
  if (resumeSelBtn_) resumeSelBtn_->setEnabled(hasSel);
  if (limitSelBtn_) limitSelBtn_->setEnabled(hasSel);
  if (stopSelBtn_)  stopSelBtn_->setEnabled(hasSel);
  if (stopAllBtn_)  stopAllBtn_->setEnabled(hasAny && canCancelAll);
}

void TransferQueueDialog::onFilterChanged(int id) {
  if (!proxy_) return;
  proxy_->setFilterMode(id);
  if (table_) table_->clearSelection();
  updateSummary();
}

QVector<quint64> TransferQueueDialog::selectedTaskIds() const {
  QVector<quint64> ids;
  if (!table_ || !table_->selectionModel()) return ids;
  const auto rows = table_->selectionModel()->selectedRows();
  ids.reserve(rows.size());
  for (const QModelIndex& idx : rows) {
    const QVariant raw = idx.data(TransferTaskTableModel::TaskIdRole);
    bool ok = false;
    const quint64 id = raw.toULongLong(&ok);
    if (ok) ids.push_back(id);
  }
  return ids;
}

void TransferQueueDialog::showContextMenu(const QPoint& pos) {
  QModelIndex idx = table_->indexAt(pos);
  if (idx.isValid()) {
    // If the clicked row is not selected, select only that row
      if (!table_->selectionModel()->isSelected(idx)) {
        table_->clearSelection();
        table_->selectRow(idx.row());
      }
  } else {
    // click in empty area: do not show menu
    return;
  }

  const bool hasSel = table_->selectionModel() && table_->selectionModel()->hasSelection();
  QMenu menu(this);
  QAction* actPauseSel  = menu.addAction(tr("Pause selected"));
  QAction* actResumeSel = menu.addAction(tr("Resume selected"));
  QAction* actLimitSel  = menu.addAction(tr("Limit selected"));
  QAction* actCancelSel = menu.addAction(tr("Cancel selected"));
  actPauseSel->setEnabled(hasSel);
  actResumeSel->setEnabled(hasSel);
  actLimitSel->setEnabled(hasSel);
  actCancelSel->setEnabled(hasSel);

  QAction* chosen = menu.exec(table_->viewport()->mapToGlobal(pos));
  if (!chosen) return;
  if (chosen == actPauseSel) onPauseSelected();
  else if (chosen == actResumeSel) onResumeSelected();
  else if (chosen == actLimitSel) onLimitSelected();
  else if (chosen == actCancelSel) onStopSelected();
}
