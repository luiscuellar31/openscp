// Shared helper utilities for MainWindow split implementation files.
#pragma once

#include "TransferManager.hpp"

#include <QDir>
#include <QFileInfo>
#include <QPair>
#include <QString>
#include <QVector>
#include <QWidget>
#include <string>

bool isValidEntryName(const QString &name, QString *why = nullptr);
bool promptValidEntryName(QWidget *parent, const QString &dialogTitle,
                          const QString &labelText,
                          const QString &initialValue, QString &nameOut);
QString shortRemoteError(const QString &raw, const QString &fallback);
QString shortRemoteError(const std::string &raw, const QString &fallback);
QString joinRemotePath(const QString &base, const QString &name);
QString normalizeRemotePath(const QString &rawPath);

QVector<QPair<QString, QString>> buildLocalDestinationPairsWithOverwritePrompt(
    QWidget *parent, const QVector<QFileInfo> &sources,
    const QDir &destinationDir, int *skippedCount = nullptr);

bool isTransferTaskActiveStatus(TransferTask::Status status);
bool isTransferTaskFinalStatus(TransferTask::Status status);
const TransferTask *findTransferTask(const QVector<TransferTask> &tasks,
                                     TransferTask::Type type,
                                     const QString &src, const QString &dst);
bool hasActiveTransferTask(const QVector<TransferTask> &tasks,
                           TransferTask::Type type, const QString &src,
                           const QString &dst);
bool areTransferPairsFinal(const QVector<TransferTask> &tasks,
                           TransferTask::Type type,
                           const QVector<QPair<QString, QString>> &pairs);
