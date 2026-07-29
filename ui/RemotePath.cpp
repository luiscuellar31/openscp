#include "RemotePath.hpp"

#include <QDir>
#include <QStringDecoder>
#include <QStringList>

#include <algorithm>

QString normalizeRemotePath(const QString &rawPath) {
    QString normalizedInput = rawPath.trimmed();
    normalizedInput.replace(QLatin1Char('\\'), QLatin1Char('/'));

    const QStringList rawSegments =
        normalizedInput.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QStringList segments;
    segments.reserve(rawSegments.size());
    for (const QString &segment : rawSegments) {
        if (segment == QLatin1String("."))
            continue;
        if (segment == QLatin1String("..")) {
            // A remote path is confined to its logical root.
            if (!segments.isEmpty())
                segments.removeLast();
            continue;
        }
        segments.push_back(segment);
    }

    return segments.isEmpty()
               ? QStringLiteral("/")
               : QStringLiteral("/") + segments.join(QLatin1Char('/'));
}

QString joinRemotePath(const QString &base, const QString &relativePath) {
    if (relativePath.isEmpty())
        return normalizeRemotePath(base);
    return normalizeRemotePath(base + QLatin1Char('/') + relativePath);
}

bool isSafeRemoteEntryName(const QString &name) {
    if (name.isEmpty() || name == QLatin1String(".") ||
        name == QLatin1String("..") || name.contains(QLatin1Char('/')) ||
        name.contains(QLatin1Char('\\'))) {
        return false;
    }
    for (const QChar character : name) {
        const ushort codePoint = character.unicode();
        if (codePoint < 0x20u || codePoint == 0x7fu)
            return false;
    }
    return true;
}

bool isSafeRemoteRelativePath(const QString &relativePath) {
    if (relativePath.isEmpty() || relativePath.startsWith(QLatin1Char('/')) ||
        QDir::isAbsolutePath(relativePath) ||
        QDir::cleanPath(relativePath) != relativePath) {
        return false;
    }
    const QStringList parts =
        relativePath.split(QLatin1Char('/'), Qt::KeepEmptyParts);
    return std::all_of(parts.cbegin(), parts.cend(), [](const QString &part) {
        return isSafeRemoteEntryName(part);
    });
}

std::optional<QString> decodeRemoteEntryName(const std::string &rawName) {
    QStringDecoder decoder(QStringDecoder::Utf8);
    const QString name = decoder.decode(QByteArray::fromStdString(rawName));
    if (decoder.hasError() || !isSafeRemoteEntryName(name))
        return std::nullopt;
    return name;
}

bool isRemoteSymlink(std::uint32_t mode) {
    return (mode & 0170000u) == 0120000u;
}
