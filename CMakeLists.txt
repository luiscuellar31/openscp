cmake_minimum_required(VERSION 3.22)
project(OpenSCP_hello LANGUAGES CXX VERSION 0.5.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets LinguistTools)

option(OPEN_SCP_BUILD_SECURE_ONLY "Disable insecure fallback for secrets storage" ON)

add_subdirectory(core)

add_executable(openscp_hello
  ui/main.cpp
  ui/MainWindow.hpp
  ui/MainWindow.cpp
  ui/ConnectionDialog.hpp
  ui/ConnectionDialog.cpp
  ui/RemoteModel.hpp
  ui/RemoteModel.cpp
  ui/PermissionsDialog.hpp
  ui/PermissionsDialog.cpp
  ui/TransferManager.hpp
  ui/TransferManager.cpp
  ui/TransferQueueDialog.hpp
  ui/TransferQueueDialog.cpp
  ui/SiteManagerDialog.hpp
  ui/SiteManagerDialog.cpp
  ui/SecretStore.hpp
  ui/SecretStore.cpp
  ui/SettingsDialog.hpp
  ui/SettingsDialog.cpp
  ui/AboutDialog.hpp
  ui/AboutDialog.cpp
)

# Generate a header with the application version from CMake project version
configure_file(ui/AppVersion.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/AppVersion.hpp @ONLY)

target_include_directories(openscp_hello PRIVATE
  ${CMAKE_SOURCE_DIR}/core/include
  ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(openscp_hello PRIVATE
  Qt6::Widgets
  openscp_core
)

if (APPLE)
  target_link_libraries(openscp_hello PRIVATE
    "-framework Security"
  )
endif()

if (OPEN_SCP_BUILD_SECURE_ONLY)
  target_compile_definitions(openscp_hello PRIVATE OPEN_SCP_BUILD_SECURE_ONLY=1)
endif()

# Translations (es/en): compile TS to QM and embed as a resource.
# .ts files live in the top-level 'translations/' folder.
qt_add_translation(QM_FILES
  translations/openscp_es.ts
  translations/openscp_en.ts
)
# Set relative aliases for resources to avoid absolute paths in the qrc
set(QM_ES "")
set(QM_EN "")
foreach(qm ${QM_FILES})
  if(qm MATCHES "openscp_es\\.qm$")
    set(QM_ES "${qm}")
  elseif(qm MATCHES "openscp_en\\.qm$")
    set(QM_EN "${qm}")
  endif()
endforeach()
if(QM_ES)
  set_source_files_properties(${QM_ES} PROPERTIES QT_RESOURCE_ALIAS "openscp_es.qm")
endif()
if(QM_EN)
  set_source_files_properties(${QM_EN} PROPERTIES QT_RESOURCE_ALIAS "openscp_en.qm")
endif()
qt_add_resources(openscp_hello i18n
  PREFIX "/i18n"
  FILES ${QM_FILES}
)

# Application icons (SVG). Packaged as Qt resources.
set(ICON_FILES
  icons/using/action-connect.svg
  icons/using/action-copy.svg
  icons/using/action-delete.svg
  icons/using/action-disconnect.svg
  icons/using/action-download.svg
  icons/using/action-go-up.svg
  icons/using/action-open-about-us.svg
  icons/using/action-move-to-left.svg
  icons/using/action-move-to-right.svg
  icons/using/action-new-file.svg
  icons/using/action-new-folder.svg
  icons/using/action-open-folder.svg
  icons/using/action-open-folder-remote.svg
  icons/using/action-open-saved-sites.svg
  icons/using/action-open-transfer-queue.svg
  icons/using/action-open-settings.svg
  icons/using/action-rename.svg
  icons/using/action-upload.svg
)

# Aliases for stable paths: /icons/using/<name>.svg
foreach(icon ${ICON_FILES})
  get_filename_component(_name ${icon} NAME)
  set_source_files_properties(${icon} PROPERTIES QT_RESOURCE_ALIAS using/${_name})
endforeach()

qt_add_resources(openscp_hello app_icons
  PREFIX "/icons"
  FILES ${ICON_FILES}
)
