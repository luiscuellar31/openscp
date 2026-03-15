#!/usr/bin/env bash

set -euo pipefail

# OpenSCP Linux AppImage packaging script
# - Builds Release via CMake
# - Assembles an AppDir with desktop file and icon
# - Uses linuxdeploy (+ linuxdeploy-plugin-qt) to bundle Qt and system deps
# - Generates an AppImage with appimagetool
#
# Requirements (on your Linux machine):
#   - cmake, g++, make
#   - Qt 6 dev libs/tools (Qt Widgets)
#   - libssh2-dev, openssl (libssl) dev
#   - linuxdeploy and linuxdeploy-plugin-qt available in PATH
#   - appimagetool available in PATH
#
# Optional env vars:
#   APP_NAME            Default: "OpenSCP"
#   EXEC_NAME           Default: "openscp_hello" (the built binary name)
#   CMAKE_PREFIX_PATH   Point to your Qt root if not in system paths
#   Qt6_DIR             Qt6 CMake config dir (…/lib/cmake/Qt6), optional
#   LINUXDEPLOY         Path or command name (default: linuxdeploy)
#   LINUXDEPLOY_QT      Path or command name (default: linuxdeploy-plugin-qt)
#   APPIMAGETOOL        Path or command name (default: appimagetool)
#   SKIP_QT_PLUGIN      Set to 1 to skip the qt plugin (not recommended)

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${REPO_DIR}/build"
DIST_DIR="${REPO_DIR}/dist"

APP_NAME="${APP_NAME:-OpenSCP}"
EXEC_NAME="${EXEC_NAME:-openscp_hello}"
LINUXDEPLOY="${LINUXDEPLOY:-linuxdeploy}"
LINUXDEPLOY_QT="${LINUXDEPLOY_QT:-linuxdeploy-plugin-qt}"
APPIMAGETOOL="${APPIMAGETOOL:-appimagetool}"

APPDIR="${DIST_DIR}/${APP_NAME}.AppDir"

log() { printf "\033[1;34m[pack]\033[0m %s\n" "$*"; }
warn() { printf "\033[1;33m[warn]\033[0m %s\n" "$*"; }
err() { printf "\033[1;31m[err ]\033[0m %s\n" "$*"; }
die() { err "$*"; exit 1; }

ensure_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "Missing required tool: $1"
}

detect_qt_plugins_root() {
  local candidate=""

  if command -v qtpaths6 >/dev/null 2>&1; then
    candidate="$(qtpaths6 --query QT_INSTALL_PLUGINS 2>/dev/null || true)"
    [[ -n "$candidate" && -d "$candidate" ]] && { echo "$candidate"; return; }
  fi
  if command -v qtpaths >/dev/null 2>&1; then
    candidate="$(qtpaths --query QT_INSTALL_PLUGINS 2>/dev/null || true)"
    [[ -n "$candidate" && -d "$candidate" ]] && { echo "$candidate"; return; }
  fi
  if command -v qmake6 >/dev/null 2>&1; then
    candidate="$(qmake6 -query QT_INSTALL_PLUGINS 2>/dev/null || true)"
    [[ -n "$candidate" && -d "$candidate" ]] && { echo "$candidate"; return; }
  fi
  if command -v qmake >/dev/null 2>&1; then
    candidate="$(qmake -query QT_INSTALL_PLUGINS 2>/dev/null || true)"
    [[ -n "$candidate" && -d "$candidate" ]] && { echo "$candidate"; return; }
  fi

  for candidate in \
    "/usr/lib/qt6/plugins" \
    "/usr/lib/$(uname -m)-linux-gnu/qt6/plugins" \
    "/usr/lib64/qt6/plugins"
  do
    [[ -d "$candidate" ]] && { echo "$candidate"; return; }
  done

  return 1
}

detect_version() {
  local ver
  ver=$(sed -n 's/^project(.*VERSION[[:space:]]*\([0-9][0-9.]*\).*/\1/p' "${REPO_DIR}/CMakeLists.txt" | head -n1 || true)
  if [[ -z "$ver" ]]; then ver="0.0.0"; fi
  echo "$ver"
}

appimage_arch() {
  local m; m=$(uname -m)
  case "$m" in
    x86_64) echo x86_64 ;;
    aarch64|arm64) echo aarch64 ;;
    *) echo "$m" ;;
  esac
}

