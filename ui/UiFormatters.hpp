// Shared, locale-independent formatting for sizes and transfer rates.
#pragma once

#include <QString>
#include <QtTypes>

[[nodiscard]] QString formatByteSize(quint64 bytes);
[[nodiscard]] QString formatTransferRate(double kibibytesPerSecond);
