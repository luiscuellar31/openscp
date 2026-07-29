#include "UiFormatters.hpp"

#include <array>

QString formatByteSize(quint64 bytes) {
    static constexpr std::array<const char *, 5> units{"B", "KiB", "MiB", "GiB",
                                                       "TiB"};
    double value = static_cast<double>(bytes);
    std::size_t unitIndex = 0;
    while (value >= 1024.0 && unitIndex + 1 < units.size()) {
        value /= 1024.0;
        ++unitIndex;
    }
    const int precision = unitIndex > 0 && value < 10.0 ? 1 : 0;
    return QStringLiteral("%1 %2").arg(QString::number(value, 'f', precision),
                                       QString::fromLatin1(units[unitIndex]));
}

QString formatTransferRate(double kibibytesPerSecond) {
    if (kibibytesPerSecond <= 0.0)
        return QString::fromUtf8("—");
    const auto bytesPerSecond =
        static_cast<quint64>(kibibytesPerSecond * 1024.0);
    return formatByteSize(bytesPerSecond) + QStringLiteral("/s");
}
