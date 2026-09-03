// Shared helper utilities for MainWindow split implementation files.
#pragma once

#include "RemotePath.hpp"
#include "TransferTypes.hpp"

#include <QDir>
#include <QFileInfo>
#include <QPair>
#include <QString>
#include <QVector>
#include <QWidget>

inline constexpr int kNameColumn = 0;

struct PathDepthComparator {
    bool deepestFirst = false;

    bool operator()(const QString &left, const QString &right) const;
};

bool isValidEntryName(const QString &name, QString *why = nullptr);
bool promptValidEntryName(QWidget *parent, const QString &dialogTitle,
                          const QString &labelText, const QString &initialValue,
                          QString &nameOut);
QString shortRemoteError(const QString &raw, const QString &fallback);

QVector<QPair<QString, QString>> buildLocalDestinationPairsWithOverwritePrompt(
    QWidget *parent, const QVector<QFileInfo> &sources,
    const QDir &destinationDir, int *skippedCount = nullptr);
