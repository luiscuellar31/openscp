#pragma once

#include "logic/updates/UpdateMetadata.hpp"

namespace openscpui {
// Publishes through a retained directory descriptor. The original stays usable
// until the final atomic rename; a hard-linked .previous image is retained.
bool installAppImageUpdate(const QString &original, const QString &download,
                           const UpdateArtifact &artifact, QString &error);
} // namespace openscpui
