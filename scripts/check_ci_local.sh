#!/usr/bin/env bash

set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${REPO_DIR}/build-ci-local}"
RUN_FULL=0
CLEAN=0
JOBS="${JOBS:-}"
QT_WRAP_DIR=""

log() { printf "\033[1;34m[ci-check]\033[0m %s\n" "$*"; }
warn() { printf "\033[1;33m[warn]\033[0m %s\n" "$*"; }
die() { printf "\033[1;31m[err ]\033[0m %s\n" "$*"; exit 1; }

usage() {
  cat <<'EOF'
Usage: ./scripts/check_ci_local.sh [options]

Options:
  --full            Build GUI app target too (openscp_hello)
  --clean           Remove build directory before configuring
  --build-dir <p>   Custom build directory (default: build-ci-local)
  -j, --jobs <n>    Parallel build jobs
  -h, --help        Show help

Env vars:
  BUILD_DIR         Same as --build-dir
  JOBS              Same as --jobs

Examples:
  ./scripts/check_ci_local.sh
  ./scripts/check_ci_local.sh --clean --full
  ./scripts/check_ci_local.sh --build-dir build-ci-local -j 8
EOF
}

parse_args() {
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --full)
        RUN_FULL=1
        shift
        ;;
      --clean)
        CLEAN=1
        shift
        ;;
      --build-dir)
        [[ $# -ge 2 ]] || die "--build-dir requires a value"
        BUILD_DIR="$2"
        shift 2
        ;;
      -j|--jobs)
        [[ $# -ge 2 ]] || die "$1 requires a value"
        JOBS="$2"
        shift 2
        ;;
      -h|--help)
        usage
        exit 0
        ;;
      *)
        die "Unknown option: $1"
        ;;
    esac
  done
}

normalize_paths() {
  case "$BUILD_DIR" in
    /*) ;;
    *) BUILD_DIR="${REPO_DIR}/${BUILD_DIR}" ;;
  esac
}

latest_qt6_dir() {
  local best=""
  local cand=""
  shopt -s nullglob
  for cand in "${HOME}"/Qt/*/macos/lib/cmake/Qt6; do
    [[ -f "${cand}/Qt6Config.cmake" ]] || continue
    best="$cand"
  done
  shopt -u nullglob
  [[ -n "$best" ]] && printf "%s\n" "$best"
}

create_qt_x86_wrapper() {
  local target="$1"
  local real_bin="$2"
  cat > "${target}" <<EOF
#!/usr/bin/env bash
exec arch -x86_64 "${real_bin}" "\$@"
EOF
  chmod +x "${target}"
}

setup_macos_qt_wrappers_if_needed() {
  local qt6_dir="$1"
  [[ "$(uname -s)" == "Darwin" ]] || return 0
  [[ "$(uname -m)" == "arm64" ]] || return 0
  [[ -n "$qt6_dir" ]] || return 0

  local qt_prefix
  qt_prefix="$(cd "$qt6_dir/../../.." && pwd)"
  local uic="${qt_prefix}/libexec/uic"
  local rcc="${qt_prefix}/libexec/rcc"
  local moc="${qt_prefix}/libexec/moc"
  local lrelease="${qt_prefix}/libexec/lrelease"
  [[ -x "$uic" && -x "$rcc" && -x "$moc" ]] || return 0

  if arch -x86_64 "$uic" -h >/dev/null 2>&1; then
    QT_WRAP_DIR="${BUILD_DIR}/qt-tools-wrap"
    mkdir -p "$QT_WRAP_DIR"
    create_qt_x86_wrapper "${QT_WRAP_DIR}/uic" "$uic"
    create_qt_x86_wrapper "${QT_WRAP_DIR}/rcc" "$rcc"
    create_qt_x86_wrapper "${QT_WRAP_DIR}/moc" "$moc"
    if [[ -x "$lrelease" ]]; then
      create_qt_x86_wrapper "${QT_WRAP_DIR}/lrelease" "$lrelease"
    fi
    warn "Using x86_64 wrappers for Qt host tools (uic/rcc/moc/lrelease)."
    return 0
  fi

  if "$uic" -h >/dev/null 2>&1; then
    return 0
  fi
  warn "Qt host tools are not runnable (native/x86_64). Check your Qt installation."
}

configure_project() {
  local -a cmake_args
  cmake_args=(
    -S "$REPO_DIR"
    -B "$BUILD_DIR"
    -DOPENSCP_BUILD_TESTS=ON
  )

  if [[ "$(uname -s)" == "Darwin" ]]; then
    local effective_qt6_dir="${Qt6_DIR:-}"
    if [[ -z "${Qt6_DIR:-}" ]]; then
      local auto_qt
      auto_qt="$(latest_qt6_dir || true)"
      if [[ -n "$auto_qt" ]]; then
        effective_qt6_dir="$auto_qt"
        cmake_args+=("-DQt6_DIR=${auto_qt}")
        cmake_args+=("-DCMAKE_PREFIX_PATH=$(cd "$auto_qt/../../.." && pwd)")
      else
        warn "Qt6_DIR not set and no ~/Qt/*/macos/lib/cmake/Qt6 detected."
      fi
    fi

    setup_macos_qt_wrappers_if_needed "$effective_qt6_dir"
    if [[ -n "$QT_WRAP_DIR" ]]; then
      cmake_args+=("-DCMAKE_AUTOUIC_EXECUTABLE=${QT_WRAP_DIR}/uic")
      cmake_args+=("-DCMAKE_AUTORCC_EXECUTABLE=${QT_WRAP_DIR}/rcc")
      cmake_args+=("-DCMAKE_AUTOMOC_EXECUTABLE=${QT_WRAP_DIR}/moc")
      if [[ -x "${QT_WRAP_DIR}/lrelease" ]]; then
        cmake_args+=("-DQt6_LRELEASE_EXECUTABLE=${QT_WRAP_DIR}/lrelease")
        cmake_args+=("-DQT_LRELEASE_EXECUTABLE=${QT_WRAP_DIR}/lrelease")
      fi
    fi
  fi

  log "Configuring: ${BUILD_DIR}"
  cmake "${cmake_args[@]}"
}

build_targets() {
  local -a build_args
  build_args=(--build "$BUILD_DIR")
  if [[ -n "$JOBS" ]]; then
    build_args+=(--parallel "$JOBS")
  else
    build_args+=(--parallel)
  fi

  log "Building core + test targets"
  cmake "${build_args[@]}" --target \
    openscp_core \
    openscp_core_tests \
    openscp_sftp_integration_tests \
    openscp_scp_integration_tests \
    openscp_ftp_integration_tests \
    openscp_ftps_integration_tests

  if [[ "$RUN_FULL" -eq 1 ]]; then
    log "Building GUI app target (openscp_hello)"
    cmake "${build_args[@]}" --target openscp_hello
  fi
}

run_tests() {
  log "Running tests"
  ctest --test-dir "$BUILD_DIR" --output-on-failure
}

main() {
  parse_args "$@"
  normalize_paths

  if [[ "$CLEAN" -eq 1 ]]; then
    log "Cleaning build directory: ${BUILD_DIR}"
    rm -rf "$BUILD_DIR"
  fi

  configure_project
  build_targets
  run_tests

  log "Done. Local CI check passed."
}

main "$@"
