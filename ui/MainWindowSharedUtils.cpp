// Shared helper utilities for MainWindow split implementation files.
#include "MainWindowSharedUtils.hpp"

#include "UiAlerts.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>

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

bool promptValidEntryName(QWidget *parent, const QString &dialogTitle,
                          const QString &labelText, const QString &initialValue,
                          QString &nameOut) {
    bool inputAccepted = false;
    const QString candidateName =
        QInputDialog::getText(parent, dialogTitle, labelText, QLineEdit::Normal,
                              initialValue, &inputAccepted);
    if (!inputAccepted || candidateName.isEmpty())
        return false;

    QString invalidReason;
    if (!isValidEntryName(candidateName, &invalidReason)) {
        UiAlerts::warning(
            parent, QCoreApplication::translate("MainWindow", "Invalid name"),
            invalidReason);
        return false;
    }
    nameOut = candidateName;
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

    const qsizetype newlineIndex = message.indexOf('\n');
    if (newlineIndex > 0) {
        message = message.left(newlineIndex);
    }
    message = message.simplified();
    if (message.size() > 96) {
        message = message.left(93) + "...";
    }
    return message;
}

QVector<QPair<QString, QString>> buildLocalDestinationPairsWithOverwritePrompt(
    QWidget *parent, const QVector<QFileInfo> &sources,
    const QDir &destinationDir, int *skippedCount) {
    enum class OverwritePolicy { Ask, OverwriteAll, SkipAll };
    OverwritePolicy policy = OverwritePolicy::Ask;

    int skipped = 0;
    QVector<QPair<QString, QString>> pairs;
    pairs.reserve(sources.size());

    for (const QFileInfo &sourceInfo : sources) {
        const QString targetPath =
            destinationDir.filePath(sourceInfo.fileName());
        if (QFileInfo::exists(targetPath)) {
            if (policy == OverwritePolicy::Ask) {
                const auto decision = UiAlerts::question(
                    parent,
                    QCoreApplication::translate("MainWindow", "Conflict"),
                    QCoreApplication::translate(
                        "MainWindow",
                        "“%1” already exists at destination.\nOverwrite?")
                        .arg(sourceInfo.fileName()),
                    QMessageBox::Yes | QMessageBox::No | QMessageBox::YesToAll |
                        QMessageBox::NoToAll);
                if (decision == QMessageBox::YesToAll)
                    policy = OverwritePolicy::OverwriteAll;
                else if (decision == QMessageBox::NoToAll)
                    policy = OverwritePolicy::SkipAll;

                if (decision == QMessageBox::No ||
                    policy == OverwritePolicy::SkipAll) {
                    ++skipped;
                    continue;
                }
            } else if (policy == OverwritePolicy::SkipAll) {
                ++skipped;
                continue;
            }
        }
        pairs.push_back({sourceInfo.absoluteFilePath(), targetPath});
    }

    if (skippedCount)
        *skippedCount = skipped;
    return pairs;
}

bool isTransferTaskFinalStatus(TransferTask::Status status) {
    return status == TransferTask::Status::Done ||
           status == TransferTask::Status::Error ||
           status == TransferTask::Status::Canceled;
}