prepare_desktop() {
  local dst="$1"
  mkdir -p "$(dirname "$dst")"
  if [[ -f "${REPO_DIR}/assets/linux/openscp.desktop" ]]; then
    cp "${REPO_DIR}/assets/linux/openscp.desktop" "$dst"
    return
  fi
  cat > "$dst" << 'EOF'
[Desktop Entry]
Type=Application
Name=OpenSCP
GenericName=SFTP Client
Comment=Two-panel SFTP client focused on simplicity and security
Exec=openscp_hello
Icon=openscp
Terminal=false
Categories=Network;FileTransfer;Utility;
Keywords=SFTP;SSH;File;Transfer;Client;
StartupWMClass=OpenSCP
EOF
}

prepare_icon() {
  local src_png_256="${REPO_DIR}/assets/program/icon-openscp-256.png"
  local src_png_large="${REPO_DIR}/assets/program/icon-openscp-2048.png"
  local dst_png="$1"
  mkdir -p "$(dirname "$dst_png")"
  if [[ -f "$src_png_256" ]]; then
    cp "$src_png_256" "$dst_png"
    return
  fi
  if [[ -f "$src_png_large" ]] && command -v convert >/dev/null 2>&1; then
    convert "$src_png_large" -resize 256x256 "$dst_png"
    return
  fi
  if [[ -f "$src_png_large" ]]; then
    die "Missing 256x256 icon asset and ImageMagick 'convert' is unavailable to resize ${src_png_large}"
  fi
  die "No source icon found for AppImage packaging"
}

copy_licenses() {
  if [[ -d "${REPO_DIR}/docs/credits/LICENSES" ]]; then
    local destdir="$APPDIR/usr/share/doc/openscp/licenses"
    mkdir -p "$destdir"
    cp -R "${REPO_DIR}/docs/credits/LICENSES" "$destdir/"
    [[ -f "${REPO_DIR}/docs/credits/CREDITS.md" ]] && cp "${REPO_DIR}/docs/credits/CREDITS.md" "$destdir/"
  fi
}

# Copy full docs directory to the AppDir root so the About dialog can find
# docs/ABOUT_LIBRARIES_*.txt by walking up from applicationDirPath.
copy_docs() {
  if [[ -d "${REPO_DIR}/docs" ]]; then
    mkdir -p "$APPDIR/docs"
    cp -R "${REPO_DIR}/docs/." "$APPDIR/docs/"
  fi
}

# Copy the original about PNG under AppDir/assets/program so AboutDialog's
# filesystem fallback can find it if resource loading fails.
copy_about_png_fallback() {
  if [[ -f "${REPO_DIR}/assets/program/icon-openscp-2048.png" ]]; then
    mkdir -p "$APPDIR/assets/program"
    cp "${REPO_DIR}/assets/program/icon-openscp-2048.png" "$APPDIR/assets/program/"
  fi
}

# Validate that Qt runtime plugins and dependencies are present in the packaged tree.
verify_qt_runtime_plugins() {
  local checker="${REPO_DIR}/scripts/verify_qt_svg_plugins.sh"
  [[ -x "$checker" ]] || die "Qt runtime checker not found or not executable: $checker"
  "$checker" --context appimage "$APPDIR"
}

ensure_qt_svg_iconengine() {
  local dest_dir="$APPDIR/usr/plugins/iconengines"
  if compgen -G "$dest_dir/libqsvgicon.so*" >/dev/null 2>&1 || compgen -G "$dest_dir/qsvgicon.so*" >/dev/null 2>&1; then
    return
  fi

  local plugin_root=""
  plugin_root="$(detect_qt_plugins_root || true)"
  [[ -n "$plugin_root" ]] || die "Could not locate the Qt plugins directory needed to bundle qsvgicon"

  local src_dir="${plugin_root}/iconengines"
  [[ -d "$src_dir" ]] || die "Qt iconengines directory not found under: $plugin_root"

  local src_plugin=""
  src_plugin="$(find "$src_dir" -maxdepth 1 \( -type f -o -type l \) \( -name 'libqsvgicon.so*' -o -name 'qsvgicon.so*' \) | head -n1 || true)"
  [[ -n "$src_plugin" ]] || die "Required Qt icon engine qsvgicon not found in: $src_dir"

  log "Copying required Qt icon engine: $(basename "$src_plugin")"
  mkdir -p "$dest_dir"
  cp -L "$src_plugin" "$dest_dir/"
}

