#!/usr/bin/env bash

set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${REPO_DIR}/build"
APP_PATH="${BUILD_DIR}/OpenSCP.app"
EFFECTIVE_QT6_DIR=""
EFFECTIVE_QT_PREFIX=""

log() { printf "\033[1;34m[macos]\033[0m %s\n" "$*"; }
die() { printf "\033[1;31m[err ]\033[0m %s\n" "$*"; exit 1; }

version_key() {
  local v="${1:-0}"
  local a=0 b=0 c=0 d=0
  IFS='.' read -r a b c d <<<"$v"
  printf "%04d%04d%04d%04d" "${a:-0}" "${b:-0}" "${c:-0}" "${d:-0}"
}

detect_qt6_dir_from_home() {
  local best_dir=""
  local best_key=""
  local cand ver key
  shopt -s nullglob
  for cand in "${HOME}"/Qt/*/macos/lib/cmake/Qt6; do
    [[ -f "${cand}/Qt6Config.cmake" ]] || continue
    ver="$(sed -E 's#^.*/Qt/([^/]+)/macos/lib/cmake/Qt6$#\1#' <<<"$cand")"
    [[ "$ver" =~ ^[0-9]+(\.[0-9]+)*$ ]] || continue
    key="$(version_key "$ver")"
    if [[ -z "$best_key" || "$key" > "$best_key" ]]; then
      best_key="$key"
      best_dir="$cand"
    fi
  done
  shopt -u nullglob
  [[ -n "$best_dir" ]] && printf "%s\n" "$best_dir"
}

resolve_qt_paths() {
  local qt6_dir=""

  if [[ -n "${Qt6_DIR:-}" ]]; then
    qt6_dir="${Qt6_DIR}"
  elif [[ -n "${QT6_DIR:-}" ]]; then
    qt6_dir="${QT6_DIR}"
  elif [[ -n "${QT_PREFIX:-}" ]]; then
    qt6_dir="${QT_PREFIX}/lib/cmake/Qt6"
  else
    qt6_dir="$(detect_qt6_dir_from_home || true)"
  fi

  if [[ -n "${QT_PREFIX:-}" ]]; then
    EFFECTIVE_QT_PREFIX="${QT_PREFIX}"
  elif [[ -n "$qt6_dir" && -d "$qt6_dir" ]]; then
    EFFECTIVE_QT_PREFIX="$(cd "$qt6_dir/../.." && pwd)"
  else
    EFFECTIVE_QT_PREFIX=""
  fi

  if [[ -n "$qt6_dir" ]]; then
    EFFECTIVE_QT6_DIR="$qt6_dir"
  else
    EFFECTIVE_QT6_DIR=""
  fi
}

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
  QT6_DIR=/path/to/Qt/<ver>/macos/lib/cmake/Qt6
  CMAKE_OSX_ARCHITECTURES=arm64|x86_64|arm64;x86_64
  SKIP_CODESIGN=1|0
  SKIP_NOTARIZATION=1|0

If no Qt path is provided, the script auto-detects the newest Qt under:
  $HOME/Qt/<version>/macos
EOF
}

resolve_qt_paths

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
