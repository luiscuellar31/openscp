cmake_minimum_required(VERSION 3.22)

# These values influence compiler and SDK selection, so establish them before
# project() enables CXX. CMake may create empty cache entries for both variables;
# testing only DEFINED would therefore leave an invalid empty deployment target
# in the generated Info.plist.
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
        set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING
            "Minimum macOS version" FORCE)
    endif()
endif()

project(OpenSCP LANGUAGES CXX VERSION 1.0.0)

if(WIN32)
    option(OPENSCP_ENABLE_EXPERIMENTAL_WINDOWS_PORT
        "Allow configuration of the unsupported Windows port scaffold"
        OFF)
    if(NOT OPENSCP_ENABLE_EXPERIMENTAL_WINDOWS_PORT)
        message(FATAL_ERROR
            "OpenSCP does not currently support Windows. The modular port "
            "is intentionally gated to prevent accidental release builds. "
            "Port developers may opt in with "
            "-DOPENSCP_ENABLE_EXPERIMENTAL_WINDOWS_PORT=ON.")
    endif()
    message(WARNING
        "Experimental Windows scaffold enabled: this configuration is not "
        "a supported OpenSCP release target.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(OPENSCP_AUTHOR_NAME "luiscuellar31" CACHE STRING "Author display name")
set(OPENSCP_AUTHOR_URL "https://github.com/luiscuellar31" CACHE STRING
    "Author profile URL")
set(OPENSCP_REPOSITORY_URL "https://github.com/luiscuellar31/openscp" CACHE STRING
    "Project repository URL")
set(OPENSCP_ISSUES_URL "${OPENSCP_REPOSITORY_URL}/issues" CACHE STRING
    "Project issues URL")

if(CMAKE_BUILD_TYPE)
    set(OPENSCP_BUILD_TYPE_STR "${CMAKE_BUILD_TYPE}")
else()
    set(OPENSCP_BUILD_TYPE_STR "Multi-config")
endif()

set(OPENSCP_GIT_COMMIT "unknown")
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_SOURCE_DIR}" rev-parse --short=12 HEAD
        OUTPUT_VARIABLE _git_commit
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(_git_commit)
        set(OPENSCP_GIT_COMMIT "${_git_commit}")
    endif()
endif()

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(OPENSCP_BUILD_TESTS "Build and run automated tests (CTest)" OFF)
option(OPENSCP_WERROR "Treat first-party compiler warnings as errors" OFF)

add_library(openscp_project_warnings INTERFACE)
if(MSVC)
    target_compile_options(openscp_project_warnings INTERFACE
        /W4
        /permissive-
    )
    if(OPENSCP_WERROR)
        target_compile_options(openscp_project_warnings INTERFACE /WX)
    endif()
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(openscp_project_warnings INTERFACE
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wformat=2
        -Wundef
        -Wconversion
        -Wsign-conversion
        -Wcast-qual
        -Wold-style-cast
        -Wnon-virtual-dtor
        -Woverloaded-virtual
    )
    if(OPENSCP_WERROR)
        target_compile_options(openscp_project_warnings INTERFACE -Werror)
    endif()
endif()

