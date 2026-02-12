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
#include <QSpinBox>
#include <QInputDialog>
#include <QMenu>

static QString statusText(TransferTask::Status s) {
  switch (s) {
    case TransferTask::Status::Queued: return TransferQueueDialog::tr("En cola");
    case TransferTask::Status::Running: return TransferQueueDialog::tr("En progreso");
    case TransferTask::Status::Paused: return TransferQueueDialog::tr("Pausado");
    case TransferTask::Status::Done: return TransferQueueDialog::tr("Completado");
    case TransferTask::Status::Error: return TransferQueueDialog::tr("Error");
    case TransferTask::Status::Canceled: return TransferQueueDialog::tr("Cancelado");
  }
  return {};
}

class TransferTaskTableModel final : public QAbstractTableModel {
public:
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
        case 0: return TransferQueueDialog::tr("Tipo");
        case 1: return TransferQueueDialog::tr("Origen");
        case 2: return TransferQueueDialog::tr("Destino");
        case 3: return TransferQueueDialog::tr("Estado");
        case 4: return TransferQueueDialog::tr("Progreso");
        case 5: return TransferQueueDialog::tr("Intentos");
        default: return {};
      }
    }
    return section + 1;
  }

  QVariant data(const QModelIndex& index, int role) const override {
    if (!index.isValid() || role != Qt::DisplayRole) return {};
    if (index.row() < 0 || index.row() >= tasks_.size()) return {};
    const auto& t = tasks_[index.row()];
    switch (index.column()) {
      case 0: return t.type == TransferTask::Type::Upload ? TransferQueueDialog::tr("Subida") : TransferQueueDialog::tr("Descarga");
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
        emit dataChanged(index(row, 0), index(row, 5), {Qt::DisplayRole});
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

TransferQueueDialog::TransferQueueDialog(TransferManager* mgr, QWidget* parent)
  : QDialog(parent), mgr_(mgr) {
  setWindowTitle(tr("Cola de transferencias"));
  resize(760, 380);
  setSizeGripEnabled(true);

  auto* lay = new QVBoxLayout(this);

  // Tasks table
  model_ = new TransferTaskTableModel(this);
  table_ = new QTableView(this);
  table_->setModel(model_);
  table_->horizontalHeader()->setStretchLastSection(true);
  table_->verticalHeader()->setVisible(false);
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
  table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table_->setAlternatingRowColors(true);
  table_->setContextMenuPolicy(Qt::CustomContextMenu);
  lay->addWidget(table_);

  // Row 1: Controls (buttons in a single row)
  auto* controls = new QWidget(this);
  auto* hb = new QHBoxLayout(controls);
  hb->setContentsMargins(0,0,0,0);

  pauseBtn_  = new QPushButton(tr("Pausar"), controls);
  resumeBtn_ = new QPushButton(tr("Reanudar"), controls);
  pauseSelBtn_  = new QPushButton(tr("Pausar sel."), controls);
  resumeSelBtn_ = new QPushButton(tr("Reanudar sel."), controls);
  stopSelBtn_   = new QPushButton(tr("Cancelar sel."), controls);
  stopAllBtn_   = new QPushButton(tr("Cancelar"), controls);
  retryBtn_  = new QPushButton(tr("Reintentar"), controls);
  clearBtn_  = new QPushButton(tr("Limpiar"), controls);
  closeBtn_  = new QPushButton(tr("Cerrar"), controls);

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

  // Row 2: Global speed setting (speed and apply only)
  auto* speedRow = new QWidget(this);
  auto* hs2 = new QHBoxLayout(speedRow);
  hs2->setContentsMargins(0,0,0,0);
  speedSpin_ = new QSpinBox(speedRow);
  speedSpin_->setRange(0, 1'000'000);
  speedSpin_->setValue(mgr_->globalSpeedLimitKBps());
  speedSpin_->setSuffix(" KB/s");
  applySpeedBtn_ = new QPushButton(tr("Aplicar vel."), speedRow);
  // Move "Limit selected" next to the speed control
  limitSelBtn_  = new QPushButton(tr("Limitar sel."), speedRow);
  hs2->addWidget(new QLabel(tr("Velocidad:"), speedRow));
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
  auto sel = table_->selectionModel(); if (!sel || !sel->hasSelection()) return;
  const auto rows = sel->selectedRows();
  for (const QModelIndex& r : rows) {
    if (auto id = model_->taskIdAtRow(r.row()); id.has_value()) mgr_->pauseTask(*id);
  }
}
void TransferQueueDialog::onResumeSelected() {
  auto sel = table_->selectionModel(); if (!sel || !sel->hasSelection()) return;
  const auto rows = sel->selectedRows();
  for (const QModelIndex& r : rows) {
    if (auto id = model_->taskIdAtRow(r.row()); id.has_value()) mgr_->resumeTask(*id);
  }
}
void TransferQueueDialog::onApplyGlobalSpeed() {
  mgr_->setGlobalSpeedLimitKBps(speedSpin_->value());
  updateSummary();
}
void TransferQueueDialog::onLimitSelected() {
  auto sel = table_->selectionModel(); if (!sel || !sel->hasSelection()) return;
  const auto rows = sel->selectedRows();
  bool ok=false; int v = QInputDialog::getInt(this, tr("Límite para tarea(s)"), tr("KB/s (0 = sin límite)"), 0, 0, 1'000'000, 1, &ok);
  if (!ok) return;
  for (const QModelIndex& r : rows) {
    if (auto id = model_->taskIdAtRow(r.row()); id.has_value()) mgr_->setTaskSpeedLimit(*id, v);
  }
}

void TransferQueueDialog::onStopSelected() {
  auto sel = table_->selectionModel(); if (!sel || !sel->hasSelection()) return;
  const auto rows = sel->selectedRows();
  for (const QModelIndex& r : rows) {
    if (auto id = model_->taskIdAtRow(r.row()); id.has_value()) mgr_->cancelTask(*id);
  }
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
  QString summary = tr("Total: %1  |  En cola: %2  |  En progreso: %3  |  Pausado: %4  |  Error: %5  |  Completado: %6  |  Cancelado: %7")
                    .arg(tasks.size())
                    .arg(queued)
                    .arg(running)
                    .arg(paused)
                    .arg(error)
                    .arg(done)
                    .arg(canceled);
  const int gkb = mgr_->globalSpeedLimitKBps();
  if (gkb > 0) {
    summary += tr("  |  Límite global: %1 KB/s").arg(gkb);
  }
  summaryLabel_->setText(summary);

  // Enable/Disable actions based on state
  const bool hasAny = !tasks.isEmpty();
  const bool canPause = (queued + running) > 0; // something to pause
  const bool canResume = queued > 0;            // something queued to resume
  const bool canRetry = (error + canceled) > 0; // there are failed/canceled
  const bool canClear = done > 0;               // there are completed to clear
  const bool hasSel = table_->selectionModel() && table_->selectionModel()->hasSelection();

  if (pauseBtn_)  pauseBtn_->setEnabled(hasAny && canPause);
  if (resumeBtn_) resumeBtn_->setEnabled(hasAny && canResume);
  if (retryBtn_)  retryBtn_->setEnabled(hasAny && canRetry);
  if (clearBtn_)  clearBtn_->setEnabled(hasAny && canClear);
  if (pauseSelBtn_) pauseSelBtn_->setEnabled(hasSel);
  if (resumeSelBtn_) resumeSelBtn_->setEnabled(hasSel);
  if (limitSelBtn_) limitSelBtn_->setEnabled(hasSel);
  if (stopSelBtn_)  stopSelBtn_->setEnabled(hasSel);
  if (stopAllBtn_)  stopAllBtn_->setEnabled(running > 0);
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
  QAction* actPauseSel  = menu.addAction(tr("Pausar sel."));
  QAction* actResumeSel = menu.addAction(tr("Reanudar sel."));
  QAction* actLimitSel  = menu.addAction(tr("Limitar sel."));
  QAction* actCancelSel = menu.addAction(tr("Cancelar sel."));
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
