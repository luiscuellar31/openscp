get_target_property(OPENSCP_FUZZ_HAS_FTP openscp_core OPENSCP_HAS_CURL_FTP)
get_target_property(OPENSCP_FUZZ_HAS_WEBDAV
                    openscp_core OPENSCP_HAS_CURL_WEBDAV)

function(openscp_add_listing_fuzzer TARGET_NAME SOURCE)
    add_executable(${TARGET_NAME} ${SOURCE})
    target_include_directories(${TARGET_NAME} PRIVATE
        ${CMAKE_SOURCE_DIR}/core/src
    )
    target_link_libraries(${TARGET_NAME} PRIVATE openscp_core)
    target_compile_options(${TARGET_NAME} PRIVATE
        -g
        -fno-omit-frame-pointer
        -fsanitize=fuzzer-no-link,address,undefined
    )
    target_link_options(${TARGET_NAME} PRIVATE
        -fsanitize=fuzzer,address,undefined
    )
    set_target_properties(${TARGET_NAME} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/fuzz
    )
endfunction()

if(OPENSCP_FUZZ_HAS_FTP)
    openscp_add_listing_fuzzer(openscp_ftp_listing_fuzzer
                               ftp_listing_fuzz.cpp)
endif()

if(OPENSCP_FUZZ_HAS_WEBDAV)
    openscp_add_listing_fuzzer(openscp_webdav_listing_fuzzer
                               webdav_listing_fuzz.cpp)
endif()

if(NOT OPENSCP_FUZZ_HAS_FTP AND NOT OPENSCP_FUZZ_HAS_WEBDAV)
    message(FATAL_ERROR
        "Listing fuzzers require the FTP or WebDAV backend")
endif()
