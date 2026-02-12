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