copy_optional_qt_plugin() {
  local plugin_root="$1"
  local subdir="$2"
  local plugin_name="$3"
  local dest_dir="$APPDIR/usr/plugins/$subdir"
  local src_dir="${plugin_root}/${subdir}"

  [[ -d "$src_dir" ]] || return 0

  if compgen -G "$dest_dir/lib${plugin_name}.so*" >/dev/null 2>&1 || compgen -G "$dest_dir/${plugin_name}.so*" >/dev/null 2>&1; then
    return 0
  fi

  local src_plugin=""
  src_plugin="$(find "$src_dir" -maxdepth 1 \( -type f -o -type l \) \( -name "lib${plugin_name}.so*" -o -name "${plugin_name}.so*" \) | head -n1 || true)"
  [[ -n "$src_plugin" ]] || return 0

  mkdir -p "$dest_dir"
  cp -L "$src_plugin" "$dest_dir/"
  log "Copying optional Qt ${subdir} plugin: $(basename "$src_plugin")"
}

ensure_optional_qt_desktop_theme_plugins() {
  local plugin_root=""
  plugin_root="$(detect_qt_plugins_root || true)"
  if [[ -z "$plugin_root" ]]; then
    warn "Could not detect Qt plugins root for optional desktop theme plugins"
    return 0
  fi

  # Keep Linux look closer to host desktop theme when these plugins are available.
  copy_optional_qt_plugin "$plugin_root" "styles" "qgtk3"
  copy_optional_qt_plugin "$plugin_root" "platformthemes" "qgtk3"
  copy_optional_qt_plugin "$plugin_root" "platformthemes" "qxdgdesktopportal"
}

main() {
  mkdir -p "$BUILD_DIR" "$DIST_DIR"

  # Build Release
  log "Configuring and building (Release)"
  if [[ -n "${Qt6_DIR:-}" ]]; then
    cmake -S "$REPO_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DQt6_DIR="$Qt6_DIR" ${CMAKE_PREFIX_PATH:+-DCMAKE_PREFIX_PATH="$CMAKE_PREFIX_PATH"}
  else
    cmake -S "$REPO_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release ${CMAKE_PREFIX_PATH:+-DCMAKE_PREFIX_PATH="$CMAKE_PREFIX_PATH"}
  fi
  cmake --build "$BUILD_DIR" -j

  local exe
  exe="$BUILD_DIR/$EXEC_NAME"
  [[ -x "$exe" ]] || die "Built executable not found at: $exe"

  # Prepare AppDir skeleton
  rm -rf "$APPDIR"
  mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/share/applications" "$APPDIR/usr/share/icons/hicolor/256x256/apps"

  # Desktop and icon
  prepare_desktop "$APPDIR/usr/share/applications/openscp.desktop"
  prepare_icon "$APPDIR/usr/share/icons/hicolor/256x256/apps/openscp.png"
  copy_licenses
  copy_docs
  copy_about_png_fallback

  # Ensure tools
  ensure_cmd "$LINUXDEPLOY"
  ensure_cmd "$APPIMAGETOOL"
  if [[ "${SKIP_QT_PLUGIN:-0}" != "1" ]]; then
    ensure_cmd "$LINUXDEPLOY_QT"
  fi

  # VERSION env var is used by linuxdeploy/appimagetool for naming
  local version arch out_name
  version="${APP_VERSION:-$(detect_version)}"
  arch="$(appimage_arch)"
  out_name="${APP_NAME}-${version}-${arch}.AppImage"

  # Create AppImage (run from dist to keep outputs localized)
  pushd "$DIST_DIR" >/dev/null
  # Clean old file with same name
  rm -f "$out_name"

  export VERSION="$version"
  local cmd=("$LINUXDEPLOY" --appdir "$APPDIR" -e "$exe" -d "$APPDIR/usr/share/applications/openscp.desktop" -i "$APPDIR/usr/share/icons/hicolor/256x256/apps/openscp.png")
  if [[ "${SKIP_QT_PLUGIN:-0}" != "1" ]]; then
    cmd+=( --plugin qt )
  fi
  cmd+=( --output appimage )
  log "Running: ${cmd[*]}"
  "${cmd[@]}"
  ensure_qt_svg_iconengine
  ensure_optional_qt_desktop_theme_plugins
  verify_qt_runtime_plugins

  # Rename the produced AppImage to our canonical name if needed
  local produced
  produced=$(ls -1t *.AppImage 2>/dev/null | head -n1 || true)
  if [[ -n "$produced" && "$produced" != "$out_name" ]]; then
    mv -f "$produced" "$out_name"
  fi

  # Generate SHA256
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$out_name" | awk '{print $1}' > "${out_name}.sha256"
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$out_name" | awk '{print $1}' > "${out_name}.sha256"
  fi

  popd >/dev/null

  log "Done: ${DIST_DIR}/${out_name}"
}

main "$@"