add_library(openscp_project_hardening INTERFACE)
if(MSVC)
    target_compile_options(openscp_project_hardening INTERFACE /GS)
    target_link_options(openscp_project_hardening INTERFACE
        /DYNAMICBASE
        /NXCOMPAT
    )
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    set(OPENSCP_FORTIFY_SOURCE_LEVEL "AUTO" CACHE STRING
        "Linux _FORTIFY_SOURCE level (AUTO, 0, 1, 2, or 3)")
    set_property(CACHE OPENSCP_FORTIFY_SOURCE_LEVEL PROPERTY STRINGS
        AUTO 0 1 2 3)
    set(_openscp_fortify_source_level 0)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        if(OPENSCP_FORTIFY_SOURCE_LEVEL STREQUAL "AUTO")
            include(CheckCXXSourceCompiles)
            set(_openscp_saved_required_definitions
                "${CMAKE_REQUIRED_DEFINITIONS}")
            set(_openscp_saved_required_flags "${CMAKE_REQUIRED_FLAGS}")
            set(CMAKE_REQUIRED_DEFINITIONS "-D_FORTIFY_SOURCE=3")
            set(CMAKE_REQUIRED_FLAGS "-O2 -Werror")
            check_cxx_source_compiles([[
                #include <features.h>
                #if !defined(__USE_FORTIFY_LEVEL) || __USE_FORTIFY_LEVEL < 3
                #error _FORTIFY_SOURCE=3 is not fully supported
                #endif
                int main() { return 0; }
            ]] OPENSCP_HAS_FORTIFY_SOURCE_3)
            set(CMAKE_REQUIRED_DEFINITIONS
                "${_openscp_saved_required_definitions}")
            set(CMAKE_REQUIRED_FLAGS "${_openscp_saved_required_flags}")
            if(OPENSCP_HAS_FORTIFY_SOURCE_3)
                set(_openscp_fortify_source_level 3)
            else()
                set(_openscp_fortify_source_level 2)
            endif()
        elseif(OPENSCP_FORTIFY_SOURCE_LEVEL MATCHES "^[0-3]$")
            set(_openscp_fortify_source_level
                "${OPENSCP_FORTIFY_SOURCE_LEVEL}")
        else()
            message(FATAL_ERROR
                "OPENSCP_FORTIFY_SOURCE_LEVEL must be AUTO, 0, 1, 2, or 3")
        endif()
        message(STATUS
            "OpenSCP: _FORTIFY_SOURCE=${_openscp_fortify_source_level}")
    endif()
    target_compile_options(openscp_project_hardening INTERFACE
        -fstack-protector-strong
        $<$<PLATFORM_ID:Linux>:-fPIE>
    )
    if(_openscp_fortify_source_level GREATER 0)
        target_compile_definitions(openscp_project_hardening INTERFACE
            $<$<AND:$<PLATFORM_ID:Linux>,$<NOT:$<CONFIG:Debug>>>:_FORTIFY_SOURCE=${_openscp_fortify_source_level}>
        )
    endif()
    target_link_options(openscp_project_hardening INTERFACE
        $<$<PLATFORM_ID:Linux>:-pie>
        $<$<PLATFORM_ID:Linux>:-Wl,-z,relro,-z,now,-z,noexecstack>
    )
endif()

# Apply shared quality and hardening policies to every first-party target
# declared below, including targets created by core and tests subdirectories.
link_libraries(openscp_project_warnings openscp_project_hardening)

set(OPENSCP_RECOMMENDED_QT_VERSION "6.8.3" CACHE STRING
    "Qt version used for official self-contained OpenSCP artifacts")
option(OPENSCP_ENFORCE_RECOMMENDED_QT_VERSION
       "Reject a Qt version other than OPENSCP_RECOMMENDED_QT_VERSION" OFF)

find_package(Qt6 REQUIRED COMPONENTS Widgets Svg LinguistTools)
if(OPENSCP_ENFORCE_RECOMMENDED_QT_VERSION AND
   NOT Qt6_VERSION VERSION_EQUAL OPENSCP_RECOMMENDED_QT_VERSION)
    message(FATAL_ERROR
        "Official OpenSCP artifacts require Qt "
        "${OPENSCP_RECOMMENDED_QT_VERSION}, but CMake found Qt "
        "${Qt6_VERSION}. Community builds may use a compatible Qt 6 by "
        "leaving OPENSCP_ENFORCE_RECOMMENDED_QT_VERSION=OFF.")
endif()
message(STATUS
    "OpenSCP: Qt ${Qt6_VERSION} (official artifact baseline: "
    "${OPENSCP_RECOMMENDED_QT_VERSION})")
include(GNUInstallDirs)

add_custom_target(openscp_format_check
    COMMAND "${CMAKE_SOURCE_DIR}/scripts/checks/cpp-quality.sh" --format
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Checking all first-party C++ files with clang-format 17"
    VERBATIM
)

