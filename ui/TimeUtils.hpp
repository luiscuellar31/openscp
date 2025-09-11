// Small UI time helpers shared across views/dialogs.
#pragma once
#include <QString>
#include <QDateTime>
#include <QLocale>

namespace openscpui {

// Format epoch seconds for user-facing display in LOCAL time (short format),
// using system locale so 12/24h and date formats match OS preferences.
inline QString localShortTime(quint64 secs) {
    if (secs == 0) return QStringLiteral("—");
    const QDateTime dt = QDateTime::fromSecsSinceEpoch((qint64)secs);
    if (!dt.isValid()) return QStringLiteral("—");
    return QLocale::system().toString(dt, QLocale::ShortFormat);
}

// Overload for QDateTime values (assumed to be in local time unless specified).
inline QString localShortTime(const QDateTime& dt) {
    if (!dt.isValid()) return QStringLiteral("—");
    return QLocale::system().toString(dt, QLocale::ShortFormat);
}

} // namespace openscpui
