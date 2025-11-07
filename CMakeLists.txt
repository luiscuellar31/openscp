cmake_minimum_required(VERSION 3.22)
project(OpenSCP_hello LANGUAGES CXX VERSION 0.6.0)

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
  ui/DragAwareTreeView.hpp
  ui/DragAwareTreeView.cpp
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

# Libsecret (Linux): link if available to store secrets securely
find_package(PkgConfig QUIET)
if (PkgConfig_FOUND)
  pkg_check_modules(LIBSECRET QUIET libsecret-1)
  if (LIBSECRET_FOUND)
    target_include_directories(openscp_hello PRIVATE ${LIBSECRET_INCLUDE_DIRS})
    target_link_libraries(openscp_hello PRIVATE ${LIBSECRET_LIBRARIES})
    target_compile_definitions(openscp_hello PRIVATE HAVE_LIBSECRET=1)
  endif()
endif()

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

###############################################################################
# macOS bundle (OpenSCP.app)
# - Produce a proper .app bundle directly from CMake
# - Inject Info.plist from assets/macos/Info.plist.in
# - Set RPATH so dylibs are resolved from @executable_path/../Frameworks
###############################################################################
if(APPLE)
  # Reproducible defaults (override via -D on the CMake command line if needed)
  if(NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS version" FORCE)
  endif()
  if(NOT DEFINED CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "Target architectures" FORCE)
  endif()

  # Bundle identifier propagated into Info.plist
  set(BUNDLE_ID "com.luiscuellar.openscp" CACHE STRING "macOS bundle identifier")

  # Configure Info.plist from template
  set(_INFO_PLIST_IN  "${CMAKE_SOURCE_DIR}/assets/macos/Info.plist.in")
  set(_INFO_PLIST_OUT "${CMAKE_CURRENT_BINARY_DIR}/OpenSCP-Info.plist")

  # Variables used by Info.plist.in (@VAR@)
  set(BUNDLE_IDENTIFIER       "${BUNDLE_ID}")
  set(PRODUCT_NAME            "OpenSCP")
  set(EXECUTABLE_NAME         "OpenSCP")
  set(ICON_FILE               "OpenSCP")                    # without .icns extension
  set(MINIMUM_SYSTEM_VERSION  "${CMAKE_OSX_DEPLOYMENT_TARGET}")
  set(VERSION_SHORT           "${PROJECT_VERSION}")
  set(VERSION                 "${PROJECT_VERSION}")
  string(TIMESTAMP CURRENT_YEAR "%Y")

  configure_file("${_INFO_PLIST_IN}" "${_INFO_PLIST_OUT}" @ONLY)

  # Convert target into a macOS bundle and set output name to OpenSCP
  set_target_properties(openscp_hello PROPERTIES
    MACOSX_BUNDLE TRUE
    OUTPUT_NAME "OpenSCP"                                 # OpenSCP.app and inner binary 'OpenSCP'
    MACOSX_BUNDLE_INFO_PLIST "${_INFO_PLIST_OUT}"

    # Ensure runtime search path resolves to bundled Frameworks directory
    INSTALL_RPATH "@executable_path/../Frameworks"
    BUILD_WITH_INSTALL_RPATH TRUE
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
  assets/icons/action-connect.svg
  assets/icons/action-copy.svg
  assets/icons/action-delete.svg
  assets/icons/action-disconnect.svg
  assets/icons/action-download.svg
  assets/icons/action-go-up.svg
  assets/icons/action-open-about-us.svg
  assets/icons/action-move-to-left.svg
  assets/icons/action-move-to-right.svg
  assets/icons/action-new-file.svg
  assets/icons/action-new-folder.svg
  assets/icons/action-open-folder.svg
  assets/icons/action-open-folder-remote.svg
  assets/icons/action-open-saved-sites.svg
  assets/icons/action-open-transfer-queue.svg
  assets/icons/action-open-settings.svg
  assets/icons/action-rename.svg
  assets/icons/action-upload.svg
)

# Aliases for stable paths: /assets/icons/<name>.svg
foreach(icon ${ICON_FILES})
  get_filename_component(_name ${icon} NAME)
  set_source_files_properties(${icon} PROPERTIES QT_RESOURCE_ALIAS icons/${_name})
endforeach()

qt_add_resources(openscp_hello app_icons
  PREFIX "/assets"
  FILES ${ICON_FILES}
)

# Program assets (PNG icon for About dialog)
qt_add_resources(openscp_hello app_images
  PREFIX "/assets/program"
  FILES
    assets/program/icon-openscp-2048.png
)

# Ensure predictable in-resource path for the program icon
set_source_files_properties(assets/program/icon-openscp-2048.png
  PROPERTIES QT_RESOURCE_ALIAS "icon-openscp-2048.png")

if(UNIX) #only linux

set(FILE_DESKTOP
  assets/linux/openscp.desktop)

set(ICON_DESKTOP_APP
  assets/program/icon-openscp-2048.png)
#dont rule install
install(TARGETS openscp_hello DESTINATION bin)
#.svg are scalable
#new folder openscp in share/icons/openscp/scalable/apps  
install(FILES ${ICON_FILES} DESTINATION share/icons/openscp/scalable/apps)
#dont good. .png only .svg no mapping .desktop no show. 
#Change Icon Format SVG
#install(FILES ${ICON_DESKTOP_APP} DESTINATION share/icons/openscp/hicolor/2048x2048/apps)
install(FILES ${FILE_DESKTOP} DESTINATION share/applications)
endif()