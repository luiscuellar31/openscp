include(CMakeParseArguments)

function(openscp_add_test TARGET_NAME)
    set(options CUSTOM_MAIN)
    set(one_value_args SKIP_RETURN_CODE)
    set(multi_value_args SOURCES INCLUDE_DIRECTORIES LINK_LIBRARIES LABELS)
    cmake_parse_arguments(TEST
        "${options}"
        "${one_value_args}"
        "${multi_value_args}"
        ${ARGN}
    )

    if(TEST_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "openscp_add_test(${TARGET_NAME}) received unknown arguments: "
            "${TEST_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT TEST_SOURCES)
        message(FATAL_ERROR
            "openscp_add_test(${TARGET_NAME}) requires at least one source")
    endif()

    add_executable(${TARGET_NAME} ${TEST_SOURCES})
    if(TEST_INCLUDE_DIRECTORIES)
        target_include_directories(${TARGET_NAME} PRIVATE
            ${TEST_INCLUDE_DIRECTORIES})
    endif()
    if(TEST_LINK_LIBRARIES)
        target_link_libraries(${TARGET_NAME} PRIVATE ${TEST_LINK_LIBRARIES})
    endif()

    add_test(NAME ${TARGET_NAME} COMMAND ${TARGET_NAME})
    if(TEST_LABELS)
        set_tests_properties(${TARGET_NAME} PROPERTIES LABELS "${TEST_LABELS}")
    else()
        set_tests_properties(${TARGET_NAME} PROPERTIES LABELS "unit")
    endif()
    if(DEFINED TEST_SKIP_RETURN_CODE AND
       NOT "${TEST_SKIP_RETURN_CODE}" STREQUAL "")
        set_tests_properties(${TARGET_NAME} PROPERTIES
            SKIP_RETURN_CODE "${TEST_SKIP_RETURN_CODE}")
    endif()

    set_property(GLOBAL APPEND PROPERTY OPENSCP_TEST_BINARY_TARGETS
        ${TARGET_NAME})
    foreach(TEST_SOURCE IN LISTS TEST_SOURCES)
        get_filename_component(TEST_SOURCE_ABSOLUTE "${TEST_SOURCE}" ABSOLUTE
            BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        if(TEST_SOURCE_ABSOLUTE MATCHES "_tests\\.cpp$")
            set_property(GLOBAL APPEND PROPERTY OPENSCP_REGISTERED_TEST_SOURCES
                "${TEST_SOURCE_ABSOLUTE}")
            if(NOT TEST_CUSTOM_MAIN)
                set_property(GLOBAL APPEND PROPERTY OPENSCP_HARNESS_TEST_SOURCES
                    "${TEST_SOURCE_ABSOLUTE}")
            endif()
        endif()
    endforeach()
endfunction()

openscp_add_test(openscp_core_tests
    SOURCES core_mock_tests.cpp
    INCLUDE_DIRECTORIES
        ${CMAKE_SOURCE_DIR}/core/include
        ${CMAKE_SOURCE_DIR}/core/src/common
        ${CMAKE_SOURCE_DIR}/core/src/libssh2/detail
    LINK_LIBRARIES openscp_core
)

foreach(TEST_COMPONENT IN ITEMS
        transfer_manager
        remote_operation_controller
        remote_model
        saved_sites_persistence
        site_credential_repository
        navigation_scope
        navigation_store
        pane_controller
        remote_action_controller
        session_controller
        connection_coordinators
        transfer_ui_controller
        terminal_command_builder
        sync_coordinator
        local_tree_discovery)
    openscp_add_test(openscp_${TEST_COMPONENT}_tests
        SOURCES ${TEST_COMPONENT}_tests.cpp
        LINK_LIBRARIES openscp_ui_logic
    )
endforeach()

openscp_add_test(openscp_ui_utilities_tests
    SOURCES
        ui_utilities_tests.cpp
        ${CMAKE_SOURCE_DIR}/ui/RemotePath.cpp
        ${CMAKE_SOURCE_DIR}/ui/RemotePath.hpp
        ${CMAKE_SOURCE_DIR}/ui/UiFormatters.cpp
        ${CMAKE_SOURCE_DIR}/ui/UiFormatters.hpp
    INCLUDE_DIRECTORIES ${CMAKE_SOURCE_DIR}/ui
    LINK_LIBRARIES Qt6::Core openscp_core
)

openscp_add_test(openscp_sync_comparison_tests
    SOURCES sync_comparison_tests.cpp
    LINK_LIBRARIES openscp_sync_logic
)

openscp_add_test(openscp_path_navigation_model_tests
    SOURCES path_navigation_model_tests.cpp
    LINK_LIBRARIES openscp_ui_logic
)

openscp_add_test(openscp_sftp_integration_tests
    CUSTOM_MAIN
    SOURCES libssh2_integration_tests.cpp
    INCLUDE_DIRECTORIES ${CMAKE_SOURCE_DIR}/core/include
    LINK_LIBRARIES openscp_core
    LABELS integration sftp
    SKIP_RETURN_CODE 77
)

openscp_add_test(openscp_scp_integration_tests
    CUSTOM_MAIN
    SOURCES libssh2_scp_integration_tests.cpp
    INCLUDE_DIRECTORIES ${CMAKE_SOURCE_DIR}/core/include
    LINK_LIBRARIES openscp_core
    LABELS integration scp
    SKIP_RETURN_CODE 77
)

get_target_property(OPENSCP_CORE_HAS_CURL_FTP
    openscp_core OPENSCP_HAS_CURL_FTP)
if(OPENSCP_CORE_HAS_CURL_FTP)
    openscp_add_test(openscp_ftp_integration_tests
        CUSTOM_MAIN
        SOURCES curl_ftp_integration_tests.cpp
        INCLUDE_DIRECTORIES ${CMAKE_SOURCE_DIR}/core/include
        LINK_LIBRARIES openscp_core
        LABELS integration ftp
        SKIP_RETURN_CODE 77
    )
    openscp_add_test(openscp_ftps_integration_tests
        CUSTOM_MAIN
        SOURCES curl_ftps_integration_tests.cpp
        INCLUDE_DIRECTORIES ${CMAKE_SOURCE_DIR}/core/include
        LINK_LIBRARIES openscp_core
        LABELS integration ftps
        SKIP_RETURN_CODE 77
    )
else()
    message(STATUS
        "OpenSCP tests: skipping FTP/FTPS integration tests "
        "(FTP/FTPS backend disabled)")
endif()

get_target_property(OPENSCP_CORE_HAS_CURL_WEBDAV
    openscp_core OPENSCP_HAS_CURL_WEBDAV)
if(OPENSCP_CORE_HAS_CURL_FTP OR OPENSCP_CORE_HAS_CURL_WEBDAV)
    openscp_add_test(openscp_curl_backend_common_tests
        SOURCES curl_backend_common_tests.cpp
        INCLUDE_DIRECTORIES
            ${CMAKE_SOURCE_DIR}/core/include
            ${CMAKE_SOURCE_DIR}/core/src/curl
        LINK_LIBRARIES openscp_core
    )
endif()

if(OPENSCP_CORE_HAS_CURL_WEBDAV)
    openscp_add_test(openscp_webdav_integration_tests
        CUSTOM_MAIN
        SOURCES curl_webdav_integration_tests.cpp
        INCLUDE_DIRECTORIES ${CMAKE_SOURCE_DIR}/core/include
        LINK_LIBRARIES openscp_core
        LABELS integration webdav
        SKIP_RETURN_CODE 77
    )
else()
    message(STATUS
        "OpenSCP tests: skipping WebDAV integration tests "
        "(WebDAV backend disabled)")
endif()

# A new test source must be registered above. CONFIGURE_DEPENDS makes CMake
# re-run this audit automatically when a *_tests.cpp file is added.
file(GLOB OPENSCP_DISCOVERED_TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*_tests.cpp")
get_property(OPENSCP_REGISTERED_TEST_SOURCES GLOBAL PROPERTY
    OPENSCP_REGISTERED_TEST_SOURCES)
list(REMOVE_DUPLICATES OPENSCP_REGISTERED_TEST_SOURCES)
foreach(TEST_SOURCE IN LISTS OPENSCP_DISCOVERED_TEST_SOURCES)
    list(FIND OPENSCP_REGISTERED_TEST_SOURCES "${TEST_SOURCE}"
        TEST_SOURCE_INDEX)
    if(TEST_SOURCE_INDEX EQUAL -1)
        message(FATAL_ERROR
            "Unregistered test source: ${TEST_SOURCE}. "
            "Add it with openscp_add_test(...).")
    endif()
endforeach()

# Unit-test sources use TestHarness and OPENSCP_TEST static registration. The
# CUSTOM_MAIN escape hatch is reserved for protocol scenarios whose exit code
# 77 preserves CTest's service/credential skip contract.
get_property(OPENSCP_HARNESS_TEST_SOURCES GLOBAL PROPERTY
    OPENSCP_HARNESS_TEST_SOURCES)
list(REMOVE_DUPLICATES OPENSCP_HARNESS_TEST_SOURCES)
foreach(TEST_SOURCE IN LISTS OPENSCP_HARNESS_TEST_SOURCES)
    file(READ "${TEST_SOURCE}" TEST_SOURCE_CONTENTS)
    if(NOT TEST_SOURCE_CONTENTS MATCHES "openscp::test::TestHarness")
        message(FATAL_ERROR
            "Test source bypasses TestHarness: ${TEST_SOURCE}. "
            "Use OPENSCP_TEST(...) auto-registration or mark a protocol "
            "integration scenario CUSTOM_MAIN.")
    endif()
endforeach()

get_property(OPENSCP_TEST_BINARY_TARGETS GLOBAL PROPERTY
    OPENSCP_TEST_BINARY_TARGETS)
add_custom_target(openscp_test_binaries
    DEPENDS ${OPENSCP_TEST_BINARY_TARGETS}
)
