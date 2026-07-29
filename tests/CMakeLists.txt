add_executable(openscp_core_tests
    core_mock_tests.cpp
)

target_include_directories(openscp_core_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/core/src/common
    ${CMAKE_SOURCE_DIR}/core/src/libssh2/detail
)

target_link_libraries(openscp_core_tests PRIVATE
    openscp_core
)

add_test(NAME openscp_core_tests COMMAND openscp_core_tests)

add_executable(openscp_transfer_manager_tests
    transfer_manager_tests.cpp
    ${CMAKE_SOURCE_DIR}/ui/TransferManager.cpp
    ${CMAKE_SOURCE_DIR}/ui/TransferManager.hpp
    ${CMAKE_SOURCE_DIR}/ui/UiAlerts.cpp
    ${CMAKE_SOURCE_DIR}/ui/UiAlerts.hpp
)

target_include_directories(openscp_transfer_manager_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/ui
)

target_link_libraries(openscp_transfer_manager_tests PRIVATE
    Qt6::Widgets
    openscp_core
)

add_test(NAME openscp_transfer_manager_tests
         COMMAND openscp_transfer_manager_tests)

add_executable(openscp_remote_operation_controller_tests
    remote_operation_controller_tests.cpp
    ${CMAKE_SOURCE_DIR}/ui/RemoteOperationController.cpp
    ${CMAKE_SOURCE_DIR}/ui/RemoteOperationController.hpp
)

target_include_directories(openscp_remote_operation_controller_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/ui
)

target_link_libraries(openscp_remote_operation_controller_tests PRIVATE
    Qt6::Widgets
    openscp_core
)

add_test(NAME openscp_remote_operation_controller_tests
         COMMAND openscp_remote_operation_controller_tests)

add_executable(openscp_remote_model_tests
    remote_model_tests.cpp
    ${CMAKE_SOURCE_DIR}/ui/RemoteModel.cpp
    ${CMAKE_SOURCE_DIR}/ui/RemoteModel.hpp
    ${CMAKE_SOURCE_DIR}/ui/MainWindowSharedUtils.cpp
    ${CMAKE_SOURCE_DIR}/ui/MainWindowSharedUtils.hpp
    ${CMAKE_SOURCE_DIR}/ui/UiAlerts.cpp
    ${CMAKE_SOURCE_DIR}/ui/UiAlerts.hpp
)

target_include_directories(openscp_remote_model_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/ui
)

target_link_libraries(openscp_remote_model_tests PRIVATE
    Qt6::Widgets
    openscp_core
)

add_test(NAME openscp_remote_model_tests
         COMMAND openscp_remote_model_tests)

add_executable(openscp_saved_sites_persistence_tests
    saved_sites_persistence_tests.cpp
    ${CMAKE_SOURCE_DIR}/ui/SavedSitesPersistence.cpp
    ${CMAKE_SOURCE_DIR}/ui/SavedSitesPersistence.hpp
)

target_include_directories(openscp_saved_sites_persistence_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/ui
)

target_link_libraries(openscp_saved_sites_persistence_tests PRIVATE
    Qt6::Widgets
    openscp_core
)

add_test(NAME openscp_saved_sites_persistence_tests
         COMMAND openscp_saved_sites_persistence_tests)

add_executable(openscp_navigation_scope_tests
    navigation_scope_tests.cpp
    ${CMAKE_SOURCE_DIR}/ui/NavigationScope.cpp
    ${CMAKE_SOURCE_DIR}/ui/NavigationScope.hpp
)

target_include_directories(openscp_navigation_scope_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/ui
)

target_link_libraries(openscp_navigation_scope_tests PRIVATE
    Qt6::Widgets
    openscp_core
)

add_test(NAME openscp_navigation_scope_tests
         COMMAND openscp_navigation_scope_tests)

add_executable(openscp_sync_comparison_tests
    sync_comparison_tests.cpp
    ${CMAKE_SOURCE_DIR}/ui/SyncDialog.cpp
    ${CMAKE_SOURCE_DIR}/ui/SyncDialog.hpp
)

target_include_directories(openscp_sync_comparison_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/ui
)

target_link_libraries(openscp_sync_comparison_tests PRIVATE
    Qt6::Widgets
)

add_test(NAME openscp_sync_comparison_tests
         COMMAND openscp_sync_comparison_tests)

add_executable(openscp_sync_coordinator_tests
    sync_coordinator_tests.cpp
    ${CMAKE_SOURCE_DIR}/ui/SyncCoordinator.cpp
    ${CMAKE_SOURCE_DIR}/ui/SyncCoordinator.hpp
    ${CMAKE_SOURCE_DIR}/ui/SyncDialog.cpp
    ${CMAKE_SOURCE_DIR}/ui/SyncDialog.hpp
    ${CMAKE_SOURCE_DIR}/ui/RemoteOperationController.cpp
    ${CMAKE_SOURCE_DIR}/ui/RemoteOperationController.hpp
    ${CMAKE_SOURCE_DIR}/ui/TransferManager.cpp
    ${CMAKE_SOURCE_DIR}/ui/TransferManager.hpp
    ${CMAKE_SOURCE_DIR}/ui/UiAlerts.cpp
    ${CMAKE_SOURCE_DIR}/ui/UiAlerts.hpp
)

