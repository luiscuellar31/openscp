#!/usr/bin/env bash

set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${REPO_DIR}/build"
APP_PATH="${BUILD_DIR}/OpenSCP.app"
DEFAULT_QT_PREFIX="/Users/luiscuellar/Qt/6.8.3/macos"

if [[ -n "${Qt6_DIR:-}" ]]; then
  EFFECTIVE_QT6_DIR="${Qt6_DIR}"
elif [[ -n "${QT6_DIR:-}" ]]; then
  EFFECTIVE_QT6_DIR="${QT6_DIR}"
else
  EFFECTIVE_QT6_DIR="${DEFAULT_QT_PREFIX}/lib/cmake/Qt6"
fi

if [[ -n "${QT_PREFIX:-}" ]]; then
  EFFECTIVE_QT_PREFIX="${QT_PREFIX}"
else
  EFFECTIVE_QT_PREFIX="${DEFAULT_QT_PREFIX}"
fi

log() { printf "\033[1;34m[macos]\033[0m %s\n" "$*"; }
die() { printf "\033[1;31m[err ]\033[0m %s\n" "$*"; exit 1; }

usage() {
  cat <<'EOF'
Usage: ./scripts/macos.sh <command>

Commands:
  configure   Configure Release build directory (build/)
  build       Build Release target
  run         Open build/OpenSCP.app
  dev         Configure + build + run
  app         Build/package as local unsigned app ZIP (dist/*.zip)
  pkg         Build/package as local unsigned PKG (dist/*.pkg)
  dmg         Build/package as local unsigned DMG (dist/*.dmg)
  dist        Build/package app+pkg+dmg
  help        Show this help

Optional env vars:
  QT_PREFIX=/path/to/Qt/<ver>/macos
  Qt6_DIR=/path/to/Qt/<ver>/macos/lib/cmake/Qt6
  CMAKE_OSX_ARCHITECTURES=arm64|x86_64|arm64;x86_64
  SKIP_CODESIGN=1|0
  SKIP_NOTARIZATION=1|0
EOF
}

configure_release() {
  local args=(
    -S "$REPO_DIR"
    -B "$BUILD_DIR"
    -DCMAKE_BUILD_TYPE=Release
  )
  if [[ -d "$EFFECTIVE_QT_PREFIX" ]]; then
    args+=("-DCMAKE_PREFIX_PATH=${EFFECTIVE_QT_PREFIX}")
  fi
  if [[ -d "$EFFECTIVE_QT6_DIR" ]]; then
    args+=("-DQt6_DIR=${EFFECTIVE_QT6_DIR}")
  fi
  if [[ -n "${CMAKE_OSX_ARCHITECTURES:-}" ]]; then
    args+=("-DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}")
  fi
  log "Configuring Release build"
  cmake "${args[@]}"
}

build_release() {
  log "Building"
  cmake --build "$BUILD_DIR" -j
}

run_app() {
  [[ -d "$APP_PATH" ]] || die "App bundle not found at ${APP_PATH}. Run './scripts/macos.sh build' first."
  log "Opening ${APP_PATH}"
  if ! open "$APP_PATH"; then
    local bin="${APP_PATH}/Contents/MacOS/OpenSCP"
    [[ -x "$bin" ]] || die "Cannot launch app: missing executable at ${bin}"
    log "LaunchServices open() failed; running binary directly with Qt env"
    if [[ -d "${EFFECTIVE_QT_PREFIX}/lib" ]]; then
      QT_PLUGIN_PATH="${EFFECTIVE_QT_PREFIX}/plugins" \
      QML2_IMPORT_PATH="${EFFECTIVE_QT_PREFIX}/qml" \
      DYLD_FRAMEWORK_PATH="${EFFECTIVE_QT_PREFIX}/lib" \
      DYLD_LIBRARY_PATH="${EFFECTIVE_QT_PREFIX}/lib" \
      nohup "$bin" >/dev/null 2>&1 &
    else
      nohup "$bin" >/dev/null 2>&1 &
    fi
  fi
}

package_format() {
  local formats="$1"
  [[ -x "${REPO_DIR}/scripts/package_mac.sh" ]] || die "Missing scripts/package_mac.sh"
  log "Packaging formats: ${formats}"
  SKIP_CODESIGN="${SKIP_CODESIGN:-1}" \
  SKIP_NOTARIZATION="${SKIP_NOTARIZATION:-1}" \
  PACKAGE_FORMATS="${formats}" \
  Qt6_DIR="${EFFECTIVE_QT6_DIR}" \
  CMAKE_OSX_ARCHITECTURES="${CMAKE_OSX_ARCHITECTURES:-arm64}" \
  "${REPO_DIR}/scripts/package_mac.sh"
}

cmd="${1:-help}"
case "$cmd" in
  configure)
    configure_release
    ;;
  build)
    build_release
    ;;
  run)
    run_app
    ;;
  dev)
    configure_release
    build_release
    run_app
    ;;
  app)
    package_format "app"
    ;;
  pkg)
    package_format "pkg"
    ;;
  dmg)
    package_format "dmg"
    ;;
  dist)
    package_format "app,pkg,dmg"
    ;;
  help|-h|--help)
    usage
    ;;
  *)
    usage
    die "Unknown command: ${cmd}"
    ;;
esac
