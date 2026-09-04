cmake_minimum_required(VERSION 3.22)

option(OPENSCP_ENABLE_MOCK "Build mock SFTP client" OFF)
option(OPENSCP_ENABLE_FTP_BACKEND
       "Build FTP/FTPS backend module (requires libcurl development files)" ON)
option(OPENSCP_ENABLE_WEBDAV_BACKEND
       "Build WebDAV backend module (requires libcurl + tinyxml2 development files)" ON)

set(OPENSCP_CORE_PUBLIC_HEADERS
    include/openscp/ClientFactory.hpp
    include/openscp/KnownHostsUtils.hpp
    include/openscp/Protocol.hpp
    include/openscp/RemoteClient.hpp
    include/openscp/RemoteError.hpp
    include/openscp/RemoteFileInfo.hpp
    include/openscp/RemotePath.hpp
    include/openscp/RuntimeLogging.hpp
    include/openscp/SecureString.hpp
    include/openscp/SessionOptions.hpp
)

set(OPENSCP_CORE_SRCS
    src/ClientFactory.cpp
    src/common/RemoteListingLimits.hpp
    src/common/RemotePath.cpp
    src/common/SafeLocalFile.cpp
    src/common/SafeLocalFile.hpp
    src/common/UniqueFile.hpp
    src/libssh2/detail/Libssh2ErrorClassifier.cpp
    src/libssh2/detail/Libssh2ErrorClassifier.hpp
    src/libssh2/detail/Libssh2InputSafety.hpp
    src/libssh2/Libssh2ScpClient.cpp   # SCP backend module
    src/libssh2/Libssh2ScpClient.hpp
    src/libssh2/Libssh2SftpClient.cpp   # real implementation
    src/libssh2/Libssh2SftpClient.hpp
)

set(OPENSCP_HAS_CURL_FTP 0)
set(OPENSCP_HAS_CURL_WEBDAV 0)

set(_openscp_need_curl OFF)
if (OPENSCP_ENABLE_FTP_BACKEND OR OPENSCP_ENABLE_WEBDAV_BACKEND)
    set(_openscp_need_curl ON)
endif()

if (_openscp_need_curl)
    find_package(CURL QUIET)
    if (CURL_FOUND)
        list(APPEND OPENSCP_CORE_SRCS
            src/curl/CurlBackendCommon.cpp
            src/curl/CurlBackendCommon.hpp
        )
    endif()
endif()

if (OPENSCP_ENABLE_FTP_BACKEND)
    if (CURL_FOUND)
        list(APPEND OPENSCP_CORE_SRCS
            src/curl/CurlFtpClient.cpp        # FTP/FTPS backend module
            src/curl/CurlFtpClient.hpp
        )
        set(OPENSCP_HAS_CURL_FTP 1)
        message(STATUS "OpenSCP: FTP/FTPS backend enabled (libcurl found)")
    else()
        message(WARNING
            "OpenSCP: libcurl not found; FTP/FTPS backend disabled. "
            "Install curl dev package to enable Protocol::Ftp/Protocol::Ftps backends.")
    endif()
else()
    message(STATUS
        "OpenSCP: FTP/FTPS backend disabled by OPENSCP_ENABLE_FTP_BACKEND=OFF")
endif()

set(OPENSCP_TINYXML2_TARGET "")
if (OPENSCP_ENABLE_WEBDAV_BACKEND)
    if (CURL_FOUND)
        find_package(tinyxml2 CONFIG QUIET)
        if (TARGET tinyxml2::tinyxml2)
            set(OPENSCP_TINYXML2_TARGET tinyxml2::tinyxml2)
        else()
            find_path(OPENSCP_TINYXML2_INCLUDE_DIR
                NAMES tinyxml2.h
                PATHS /opt/homebrew/include /opt/homebrew/opt/tinyxml2/include
                      /usr/local/include /usr/local/opt/tinyxml2/include
                      /usr/include
            )
            find_library(OPENSCP_TINYXML2_LIBRARY
                NAMES tinyxml2
                PATHS /opt/homebrew/lib /opt/homebrew/opt/tinyxml2/lib
                      /usr/local/lib /usr/local/opt/tinyxml2/lib /usr/lib
                      /usr/lib/x86_64-linux-gnu
            )
            if (OPENSCP_TINYXML2_INCLUDE_DIR AND OPENSCP_TINYXML2_LIBRARY)
                add_library(openscp_tinyxml2 UNKNOWN IMPORTED)
                set_target_properties(openscp_tinyxml2 PROPERTIES
                    IMPORTED_LOCATION "${OPENSCP_TINYXML2_LIBRARY}"
                    INTERFACE_INCLUDE_DIRECTORIES
                        "${OPENSCP_TINYXML2_INCLUDE_DIR}"
                )
                set(OPENSCP_TINYXML2_TARGET openscp_tinyxml2)
            endif()
        endif()

        if (OPENSCP_TINYXML2_TARGET)
            list(APPEND OPENSCP_CORE_SRCS
                src/curl/CurlWebDavClient.cpp   # WebDAV backend module
                src/curl/CurlWebDavClient.hpp
            )
            set(OPENSCP_HAS_CURL_WEBDAV 1)
            message(STATUS
                "OpenSCP: WebDAV backend enabled (libcurl + tinyxml2 found)")
        else()
            message(WARNING
                "OpenSCP: tinyxml2 not found; WebDAV backend disabled. "
                "Install tinyxml2 dev package to enable Protocol::WebDav backend.")
        endif()
    else()
        message(WARNING
            "OpenSCP: libcurl not found; WebDAV backend disabled. "
            "Install curl dev package to enable Protocol::WebDav backend.")
    endif()
