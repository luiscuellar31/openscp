#include "TestHarness.hpp"
#include "logic/remote/RemoteActionController.hpp"

#include <QCoreApplication>

namespace {

OPENSCP_TEST(testDisconnectedState, test) {
    openscp::ProtocolCapabilities capabilities;
    capabilities.can_upload = true;
    capabilities.can_delete = true;
    const auto result =
        openscpui::RemoteActionController::availability(capabilities, false);
    test.check(!result.canMutate && !result.canUpload && !result.canDelete,
               "disconnected sessions should disable every remote action");
}

OPENSCP_TEST(testIndependentCapabilities, test) {
    openscp::ProtocolCapabilities capabilities;
    capabilities.can_mkdir = true;
    capabilities.can_rename = true;
    capabilities.can_set_permissions = true;
    const auto result =
        openscpui::RemoteActionController::availability(capabilities);
    test.check(result.canMutate && result.canCreateDirectory &&
                   result.canRename && result.canSetPermissions,
               "supported mutations should be exposed independently");
    test.check(!result.canUpload && !result.canCreateFile &&
                   !result.canDelete && !result.canMoveToLocal,
               "unsupported actions should remain disabled");
}

OPENSCP_TEST(testMoveRequiresDownloadAndDelete, test) {
    openscp::ProtocolCapabilities capabilities;
    capabilities.can_download = true;
    auto result = openscpui::RemoteActionController::availability(capabilities);
    test.check(!result.canMoveToLocal,
               "download alone is insufficient for a remote move");

    capabilities.can_delete = true;
    result = openscpui::RemoteActionController::availability(capabilities);
    test.check(result.canMoveToLocal,
               "remote move should require both download and delete");
}

} // namespace

int main(int argc, char **argv) {
    openscp::test::TestHarness harness("remote action controller");
    return harness.runWithApplication<QCoreApplication>(argc, argv);
}
