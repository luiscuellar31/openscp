// Shared helper utilities for MainWindow split implementation files.
#pragma once

#include <QString>
#include <string>

bool isValidEntryName(const QString &name, QString *why = nullptr);
QString shortRemoteError(const QString &raw, const QString &fallback);
QString shortRemoteError(const std::string &raw, const QString &fallback);
QString joinRemotePath(const QString &base, const QString &name);
QString normalizeRemotePath(const QString &rawPath);
