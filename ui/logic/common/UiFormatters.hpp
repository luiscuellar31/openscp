// Shared, locale-independent formatting for sizes and transfer rates.
#pragma once

#include <QString>
#include <QtCore/qglobal.h>

[[nodiscard]] QString formatByteSize(quint64 bytes);
[[nodiscard]] QString formatTransferRate(double kibibytesPerSecond);
