// Shared helper utilities for MainWindow split implementation files.
#include "MainWindowSharedUtils.hpp"

#include <QCoreApplication>

bool isValidEntryName(const QString &name, QString *why) {
    if (name == "." || name == "..") {
        if (why) {
            *why = QCoreApplication::translate(
                "MainWindow", "Invalid name: cannot be '.' or '..'.");
        }
        return false;
    }
    if (name.contains('/') || name.contains('\\')) {
        if (why) {
            *why = QCoreApplication::translate(
                "MainWindow",
                "Invalid name: cannot contain separators ('/' or '\\\\').");
        }
        return false;
    }
    for (const QChar ch : name) {
        const ushort codePoint = ch.unicode();
        if (codePoint < 0x20u || codePoint == 0x7Fu) {
            if (why) {
                *why = QCoreApplication::translate(
                    "MainWindow",
                    "Invalid name: cannot contain control characters.");
            }
            return false;
        }
    }
    return true;
}

QString shortRemoteError(const QString &raw, const QString &fallback) {
    QString message = raw.trimmed();
    if (message.isEmpty()) {
        return fallback;
    }

    const QString lowercase = message.toLower();
    if (lowercase.contains("permission denied")) {
        return QCoreApplication::translate("MainWindow", "Permission denied.");
    }
    if (lowercase.contains("read-only")) {
        return QCoreApplication::translate("MainWindow",
                                           "Location is read-only.");
    }
    if (lowercase.contains("no such file") || lowercase.contains("not found")) {
        return QCoreApplication::translate("MainWindow",
                                           "File or folder does not exist.");
    }
    if (lowercase.contains("timed out") || lowercase.contains("timeout")) {
        return QCoreApplication::translate("MainWindow",
                                           "Connection timed out.");
    }
    if (lowercase.contains("could not resolve") ||
        lowercase.contains("name or service not known") ||
        lowercase.contains("nodename nor servname")) {
        return QCoreApplication::translate(
            "MainWindow", "Could not resolve the server hostname.");
    }
    if (lowercase.contains("connection refused")) {
        return QCoreApplication::translate("MainWindow",
                                           "Connection refused by the server.");
    }
    if (lowercase.contains("network is unreachable") ||
        lowercase.contains("host is unreachable")) {
        return QCoreApplication::translate(
            "MainWindow", "Network unavailable or host unreachable.");
    }
    if (lowercase.contains("authentication failed") ||
        lowercase.contains("auth fail")) {
        return QCoreApplication::translate("MainWindow",
                                           "Authentication failed.");
    }

    const int newlineIndex = message.indexOf('\n');
    if (newlineIndex > 0) {
        message = message.left(newlineIndex);
    }
    message = message.simplified();
    if (message.size() > 96) {
        message = message.left(93) + "...";
    }
    return message;
}

QString shortRemoteError(const std::string &raw, const QString &fallback) {
    return shortRemoteError(QString::fromStdString(raw), fallback);
}

QString joinRemotePath(const QString &base, const QString &name) {
    if (base == QStringLiteral("/"))
        return QStringLiteral("/") + name;
    return base.endsWith(QLatin1Char('/')) ? base + name
                                           : base + QLatin1Char('/') + name;
}

QString normalizeRemotePath(const QString &rawPath) {
    QString normalized = rawPath.trimmed();
    if (normalized.isEmpty())
        normalized = QStringLiteral("/");
    if (!normalized.startsWith(QLatin1Char('/')))
        normalized.prepend(QLatin1Char('/'));
    while (normalized.contains(QStringLiteral("//")))
        normalized.replace(QStringLiteral("//"), QStringLiteral("/"));
    if (normalized.size() > 1 && normalized.endsWith(QLatin1Char('/')))
        normalized.chop(1);
    return normalized;
}
