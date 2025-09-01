cmake_minimum_required(VERSION 3.22)
project(OpenSCP_hello LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

add_subdirectory(core)

add_executable(openscp_hello
  ui/main.cpp
  ui/MainWindow.hpp
  ui/MainWindow.cpp
  ui/ConnectionDialog.hpp
  ui/ConnectionDialog.cpp
  ui/RemoteModel.hpp
  ui/RemoteModel.cpp
)

target_include_directories(openscp_hello PRIVATE
  ${CMAKE_SOURCE_DIR}/core/include
)

target_link_libraries(openscp_hello PRIVATE
  Qt6::Widgets
  openscp_core
)