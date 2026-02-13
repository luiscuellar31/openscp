add_executable(openscp_core_tests
    core_mock_tests.cpp
)

target_include_directories(openscp_core_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
)

target_link_libraries(openscp_core_tests PRIVATE
    openscp_core
)

add_test(NAME openscp_core_tests COMMAND openscp_core_tests)

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
