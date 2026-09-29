// Builds whole-row selections for item views.
#pragma once

#include <QItemSelection>
#include <QModelIndex>
#include <QVector>

class QAbstractItemModel;

namespace openscpui {

// One selection covering the given rows, with consecutive rows merged into
// ranges. Selecting it in a single call notifies observers once; selecting row
// by row notifies once per row, and every notification walks the selection
// built so far.
[[nodiscard]] QItemSelection rowSelection(const QAbstractItemModel &model,
                                          const QModelIndex &parent,
                                          QVector<int> rows);

} // namespace openscpui
