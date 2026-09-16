// Sample data for demo builds. Compiled only when OPENSCP_ENABLE_DEMO_MODE is
// on, and never reachable from a release build.
#pragma once

namespace openscpui::demo {

// Writes sample sites and navigation history into the demo profile when it has
// none. Anything already stored is left untouched, so edits made while testing
// survive the next run.
void seedSampleDataIfEmpty();

} // namespace openscpui::demo
