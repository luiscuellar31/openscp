// Shared, locale-independent formatting for sizes and transfer rates.
#pragma once

#include <QString>
#include <QtCore/qglobal.h>

#include <limits>
#include <type_traits>

[[nodiscard]] QString formatByteSize(quint64 bytes);
[[nodiscard]] QString formatTransferRate(double kibibytesPerSecond);

// tr() takes the count that picks a plural form as an int; a larger count
// reads as the largest.
template <typename Count> [[nodiscard]] int pluralCount(Count count) {
    static_assert(std::is_integral_v<Count>, "a plural count is an integer");
    if (count <= 0)
        return 0;
    return static_cast<quint64>(count) >
                   static_cast<quint64>(std::numeric_limits<int>::max())
               ? std::numeric_limits<int>::max()
               : static_cast<int>(count);
}
