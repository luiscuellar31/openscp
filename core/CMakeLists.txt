cmake_minimum_required(VERSION 3.22)

option(OPEN_SCP_ENABLE_MOCK "Build mock SFTP client" OFF)

set(OPEN_SCP_CORE_SRCS
  src/libssh2/Libssh2SftpClient.cpp   # real implementation
)

if (OPEN_SCP_ENABLE_MOCK)
  list(APPEND OPEN_SCP_CORE_SRCS src/mock/MockSftpClient.cpp)
endif()

add_library(openscp_core STATIC ${OPEN_SCP_CORE_SRCS})

target_include_directories(openscp_core
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

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
    message(FATAL_ERROR "libssh2 no encontrado. Instala con 'brew install libssh2'.")
  endif()
endif()
