// Tabla con estado por tarea y acciones (pausar/reanudar/reintentar/limpiar).
#include "TransferQueueDialog.hpp"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QHeaderView>
#include <QAbstractItemView>
#include <QTableWidgetItem>
#include <QSpinBox>
#include <QInputDialog>
#include <QMenu>

TransferQueueDialog::TransferQueueDialog(TransferManager* mgr, QWidget* parent)
  : QDialog(parent), mgr_(mgr) {
  setWindowTitle(tr("Cola de transferencias"));
  resize(760, 380);
  setSizeGripEnabled(true);

  auto* lay = new QVBoxLayout(this);

  // Tabla de tareas
  table_ = new QTableWidget(this);
  table_->setColumnCount(6);
  table_->setHorizontalHeaderLabels({ tr("Tipo"), tr("Origen"), tr("Destino"), tr("Estado"), tr("Progreso"), tr("Intentos") });
  table_->horizontalHeader()->setStretchLastSection(true);
  table_->verticalHeader()->setVisible(false);
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
  table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table_->setAlternatingRowColors(true);
  table_->setContextMenuPolicy(Qt::CustomContextMenu);
  lay->addWidget(table_);

  // Fila 1: Controles (solo botones en un renglón)
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

  // Fila 2: Ajuste de velocidad global (solo velocidad y aplicar)
  auto* speedRow = new QWidget(this);
  auto* hs2 = new QHBoxLayout(speedRow);
  hs2->setContentsMargins(0,0,0,0);
  speedSpin_ = new QSpinBox(speedRow);
  speedSpin_->setRange(0, 1'000'000);
  speedSpin_->setValue(mgr_->globalSpeedLimitKBps());
  speedSpin_->setSuffix(" KB/s");
  applySpeedBtn_ = new QPushButton(tr("Aplicar vel."), speedRow);
  // Mover "Limitar sel." junto al control de velocidad
  limitSelBtn_  = new QPushButton(tr("Limitar sel."), speedRow);
  hs2->addWidget(new QLabel(tr("Velocidad:"), speedRow));
  hs2->addWidget(speedSpin_);
  hs2->addWidget(applySpeedBtn_);
  hs2->addWidget(limitSelBtn_);
  hs2->addStretch();
  lay->addWidget(speedRow);

  // Fila de resumen (al pie)
  auto* summary = new QWidget(this);
  auto* hs = new QHBoxLayout(summary);
  hs->setContentsMargins(0,0,0,0);
  summaryLabel_ = new QLabel(tr(""), summary);
  summaryLabel_->setWordWrap(true);
  hs->addWidget(summaryLabel_);
  lay->addWidget(summary);

  // Conexiones
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
  // Mantener habilitación de botones de selección actualizada
  connect(table_->selectionModel(), &QItemSelectionModel::selectionChanged, this, &TransferQueueDialog::updateSummary);
  connect(table_, &QTableWidget::customContextMenuRequested, this, &TransferQueueDialog::showContextMenu);
  refresh();
}

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

void TransferQueueDialog::refresh() {
  const auto& tasks = mgr_->tasks();
  table_->setRowCount(tasks.size());
  for (int i = 0; i < tasks.size(); ++i) {
    const auto& t = tasks[i];
    table_->setItem(i, 0, new QTableWidgetItem(t.type == TransferTask::Type::Upload ? tr("Subida") : tr("Descarga")));
    table_->setItem(i, 1, new QTableWidgetItem(t.src));
    table_->setItem(i, 2, new QTableWidgetItem(t.dst));
    table_->setItem(i, 3, new QTableWidgetItem(statusText(t.status)));
    table_->setItem(i, 4, new QTableWidgetItem(QString::number(t.progress) + "%"));
    table_->setItem(i, 5, new QTableWidgetItem(QString("%1/%2").arg(t.attempts).arg(t.maxAttempts)));
  }
  updateSummary();
}

void TransferQueueDialog::onPause() { mgr_->pauseAll(); }
void TransferQueueDialog::onResume(){ mgr_->resumeAll(); }
void TransferQueueDialog::onRetry() { mgr_->retryFailed(); }
void TransferQueueDialog::onClearDone() { mgr_->clearCompleted(); }

void TransferQueueDialog::onPauseSelected() {
  auto sel = table_->selectionModel(); if (!sel || !sel->hasSelection()) return;
  const auto rows = sel->selectedRows();
  const auto& tasks = mgr_->tasks();
  for (const QModelIndex& r : rows) {
    int row = r.row(); if (row < 0 || row >= tasks.size()) continue;
    mgr_->pauseTask(tasks[row].id);
  }
}
void TransferQueueDialog::onResumeSelected() {
  auto sel = table_->selectionModel(); if (!sel || !sel->hasSelection()) return;
  const auto rows = sel->selectedRows();
  const auto& tasks = mgr_->tasks();
  for (const QModelIndex& r : rows) {
    int row = r.row(); if (row < 0 || row >= tasks.size()) continue;
    mgr_->resumeTask(tasks[row].id);
  }
}
void TransferQueueDialog::onApplyGlobalSpeed() {
  mgr_->setGlobalSpeedLimitKBps(speedSpin_->value());
  updateSummary();
}
void TransferQueueDialog::onLimitSelected() {
  auto sel = table_->selectionModel(); if (!sel || !sel->hasSelection()) return;
  const auto rows = sel->selectedRows(); const auto& tasks = mgr_->tasks();
  bool ok=false; int v = QInputDialog::getInt(this, tr("Límite para tarea(s)"), tr("KB/s (0 = sin límite)"), 0, 0, 1'000'000, 1, &ok);
  if (!ok) return;
  for (const QModelIndex& r : rows) {
    int row = r.row(); if (row < 0 || row >= tasks.size()) continue;
    mgr_->setTaskSpeedLimit(tasks[row].id, v);
  }
}

void TransferQueueDialog::onStopSelected() {
  auto sel = table_->selectionModel(); if (!sel || !sel->hasSelection()) return;
  const auto rows = sel->selectedRows(); const auto& tasks = mgr_->tasks();
  for (const QModelIndex& r : rows) {
    int row = r.row(); if (row < 0 || row >= tasks.size()) continue;
    mgr_->cancelTask(tasks[row].id);
  }
}

void TransferQueueDialog::onStopAll() {
  mgr_->cancelAll();
}

void TransferQueueDialog::updateSummary() {
  const auto& tasks = mgr_->tasks();
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
  QString summary = tr("Total: %1  |  En cola: %2  |  En progreso: %3  |  Pausado: %4  |  Error: %5  |  Completado: %6")
                    .arg(tasks.size())
                    .arg(queued)
                    .arg(running)
                    .arg(paused)
                    .arg(error)
                    .arg(done);
  const int gkb = mgr_->globalSpeedLimitKBps();
  if (gkb > 0) {
    summary += tr("  |  Límite global: %1 KB/s").arg(gkb);
  }
  summaryLabel_->setText(summary);

  // Habilitar/Deshabilitar acciones según estado
  const bool hasAny = !tasks.isEmpty();
  const bool canPause = (queued + running) > 0; // hay algo que pausar
  const bool canResume = queued > 0;            // hay algo en cola para reanudar
  const bool canRetry = (error + canceled) > 0; // hay fallidos/cancelados
  const bool canClear = done > 0;               // hay completados que limpiar
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
    // Si la fila clicada no está seleccionada, seleccionar solo esa
    if (!table_->selectionModel()->isSelected(idx)) {
      table_->clearSelection();
      table_->selectRow(idx.row());
    }
  } else {
    // click en zona vacía: no mostrar menú
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
