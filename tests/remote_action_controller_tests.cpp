#include "RemoteActionController.hpp"
#include "TestHarness.hpp"

#include <QCoreApplication>

#include <iostream>

namespace {

void testDisconnectedState(TestContext &test) {
    openscp::ProtocolCapabilities capabilities;
    capabilities.can_upload = true;
    capabilities.can_delete = true;
    const auto result =
        openscpui::RemoteActionController::availability(capabilities, false);
    test.check(!result.canMutate && !result.canUpload && !result.canDelete,
               "disconnected sessions should disable every remote action");
}

void testIndependentCapabilities(TestContext &test) {
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

void testMoveRequiresDownloadAndDelete(TestContext &test) {
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
    QCoreApplication application(argc, argv);
    TestContext test;
    testDisconnectedState(test);
    testIndependentCapabilities(test);
    testMoveRequiresDownloadAndDelete(test);
    if (test.failures == 0)
        std::cout << "All remote action controller tests passed\n";
    return test.failures == 0 ? 0 : 1;
}