else()
    message(STATUS
        "OpenSCP: WebDAV backend disabled by OPENSCP_ENABLE_WEBDAV_BACKEND=OFF")
endif()

if (OPENSCP_ENABLE_MOCK)
    list(APPEND OPENSCP_CORE_SRCS
        src/mock/MockSftpClient.cpp
        src/mock/MockSftpClient.hpp
    )
endif()

add_library(openscp_core STATIC
    ${OPENSCP_CORE_PUBLIC_HEADERS}
    ${OPENSCP_CORE_SRCS}
)

target_include_directories(openscp_core
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_definitions(openscp_core
    PUBLIC OPENSCP_HAS_CURL_FTP=${OPENSCP_HAS_CURL_FTP}
           OPENSCP_HAS_CURL_WEBDAV=${OPENSCP_HAS_CURL_WEBDAV}
)
set_property(TARGET openscp_core PROPERTY OPENSCP_HAS_CURL_FTP
             ${OPENSCP_HAS_CURL_FTP})
set_property(TARGET openscp_core PROPERTY OPENSCP_HAS_CURL_WEBDAV
             ${OPENSCP_HAS_CURL_WEBDAV})

# libssh2 (Homebrew/pkg-config) with robust link flags on macOS/Homebrew.
find_package(PkgConfig QUIET)
if (PkgConfig_FOUND)
    pkg_check_modules(LIBSSH2 QUIET IMPORTED_TARGET libssh2)
endif()

# OpenSSL (for HMAC/RAND used in hashed known_hosts fallback)
find_package(OpenSSL REQUIRED)
if (OpenSSL_FOUND)
    if (TARGET OpenSSL::Crypto)
        target_link_libraries(openscp_core PRIVATE OpenSSL::Crypto)
    else()
        # Fallback linking libcrypto directly
        find_library(OPENSSL_CRYPTO NAMES crypto libcrypto PATHS /opt/homebrew/lib /usr/local/lib)
        if (OPENSSL_CRYPTO)
            target_link_libraries(openscp_core PRIVATE ${OPENSSL_CRYPTO})
        endif()
    endif()
endif()

# libcurl (FTP/FTPS backend, optional)
if (OPENSCP_HAS_CURL_FTP)
    target_link_libraries(openscp_core PRIVATE CURL::libcurl)
endif()
if (OPENSCP_HAS_CURL_WEBDAV)
    target_link_libraries(openscp_core PRIVATE CURL::libcurl
                                               ${OPENSCP_TINYXML2_TARGET})
endif()

if (TARGET PkgConfig::LIBSSH2)
    # Preferred path: preserve all pkg-config link/search flags (e.g. Homebrew -L paths).
    target_link_libraries(openscp_core PRIVATE PkgConfig::LIBSSH2)
elseif (LIBSSH2_FOUND)
    target_include_directories(openscp_core PRIVATE ${LIBSSH2_INCLUDE_DIRS})
    if (LIBSSH2_LIBRARY_DIRS)
        target_link_directories(openscp_core PRIVATE ${LIBSSH2_LIBRARY_DIRS})
    endif()
    target_link_libraries(openscp_core PRIVATE ${LIBSSH2_LIBRARIES})
else()
    # Typical paths on macOS (Apple Silicon)
    find_path(LIBSSH2_INC NAMES libssh2.h
                        PATHS /opt/homebrew/include /opt/homebrew/opt/libssh2/include
                                    /usr/local/include /usr/local/opt/libssh2/include)
    find_library(LIBSSH2_LIB NAMES ssh2
                              PATHS /opt/homebrew/lib /opt/homebrew/opt/libssh2/lib
                                          /usr/local/lib /usr/local/opt/libssh2/lib)
    if (LIBSSH2_INC AND LIBSSH2_LIB)
        target_include_directories(openscp_core PRIVATE ${LIBSSH2_INC})
        target_link_libraries(openscp_core PRIVATE ${LIBSSH2_LIB})
    else()
        message(FATAL_ERROR
            "libssh2 was not found. Install its development package "
            "(for example, 'brew install libssh2').")
    endif()
endif()
