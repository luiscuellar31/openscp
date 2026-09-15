#!/usr/bin/env bash

set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${REPO_DIR}/build-ci-local}"
RUN_FULL=0
CLEAN=0
WERROR="${OPENSCP_WERROR:-OFF}"
JOBS="${JOBS:-}"
QT_HOST_WRAP_DIR=""

log() { printf "\033[1;34m[ci-check]\033[0m %s\n" "$*"; }
warn() { printf "\033[1;33m[warn]\033[0m %s\n" "$*"; }
die() { printf "\033[1;31m[err ]\033[0m %s\n" "$*"; exit 1; }

source "${REPO_DIR}/scripts/lib/macos/qt-host-tools.sh"

usage() {
  cat <<'EOF'
Usage: ./scripts/check_ci_local.sh [options]

Options:
  --full            Build GUI app target too (openscp)
  --clean           Remove build directory before configuring
  --werror          Treat first-party compiler warnings as errors
  --build-dir <p>   Custom build directory (default: build-ci-local)
  -j, --jobs <n>    Parallel build jobs
  -h, --help        Show help

Env vars:
  BUILD_DIR         Same as --build-dir
  JOBS              Same as --jobs
  OPENSCP_WERROR    ON to treat first-party compiler warnings as errors

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
      --werror)
        WERROR=ON
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


configure_project() {
  local -a cmake_args
  cmake_args=(
    -S "$REPO_DIR"
    -B "$BUILD_DIR"
    -DOPENSCP_BUILD_TESTS=ON
    "-DOPENSCP_WERROR=${WERROR}"
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

    local effective_qt_prefix=""
    if [[ -n "$effective_qt6_dir" ]]; then
      effective_qt_prefix="$(cd "$effective_qt6_dir/../../.." && pwd)"
    fi
    setup_qt_host_wrappers_if_needed "$effective_qt_prefix" "$BUILD_DIR" 0 warn
    if [[ -n "$QT_HOST_WRAP_DIR" ]]; then
      cmake_args+=("-DCMAKE_AUTOUIC_EXECUTABLE=${QT_HOST_WRAP_DIR}/uic")
      cmake_args+=("-DCMAKE_AUTORCC_EXECUTABLE=${QT_HOST_WRAP_DIR}/rcc")
      cmake_args+=("-DCMAKE_AUTOMOC_EXECUTABLE=${QT_HOST_WRAP_DIR}/moc")
      cmake_args+=("-DOPENSCP_QT_HOST_TOOLS_DIR=${QT_HOST_WRAP_DIR}")
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

  log "Building all configured test targets"
  cmake "${build_args[@]}" --target openscp_test_binaries

  if [[ "$RUN_FULL" -eq 1 ]]; then
    log "Building GUI app target (openscp)"
    cmake "${build_args[@]}" --target openscp
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
