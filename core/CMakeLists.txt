cmake_minimum_required(VERSION 3.22)

add_library(openscp_core STATIC
  src/mock/MockSftpClient.cpp
  src/libssh2/Libssh2SftpClient.cpp   # <-- ESTE DEBE ESTAR
)

target_include_directories(openscp_core
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# libssh2 (Homebrew o pkg-config)
find_package(PkgConfig QUIET)
if (PkgConfig_FOUND)
  pkg_check_modules(LIBSSH2 libssh2)
endif()

if (LIBSSH2_FOUND)
  target_include_directories(openscp_core PRIVATE ${LIBSSH2_INCLUDE_DIRS})
  target_link_libraries(openscp_core PRIVATE ${LIBSSH2_LIBRARIES})
else()
  # Rutas típicas en macOS (Apple Silicon)
  find_path(LIBSSH2_INC NAMES libssh2.h PATHS /opt/homebrew/include /usr/local/include)
  find_library(LIBSSH2_LIB NAMES ssh2 PATHS /opt/homebrew/lib /usr/local/lib)
  if (LIBSSH2_INC AND LIBSSH2_LIB)
    target_include_directories(openscp_core PRIVATE ${LIBSSH2_INC})
    target_link_libraries(openscp_core PRIVATE ${LIBSSH2_LIB})
  else()
    message(FATAL_ERROR "libssh2 no encontrado. Instala con 'brew install libssh2'.")
  endif()
endif()