#!/usr/bin/env bash

set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${REPO_DIR}/build"
APP_PATH="${BUILD_DIR}/OpenSCP.app"
EFFECTIVE_QT6_DIR=""
EFFECTIVE_QT_PREFIX=""
QT_HOST_WRAP_DIR=""

log() { printf "\033[1;34m[macos]\033[0m %s\n" "$*"; }
warn() { printf "\033[1;33m[warn]\033[0m %s\n" "$*"; }
die() { printf "\033[1;31m[err ]\033[0m %s\n" "$*"; exit 1; }

list_rpaths() {
  local bin="$1"
  otool -l "$bin" | awk '
    $1 == "cmd" && $2 == "LC_RPATH" { in_rpath=1; next }
    in_rpath && $1 == "path" { print $2; in_rpath=0 }
  '
}

sanitize_dev_rpaths() {
  local bin="$1"
  local bundled_rpath='@executable_path/../Frameworks'
  local qt_lib_rpath=""
  if [[ -d "${EFFECTIVE_QT_PREFIX}/lib" ]]; then
    qt_lib_rpath="${EFFECTIVE_QT_PREFIX}/lib"
  fi

  local rp=""
  while IFS= read -r rp; do
    [[ -n "$rp" ]] || continue
    case "$rp" in
      "$bundled_rpath"|@executable_path/*|@loader_path/*|@rpath/*)
        ;;
      "$qt_lib_rpath")
        ;;
      /*)
        # Remove stale absolute build paths to keep dev launches relocatable.
        install_name_tool -delete_rpath "$rp" "$bin" >/dev/null 2>&1 || true
        ;;
      *)
        ;;
    esac
  done < <(list_rpaths "$bin")

  if ! list_rpaths "$bin" | grep -Fxq "$bundled_rpath"; then
    install_name_tool -add_rpath "$bundled_rpath" "$bin" >/dev/null 2>&1 || true
  fi
  if [[ -n "$qt_lib_rpath" ]] && ! list_rpaths "$bin" | grep -Fxq "$qt_lib_rpath"; then
    install_name_tool -add_rpath "$qt_lib_rpath" "$bin" >/dev/null 2>&1 || true
  fi
}

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
    EFFECTIVE_QT_PREFIX="$(cd "$qt6_dir/../../.." && pwd)"
  else
    EFFECTIVE_QT_PREFIX=""
  fi

  if [[ -n "$qt6_dir" ]]; then
    EFFECTIVE_QT6_DIR="$qt6_dir"
  else
    EFFECTIVE_QT6_DIR=""
  fi
}

create_qt_x86_wrapper() {
  local target="$1"
  local real_bin="$2"
  printf '#!/usr/bin/env bash\nexec /usr/bin/arch -x86_64 %q "$@"\n' \
    "$real_bin" > "$target"
  chmod +x "$target"
}

qt_tool_runs() {
  { ( "$@" ) >/dev/null 2>&1; } 2>/dev/null
}

setup_qt_host_wrappers_if_needed() {
  [[ "$(uname -s)" == "Darwin" ]] || return 0
  [[ "$(uname -m)" == "arm64" ]] || return 0
  [[ -n "$EFFECTIVE_QT_PREFIX" ]] || return 0

  local uic="${EFFECTIVE_QT_PREFIX}/libexec/uic"
  local rcc="${EFFECTIVE_QT_PREFIX}/libexec/rcc"
  local moc="${EFFECTIVE_QT_PREFIX}/libexec/moc"
  local lrelease="${EFFECTIVE_QT_PREFIX}/libexec/lrelease"
  if [[ ! -x "$lrelease" ]]; then
    lrelease="${EFFECTIVE_QT_PREFIX}/bin/lrelease"
  fi
  [[ -x "$uic" && -x "$rcc" && -x "$moc" && -x "$lrelease" ]] || return 0

  if qt_tool_runs "$uic" -h &&
     qt_tool_runs "$rcc" -h &&
     qt_tool_runs "$moc" -h &&
     qt_tool_runs "$lrelease" -version; then
    return 0
  fi
  if ! qt_tool_runs /usr/bin/arch -x86_64 "$uic" -h ||
     ! qt_tool_runs /usr/bin/arch -x86_64 "$rcc" -h ||
     ! qt_tool_runs /usr/bin/arch -x86_64 "$moc" -h ||
     ! qt_tool_runs /usr/bin/arch -x86_64 "$lrelease" -version; then
    die "Qt host tools are not runnable natively or through Rosetta under ${EFFECTIVE_QT_PREFIX}"
  fi

  QT_HOST_WRAP_DIR="${BUILD_DIR}/qt-tools-wrap"
  mkdir -p "$QT_HOST_WRAP_DIR"
  create_qt_x86_wrapper "${QT_HOST_WRAP_DIR}/uic" "$uic"
  create_qt_x86_wrapper "${QT_HOST_WRAP_DIR}/rcc" "$rcc"
  create_qt_x86_wrapper "${QT_HOST_WRAP_DIR}/moc" "$moc"
  create_qt_x86_wrapper "${QT_HOST_WRAP_DIR}/lrelease" "$lrelease"
  warn "Qt host tools are not runnable natively; using their x86_64 slices through Rosetta."
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
  setup_qt_host_wrappers_if_needed
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
  if [[ -n "$QT_HOST_WRAP_DIR" ]]; then
    args+=("-DCMAKE_AUTOUIC_EXECUTABLE=${QT_HOST_WRAP_DIR}/uic")
    args+=("-DCMAKE_AUTORCC_EXECUTABLE=${QT_HOST_WRAP_DIR}/rcc")
    args+=("-DCMAKE_AUTOMOC_EXECUTABLE=${QT_HOST_WRAP_DIR}/moc")
    args+=("-DOPENSCP_QT_HOST_TOOLS_DIR=${QT_HOST_WRAP_DIR}")
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
  local bin="${APP_PATH}/Contents/MacOS/OpenSCP"
  [[ -x "$bin" ]] || die "Cannot launch app: missing executable at ${bin}"
  sanitize_dev_rpaths "$bin"

  local bundled_qt_widgets="${APP_PATH}/Contents/Frameworks/QtWidgets.framework/Versions/A/QtWidgets"
  if [[ -f "$bundled_qt_widgets" ]]; then
    log "Opening ${APP_PATH}"
    if open "$APP_PATH"; then
      return
    fi
    warn "LaunchServices open() failed; falling back to direct binary launch"
  else
    warn "Dev bundle has no bundled Qt frameworks; launching with Qt runtime env"
  fi

  if [[ -d "${EFFECTIVE_QT_PREFIX}/lib" ]]; then
    QT_PLUGIN_PATH="${EFFECTIVE_QT_PREFIX}/plugins" \
    QML2_IMPORT_PATH="${EFFECTIVE_QT_PREFIX}/qml" \
    DYLD_FRAMEWORK_PATH="${EFFECTIVE_QT_PREFIX}/lib" \
    DYLD_LIBRARY_PATH="${EFFECTIVE_QT_PREFIX}/lib" \
    nohup "$bin" >/dev/null 2>&1 &
  else
    warn "Qt runtime prefix was not detected; launching without Qt env overrides"
    nohup "$bin" >/dev/null 2>&1 &
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
  CMAKE_OSX_ARCHITECTURES="${CMAKE_OSX_ARCHITECTURES:-$(uname -m)}" \
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