target_include_directories(openscp_sync_coordinator_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/ui
)

target_link_libraries(openscp_sync_coordinator_tests PRIVATE
    Qt6::Widgets
    openscp_core
)

add_test(NAME openscp_sync_coordinator_tests
         COMMAND openscp_sync_coordinator_tests)

add_executable(openscp_local_tree_discovery_tests
    local_tree_discovery_tests.cpp
    ${CMAKE_SOURCE_DIR}/ui/LocalTreeDiscovery.cpp
    ${CMAKE_SOURCE_DIR}/ui/LocalTreeDiscovery.hpp
)

target_include_directories(openscp_local_tree_discovery_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/ui
)

target_link_libraries(openscp_local_tree_discovery_tests PRIVATE
    Qt6::Widgets
)

add_test(NAME openscp_local_tree_discovery_tests
         COMMAND openscp_local_tree_discovery_tests)

add_executable(openscp_sftp_integration_tests
    libssh2_integration_tests.cpp
)

target_include_directories(openscp_sftp_integration_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
)

target_link_libraries(openscp_sftp_integration_tests PRIVATE
    openscp_core
)

add_test(NAME openscp_sftp_integration_tests COMMAND openscp_sftp_integration_tests)
set_tests_properties(openscp_sftp_integration_tests PROPERTIES
    LABELS "integration;sftp"
    SKIP_RETURN_CODE 77
)

add_executable(openscp_scp_integration_tests
    libssh2_scp_integration_tests.cpp
)

target_include_directories(openscp_scp_integration_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
)

target_link_libraries(openscp_scp_integration_tests PRIVATE
    openscp_core
)

add_test(NAME openscp_scp_integration_tests COMMAND openscp_scp_integration_tests)
set_tests_properties(openscp_scp_integration_tests PROPERTIES
    LABELS "integration;scp"
    SKIP_RETURN_CODE 77
)

get_target_property(_OPENSCP_CORE_HAS_CURL_FTP
                    openscp_core OPENSCP_HAS_CURL_FTP)
if (_OPENSCP_CORE_HAS_CURL_FTP)
    add_executable(openscp_ftp_integration_tests
        curl_ftp_integration_tests.cpp
    )

    target_include_directories(openscp_ftp_integration_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/core/include
    )

    target_link_libraries(openscp_ftp_integration_tests PRIVATE
        openscp_core
    )

    add_test(NAME openscp_ftp_integration_tests
             COMMAND openscp_ftp_integration_tests)
    set_tests_properties(openscp_ftp_integration_tests PROPERTIES
        LABELS "integration;ftp"
        SKIP_RETURN_CODE 77
    )

    add_executable(openscp_ftps_integration_tests
        curl_ftps_integration_tests.cpp
    )

    target_include_directories(openscp_ftps_integration_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/core/include
    )

    target_link_libraries(openscp_ftps_integration_tests PRIVATE
        openscp_core
    )

    add_test(NAME openscp_ftps_integration_tests
             COMMAND openscp_ftps_integration_tests)
    set_tests_properties(openscp_ftps_integration_tests PROPERTIES
        LABELS "integration;ftps"
        SKIP_RETURN_CODE 77
    )
else()
    message(STATUS "OpenSCP tests: skipping FTP/FTPS integration tests (FTP/FTPS backend disabled)")
endif()

get_target_property(_OPENSCP_CORE_HAS_CURL_WEBDAV
                    openscp_core OPENSCP_HAS_CURL_WEBDAV)

if (_OPENSCP_CORE_HAS_CURL_FTP OR _OPENSCP_CORE_HAS_CURL_WEBDAV)
    add_executable(openscp_curl_backend_common_tests
        curl_backend_common_tests.cpp
    )

    target_include_directories(openscp_curl_backend_common_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/core/include
        ${CMAKE_SOURCE_DIR}/core/src/curl
    )

    target_link_libraries(openscp_curl_backend_common_tests PRIVATE
        openscp_core
    )

    add_test(NAME openscp_curl_backend_common_tests
             COMMAND openscp_curl_backend_common_tests)
endif()

if (_OPENSCP_CORE_HAS_CURL_WEBDAV)
    add_executable(openscp_webdav_integration_tests
        curl_webdav_integration_tests.cpp
    )

    target_include_directories(openscp_webdav_integration_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/core/include
    )

    target_link_libraries(openscp_webdav_integration_tests PRIVATE
        openscp_core
    )

    add_test(NAME openscp_webdav_integration_tests
             COMMAND openscp_webdav_integration_tests)
    set_tests_properties(openscp_webdav_integration_tests PROPERTIES
        LABELS "integration;webdav"
        SKIP_RETURN_CODE 77
    )
else()
    message(STATUS "OpenSCP tests: skipping WebDAV integration tests (WebDAV backend disabled)")
endif()