add_custom_target(openscp_static_analysis
    COMMAND "${CMAKE_SOURCE_DIR}/scripts/checks/cpp-quality.sh"
            --tidy --cppcheck --build-dir "${CMAKE_BINARY_DIR}"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Running clang-tidy and cppcheck on first-party C++ code"
    VERBATIM
)

# Some universal Qt SDKs cannot execute their arm64 host-tool slice on every
# supported development Mac even though the generated application is valid for
# arm64. The macOS helper scripts can provide Rosetta wrappers without changing
# the target architecture. Override Qt's imported tool targets so this also
# covers qt_add_translation() and qt_add_resources(), which do not consult the
# CMAKE_AUTO* executable variables.
set(OPENSCP_QT_HOST_TOOLS_DIR "" CACHE PATH
    "Optional directory containing runnable moc, uic, rcc, and lrelease wrappers")
if(OPENSCP_QT_HOST_TOOLS_DIR)
    foreach(_qt_host_tool IN ITEMS moc uic rcc lrelease)
        set(_qt_host_tool_path
            "${OPENSCP_QT_HOST_TOOLS_DIR}/${_qt_host_tool}")
        if(NOT EXISTS "${_qt_host_tool_path}")
            message(FATAL_ERROR
                "Missing Qt host-tool wrapper: ${_qt_host_tool_path}")
        endif()
        if(NOT TARGET "Qt6::${_qt_host_tool}")
            message(FATAL_ERROR
                "Qt host-tool target is unavailable: Qt6::${_qt_host_tool}")
        endif()
        foreach(_qt_imported_property IN ITEMS
                IMPORTED_LOCATION
                IMPORTED_LOCATION_DEBUG
                IMPORTED_LOCATION_RELEASE
                IMPORTED_LOCATION_RELWITHDEBINFO
                IMPORTED_LOCATION_MINSIZEREL)
            set_property(TARGET "Qt6::${_qt_host_tool}"
                PROPERTY "${_qt_imported_property}" "${_qt_host_tool_path}")
        endforeach()
    endforeach()
    message(STATUS
        "OpenSCP: using Qt host-tool wrappers from ${OPENSCP_QT_HOST_TOOLS_DIR}")
endif()

# Qt 6.8.x on recent macOS SDKs may expose legacy AGL through WrapOpenGL.
# AGL no longer ships a linkable binary, so strip it to avoid link errors.
if (APPLE AND TARGET WrapOpenGL::WrapOpenGL)
    get_target_property(_wrapogl_libs WrapOpenGL::WrapOpenGL INTERFACE_LINK_LIBRARIES)
    if (_wrapogl_libs)
        set(_wrapogl_filtered "")
        foreach(_lib IN LISTS _wrapogl_libs)
            if (_lib STREQUAL "-framework AGL" OR _lib MATCHES "(^|/)AGL(\\.framework)?$")
                message(STATUS "OpenSCP: removing legacy OpenGL dependency '${_lib}'")
                continue()
            endif()
            list(APPEND _wrapogl_filtered "${_lib}")
        endforeach()
        set_target_properties(WrapOpenGL::WrapOpenGL PROPERTIES
            INTERFACE_LINK_LIBRARIES "${_wrapogl_filtered}")
    endif()
endif()

option(OPENSCP_BUILD_SECURE_ONLY "Disable insecure fallback for secrets storage" ON)

if (OPENSCP_BUILD_TESTS)
    # Tests exercise MockSftpClient and require mock sources in openscp_core.
    set(OPENSCP_ENABLE_MOCK ON CACHE BOOL "Build mock SFTP client" FORCE)
endif()

add_subdirectory(core)

add_library(openscp_sync_logic STATIC
    ui/SyncTypes.hpp
    ui/SyncComparisonEngine.hpp
    ui/SyncComparisonEngine.cpp
)

target_include_directories(openscp_sync_logic
    PUBLIC ${CMAKE_SOURCE_DIR}/ui
)

