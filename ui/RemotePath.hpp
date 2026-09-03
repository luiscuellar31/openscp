// Canonical helpers for logical, root-confined remote paths.
#pragma once

#include <QString>

#include <cstdint>
#include <optional>
#include <string>

[[nodiscard]] QString normalizeRemotePath(const QString &rawPath);
[[nodiscard]] QString joinRemotePath(const QString &base,
                                     const QString &relativePath);
[[nodiscard]] bool isSafeRemoteRelativePath(const QString &relativePath);
[[nodiscard]] std::optional<QString>
decodeRemoteEntryName(const std::string &rawName);
[[nodiscard]] bool isRemoteSymlink(std::uint32_t mode);
