cmake_minimum_required(VERSION 3.22)

add_library(openscp_core STATIC
  src/mock/MockSftpClient.cpp
)

target_include_directories(openscp_core
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# No Qt aquí. Core es C++ puro.

