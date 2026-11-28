#pragma once

#include "logic/updates/UpdateMetadata.hpp"

#include <QStringList>

namespace openscpui {
// The arguments that make the setup update an installation of `scope`
// silently: it waits for process `pid` to end, writes its log to `logPath`,
// and, for the current user's installation, starts OpenSCP again when it is
// done.
QStringList windowsSetupArguments(WindowsSetupScope scope, qint64 pid,
                                  const QString &logPath);

// Starts the downloaded setup once its size and SHA-256 match the signed
// artifact. The file stays open against changes from the check until the
// setup has started; an installation for all users asks for elevation.
bool startWindowsSetup(const QString &setup, const UpdateArtifact &artifact,
                       const QStringList &arguments, WindowsSetupScope scope,
                       QString &error);
} // namespace openscpui
