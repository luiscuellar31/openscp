// Windows records where a downloaded file came from in the file's
// Zone.Identifier stream, the Mark of the Web, so that SmartScreen and
// Office's Protected View treat it as coming from the internet.
#pragma once

#include "openscp/SessionOptions.hpp"

#include <QString>

namespace openscpui {

// The address of the session's server, without credentials or a path, as
// the source of what comes from it, such as sftp://files.example:2222/.
QString downloadOriginUrl(const openscp::SessionOptions &options);

// Marks the file at localPath as downloaded from originUrl, with the
// security zone that Internet Options give that address. Files from Local
// intranet or Trusted sites stay unmarked, as do all files when policy keeps
// Windows from saving zone information; other systems mark nothing.
void markDownloadedFile(const QString &localPath, const QString &originUrl);

} // namespace openscpui
