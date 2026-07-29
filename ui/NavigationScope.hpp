// Stable, secret-free identities for server-scoped navigation state.
#pragma once

#include "openscp/SftpTypes.hpp"

#include <QString>

namespace openscpui {

// Returns an opaque settings-safe identity for a saved site's stable UUID.
QString savedSiteNavigationScope(const QString &siteId);

// Returns an opaque settings-safe identity for a quick-connect endpoint.
// Credentials and local paths are intentionally excluded.
QString remoteEndpointScope(const openscp::SessionOptions &options);

} // namespace openscpui
