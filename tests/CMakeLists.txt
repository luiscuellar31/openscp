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
)

target_link_libraries(openscp_transfer_manager_tests PRIVATE
    openscp_ui_logic
)

add_test(NAME openscp_transfer_manager_tests
         COMMAND openscp_transfer_manager_tests)

add_executable(openscp_remote_operation_controller_tests
    remote_operation_controller_tests.cpp
)

target_link_libraries(openscp_remote_operation_controller_tests PRIVATE
    openscp_ui_logic
)

add_test(NAME openscp_remote_operation_controller_tests
         COMMAND openscp_remote_operation_controller_tests)

add_executable(openscp_remote_model_tests
    remote_model_tests.cpp
)

target_link_libraries(openscp_remote_model_tests PRIVATE
    openscp_ui_logic
)

add_test(NAME openscp_remote_model_tests
         COMMAND openscp_remote_model_tests)

add_executable(openscp_saved_sites_persistence_tests
    saved_sites_persistence_tests.cpp
)

target_link_libraries(openscp_saved_sites_persistence_tests PRIVATE
    openscp_ui_logic
)

add_test(NAME openscp_saved_sites_persistence_tests
         COMMAND openscp_saved_sites_persistence_tests)

add_executable(openscp_site_credential_repository_tests
    site_credential_repository_tests.cpp
)

target_link_libraries(openscp_site_credential_repository_tests PRIVATE
    openscp_ui_logic
)

add_test(NAME openscp_site_credential_repository_tests
         COMMAND openscp_site_credential_repository_tests)

add_executable(openscp_navigation_scope_tests
    navigation_scope_tests.cpp
)

target_link_libraries(openscp_navigation_scope_tests PRIVATE
    openscp_ui_logic
)

add_test(NAME openscp_navigation_scope_tests
         COMMAND openscp_navigation_scope_tests)

add_executable(openscp_navigation_store_tests
    navigation_store_tests.cpp
)

target_link_libraries(openscp_navigation_store_tests PRIVATE
    openscp_ui_logic
)

add_test(NAME openscp_navigation_store_tests
         COMMAND openscp_navigation_store_tests)

add_executable(openscp_pane_controller_tests
    pane_controller_tests.cpp
)

target_link_libraries(openscp_pane_controller_tests PRIVATE
    openscp_ui_logic
)

add_test(NAME openscp_pane_controller_tests
         COMMAND openscp_pane_controller_tests)

add_executable(openscp_remote_action_controller_tests
    remote_action_controller_tests.cpp
)

target_link_libraries(openscp_remote_action_controller_tests PRIVATE
    openscp_ui_logic
)

add_test(NAME openscp_remote_action_controller_tests
         COMMAND openscp_remote_action_controller_tests)

add_executable(openscp_session_controller_tests
    session_controller_tests.cpp
)

target_link_libraries(openscp_session_controller_tests PRIVATE
    openscp_ui_logic
)

add_test(NAME openscp_session_controller_tests
         COMMAND openscp_session_controller_tests)

add_executable(openscp_transfer_ui_controller_tests
    transfer_ui_controller_tests.cpp
)

target_link_libraries(openscp_transfer_ui_controller_tests PRIVATE
    openscp_ui_logic
)

add_test(NAME openscp_transfer_ui_controller_tests
         COMMAND openscp_transfer_ui_controller_tests)

add_executable(openscp_terminal_command_builder_tests
    terminal_command_builder_tests.cpp
)

target_link_libraries(openscp_terminal_command_builder_tests PRIVATE
    openscp_ui_logic
)

add_test(NAME openscp_terminal_command_builder_tests
         COMMAND openscp_terminal_command_builder_tests)

add_executable(openscp_ui_utilities_tests
    ui_utilities_tests.cpp
    ${CMAKE_SOURCE_DIR}/ui/RemotePath.cpp
    ${CMAKE_SOURCE_DIR}/ui/RemotePath.hpp
    ${CMAKE_SOURCE_DIR}/ui/UiFormatters.cpp
    ${CMAKE_SOURCE_DIR}/ui/UiFormatters.hpp
)

target_include_directories(openscp_ui_utilities_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/ui
)

target_link_libraries(openscp_ui_utilities_tests PRIVATE
    Qt6::Core
)

add_test(NAME openscp_ui_utilities_tests
         COMMAND openscp_ui_utilities_tests)

add_executable(openscp_sync_comparison_tests
    sync_comparison_tests.cpp
)

target_link_libraries(openscp_sync_comparison_tests PRIVATE
    openscp_sync_logic
)

add_test(NAME openscp_sync_comparison_tests
         COMMAND openscp_sync_comparison_tests)

add_executable(openscp_sync_coordinator_tests
    sync_coordinator_tests.cpp
)

target_link_libraries(openscp_sync_coordinator_tests PRIVATE
    openscp_ui_logic
)

add_test(NAME openscp_sync_coordinator_tests
         COMMAND openscp_sync_coordinator_tests)

add_executable(openscp_local_tree_discovery_tests
    local_tree_discovery_tests.cpp
)

target_link_libraries(openscp_local_tree_discovery_tests PRIVATE
    openscp_ui_logic
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

# Keep the local verification helper independent from the set of optional
# protocol backends while ensuring every configured CTest executable exists.
set(OPENSCP_TEST_BINARY_TARGETS
    openscp_core_tests
    openscp_transfer_manager_tests
    openscp_remote_operation_controller_tests
    openscp_remote_model_tests
    openscp_saved_sites_persistence_tests
    openscp_site_credential_repository_tests
    openscp_navigation_scope_tests
    openscp_navigation_store_tests
    openscp_pane_controller_tests
    openscp_remote_action_controller_tests
    openscp_session_controller_tests
    openscp_transfer_ui_controller_tests
    openscp_terminal_command_builder_tests
    openscp_ui_utilities_tests
    openscp_sync_comparison_tests
    openscp_sync_coordinator_tests
    openscp_local_tree_discovery_tests
    openscp_sftp_integration_tests
    openscp_scp_integration_tests
)

foreach(OPTIONAL_TEST_TARGET
        openscp_ftp_integration_tests
        openscp_ftps_integration_tests
        openscp_curl_backend_common_tests
        openscp_webdav_integration_tests)
    if (TARGET ${OPTIONAL_TEST_TARGET})
        list(APPEND OPENSCP_TEST_BINARY_TARGETS ${OPTIONAL_TEST_TARGET})
    endif()
endforeach()

add_custom_target(openscp_test_binaries
    DEPENDS ${OPENSCP_TEST_BINARY_TARGETS}
)