target_link_libraries(openscp_sync_logic PUBLIC Qt6::Core)

add_library(openscp_ui_logic STATIC
    ui/AppSettings.hpp
    ui/AppSettings.cpp
    ui/ConnectionStatusCoordinator.hpp
    ui/ConnectionStatusCoordinator.cpp
    ui/HostKeyPromptCoordinator.hpp
    ui/HostKeyPromptCoordinator.cpp
    ui/RemotePath.hpp
    ui/RemotePath.cpp
    ui/MainWindowSharedUtils.hpp
    ui/MainWindowSharedUtils.cpp
    ui/NavigationScope.hpp
    ui/NavigationScope.cpp
    ui/NavigationStore.hpp
    ui/NavigationStore.cpp
    ui/PaneController.hpp
    ui/PaneController.cpp
    ui/RemoteActionController.hpp
    ui/RemoteActionController.cpp
    ui/SessionController.hpp
    ui/SessionController.cpp
    ui/SessionHealthMonitor.hpp
    ui/SessionHealthMonitor.cpp
    ui/TerminalCommandBuilder.hpp
    ui/TerminalCommandBuilder.cpp
    ui/TransferUiController.hpp
    ui/TransferUiController.cpp
    ui/RemoteOperationController.hpp
    ui/RemoteOperationController.cpp
    ui/RemoteOperationTypes.hpp
    ui/RemoteTreeWalker.hpp
    ui/RemoteTreeWalker.cpp
    ui/LocalTreeDiscovery.hpp
    ui/LocalTreeDiscovery.cpp
    ui/SyncCoordinator.hpp
    ui/SyncCoordinator.cpp
    ui/SavedSitesPersistence.hpp
    ui/SavedSitesPersistence.cpp
    ui/SiteCredentialRepository.hpp
    ui/SiteCredentialRepository.cpp
    ui/SecretStore.hpp
    ui/SecretStore.cpp
    ui/UiAlerts.hpp
    ui/UiAlerts.cpp
    ui/UiFormatters.hpp
    ui/UiFormatters.cpp
    ui/RemoteModel.hpp
    ui/RemoteModel.cpp
    ui/TransferManager.hpp
    ui/TransferManager.cpp
    ui/TransferTypes.hpp
    ui/BandwidthLimiter.hpp
    ui/BandwidthLimiter.cpp
    ui/TransferQueuePersistence.hpp
    ui/TransferQueuePersistence.cpp
    ui/TransferQueue.hpp
    ui/TransferQueue.cpp
    ui/TransferExecutor.hpp
    ui/TransferExecutor.cpp
)

target_include_directories(openscp_ui_logic
    PUBLIC
        ${CMAKE_SOURCE_DIR}/core/include
        ${CMAKE_SOURCE_DIR}/ui
)

target_link_libraries(openscp_ui_logic
    PUBLIC
        Qt6::Widgets
        openscp_core
        openscp_sync_logic
)

add_library(openscp_ui_widgets STATIC
    ui/SyncDialog.hpp
    ui/SyncDialog.cpp
    ui/DragAwareTreeView.hpp
    ui/DragAwareTreeView.cpp
    ui/ConnectionDialog.hpp
    ui/ConnectionDialog.cpp
    ui/PermissionsDialog.hpp
    ui/PermissionsDialog.cpp
    ui/TransferQueueDialog.hpp
    ui/TransferQueueDialog.cpp
    ui/SiteManagerDialog.hpp
    ui/SiteManagerDialog.cpp
    ui/SiteEntry.hpp
    ui/SettingsDialog.hpp
    ui/SettingsDialog.cpp
    ui/AboutDialog.hpp
    ui/AboutDialog.cpp
)

