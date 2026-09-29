#include "logic/common/RowSelection.hpp"

#include <QAbstractItemModel>

#include <algorithm>

namespace openscpui {

QItemSelection rowSelection(const QAbstractItemModel &model,
                            const QModelIndex &parent, QVector<int> rows) {
    QItemSelection selection;
    if (rows.isEmpty())
        return selection;

    std::sort(rows.begin(), rows.end());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    const int lastColumn = std::max(0, model.columnCount(parent) - 1);
    const int rowCount = model.rowCount(parent);
    for (qsizetype index = 0; index < rows.size();) {
        const int first = rows[index];
        if (first < 0 || first >= rowCount) {
            ++index;
            continue;
        }
        int last = first;
        while (++index < rows.size() && rows[index] == last + 1 &&
               rows[index] < rowCount) {
            last = rows[index];
        }
        selection.select(model.index(first, 0, parent),
                         model.index(last, lastColumn, parent));
    }
    return selection;
}

} // namespace openscpui