target_include_directories(openscp_ui_widgets
    PUBLIC ${CMAKE_SOURCE_DIR}/ui
    PRIVATE ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(openscp_ui_widgets
    PUBLIC
        openscp_ui_logic
        Qt6::Widgets
        Qt6::Svg
)

add_executable(openscp
    ui/main.cpp
    ui/MainWindow.hpp
    ui/MainWindow.cpp
    ui/MainWindowConnection.cpp
    ui/MainWindowLocalOps.cpp
    ui/MainWindowRemoteOps.cpp
    ui/MainWindowSync.cpp
    ui/MainWindowTransfers.cpp
)

# Libsecret (Linux): link if available to store secrets securely
find_package(PkgConfig QUIET)
if (PkgConfig_FOUND)
    pkg_check_modules(LIBSECRET QUIET IMPORTED_TARGET libsecret-1)
    if (TARGET PkgConfig::LIBSECRET)
        # libsecret pulls in GLib C headers that legitimately use C-style casts.
        # Keep third-party diagnostics out of the first-party -Werror policy.
        target_include_directories(openscp_ui_logic SYSTEM PRIVATE
            ${LIBSECRET_INCLUDE_DIRS})
        target_link_libraries(openscp_ui_logic PRIVATE
            PkgConfig::LIBSECRET)
        target_compile_definitions(openscp_ui_logic PUBLIC HAVE_LIBSECRET=1)
    endif()
endif()

if (OPENSCP_BUILD_TESTS)
    include(CTest)
    enable_testing()
    add_subdirectory(tests)
endif()

# Generate a header with the application version from CMake project version
configure_file(ui/AppVersion.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/AppVersion.hpp @ONLY)

target_include_directories(openscp PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(openscp PRIVATE
    openscp_ui_widgets
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_custom_target(openscp_hardening_check
        COMMAND "${CMAKE_SOURCE_DIR}/scripts/verify/linux-hardening.sh"
                "$<TARGET_FILE:openscp>"
        DEPENDS openscp
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Verifying ELF hardening for OpenSCP"
        VERBATIM
    )
endif()

if (APPLE)
    target_link_libraries(openscp_ui_logic PRIVATE
        "-framework CoreFoundation"
        "-framework Security"
    )
elseif(WIN32)
    # CryptProtectData/CryptUnprotectData provide per-user encrypted secret
    # storage without placing plaintext credentials in application settings.
    target_link_libraries(openscp_ui_logic PRIVATE Crypt32)
endif()

###############################################################################
# macOS bundle (OpenSCP.app)
# - Produce a proper .app bundle directly from CMake
# - Inject Info.plist from assets/macos/Info.plist.in
# - Set RPATH so dylibs are resolved from @executable_path/../Frameworks
###############################################################################
if(APPLE)
    # Bundle identifier propagated into Info.plist
    set(BUNDLE_ID "io.github.luiscuellar31.openscp" CACHE STRING "macOS bundle identifier")

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
    set_target_properties(openscp PROPERTIES
        MACOSX_BUNDLE TRUE
        OUTPUT_NAME "OpenSCP"                                 # OpenSCP.app and inner binary 'OpenSCP'
        MACOSX_BUNDLE_INFO_PLIST "${_INFO_PLIST_OUT}"

        # Ensure runtime search path resolves to bundled Frameworks directory
        INSTALL_RPATH "@executable_path/../Frameworks"
        BUILD_WITH_INSTALL_RPATH TRUE
    )

    # Bundle icon (.icns) for Finder/Dock.
    set(_APP_ICON_FILE "${CMAKE_SOURCE_DIR}/assets/macos/${ICON_FILE}.icns")
    if (EXISTS "${_APP_ICON_FILE}")
        set_source_files_properties("${_APP_ICON_FILE}" PROPERTIES
            MACOSX_PACKAGE_LOCATION "Resources")
        target_sources(openscp PRIVATE "${_APP_ICON_FILE}")
        set_target_properties(openscp PROPERTIES MACOSX_BUNDLE_ICON_FILE "${ICON_FILE}.icns")
    else()
        message(WARNING "Bundle icon missing: expected ${_APP_ICON_FILE}")
    endif()

    # Keep regular build fast/simple. Packaging/deployment is handled by:
    #   ./scripts/macos.sh app|pkg|dmg|dist
    # which runs scripts/package/macos.sh (macdeployqt + signing/notarization flow).
endif()

if (OPENSCP_BUILD_SECURE_ONLY)
    target_compile_definitions(openscp_ui_logic
        PUBLIC OPENSCP_BUILD_SECURE_ONLY=1)
endif()

# Translations: compile TS catalogs to QM and embed as resources.
# .ts files live in the top-level 'translations/' folder.
qt_add_translation(QM_FILES
    translations/openscp_es.ts
    translations/openscp_fr.ts
    translations/openscp_pt.ts
)
# Set relative aliases for resources to avoid absolute paths in the qrc.
foreach(qm ${QM_FILES})
    get_filename_component(_qm_name "${qm}" NAME)
    set_source_files_properties(${qm} PROPERTIES QT_RESOURCE_ALIAS "${_qm_name}")
endforeach()
qt_add_resources(openscp i18n
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
    assets/icons/action-favorite-active.svg
    assets/icons/action-favorites.svg
    assets/icons/action-go-home.svg
    assets/icons/action-go-up.svg
    assets/icons/action-open-about-us.svg
    assets/icons/action-move-to-left.svg
    assets/icons/action-move-to-right.svg
    assets/icons/action-new-file.svg
    assets/icons/action-new-folder.svg
    assets/icons/action-open-folder.svg
    assets/icons/action-open-folder-remote.svg
    assets/icons/action-open-history.svg
    assets/icons/action-open-saved-sites.svg
    assets/icons/action-open-terminal.svg
    assets/icons/action-open-transfer-queue.svg
    assets/icons/action-open-settings.svg
    assets/icons/action-refresh.svg
    assets/icons/action-search-item.svg
    assets/icons/action-rename.svg
    assets/icons/action-upload.svg
)

# Aliases for stable paths: /assets/icons/<name>.svg
foreach(icon ${ICON_FILES})
    get_filename_component(_name ${icon} NAME)
    set_source_files_properties(${icon} PROPERTIES QT_RESOURCE_ALIAS icons/${_name})
endforeach()

qt_add_resources(openscp app_icons
    PREFIX "/assets"
    FILES ${ICON_FILES}
)

# Program assets (PNG icon for About dialog)
qt_add_resources(openscp app_images
    PREFIX "/assets/program"
    FILES
        assets/program/icon-openscp-2048.png
)

qt_add_resources(openscp credits
    PREFIX "/credits"
    BASE docs/credits
    FILES
        docs/credits/CREDITS.md
        docs/credits/LGPL_COMPLIANCE.md
        docs/credits/LICENSES/OpenSSL-Apache-2.0.txt
        docs/credits/LICENSES/Qt-LGPL-3.0.txt
        docs/credits/LICENSES/libcurl-curl-license.txt
        docs/credits/LICENSES/libssh2-BSD-3-Clause.txt
        docs/credits/LICENSES/tinyxml2-zlib-license.txt
        docs/credits/LICENSES/zlib-license.txt
)

# Ensure predictable in-resource path for the program icon
set_source_files_properties(assets/program/icon-openscp-2048.png
    PROPERTIES QT_RESOURCE_ALIAS "icon-openscp-2048.png")

if(UNIX AND NOT APPLE)
    # Desktop entry and app icon installation for Linux (XDG paths)
    set(APP_DESKTOP_FILE assets/linux/openscp.desktop)
    set(APP_ICON_PNG     assets/program/icon-openscp-256.png)
    set(APP_ICON_NAME    openscp) # must match Icon= in the .desktop

    # Install binary
    install(TARGETS openscp RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

    # Install .desktop file
    install(FILES ${APP_DESKTOP_FILE}
                    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)

    # Install app icon where desktops expect it
    # Prefer hicolor 256x256; fall back to pixmaps for broad compatibility
    install(FILES ${APP_ICON_PNG}
                    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/256x256/apps
                    RENAME ${APP_ICON_NAME}.png)

    install(FILES ${APP_ICON_PNG}
                    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/pixmaps
                    RENAME ${APP_ICON_NAME}.png)
endif()
