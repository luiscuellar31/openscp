#!/usr/bin/env bash

set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${REPO_DIR}/build}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
JOBS="${JOBS:-}"

log() { printf "\033[1;34m[linux]\033[0m %s\n" "$*"; }
die() { printf "\033[1;31m[err ]\033[0m %s\n" "$*" >&2; exit 1; }

usage() {
  cat <<'EOF'
Usage: ./scripts/linux.sh <command>

Commands:
  configure   Configure the build directory
  build       Build the OpenSCP application
  run         Run the previously built application
  dev         Configure + build + run
  help        Show this help

Optional env vars:
  BUILD_DIR=/path/to/build             Default: build
  BUILD_TYPE=Release|Debug|...         Default: Release
  JOBS=<number>                        Parallel build jobs
  CMAKE_GENERATOR=Ninja|...            CMake generator
  CMAKE_PREFIX_PATH=/path/to/Qt        Qt installation prefix
  Qt6_DIR=/path/to/Qt6/lib/cmake/Qt6  Qt package directory

Examples:
  ./scripts/linux.sh dev
  BUILD_TYPE=Debug ./scripts/linux.sh dev
  JOBS=8 ./scripts/linux.sh build
EOF
}

require_linux() {
  [[ "$(uname -s)" == "Linux" ]] ||
    die "This development flow must run on Linux."
}

normalize_and_validate_options() {
  case "$BUILD_DIR" in
    /*) ;;
    *) BUILD_DIR="${REPO_DIR}/${BUILD_DIR}" ;;
  esac

  [[ -n "$BUILD_TYPE" ]] || die "BUILD_TYPE cannot be empty."
  if [[ -n "$JOBS" && ! "$JOBS" =~ ^[1-9][0-9]*$ ]]; then
    die "JOBS must be a positive integer."
  fi
}

configure_project() {
  local -a cmake_args=(
    -S "$REPO_DIR"
    -B "$BUILD_DIR"
    "-DCMAKE_BUILD_TYPE=${BUILD_TYPE}"
  )

  if [[ -n "${Qt6_DIR:-}" ]]; then
    cmake_args+=("-DQt6_DIR=${Qt6_DIR}")
  fi

  log "Configuring ${BUILD_TYPE} build: ${BUILD_DIR}"
  cmake "${cmake_args[@]}"
}

build_app() {
  local -a build_args=(
    --build "$BUILD_DIR"
    --target openscp
  )

  if [[ -n "$JOBS" ]]; then
    build_args+=(--parallel "$JOBS")
  else
    build_args+=(--parallel)
  fi

  log "Building OpenSCP"
  cmake "${build_args[@]}"
}

resolve_app_path() {
  local candidate="${BUILD_DIR}/openscp"
  if [[ -x "$candidate" ]]; then
    printf '%s\n' "$candidate"
    return
  fi

  candidate="${BUILD_DIR}/${BUILD_TYPE}/openscp"
  if [[ -x "$candidate" ]]; then
    printf '%s\n' "$candidate"
    return
  fi

  return 1
}

run_app() {
  local app_path=""
  app_path="$(resolve_app_path || true)"
  [[ -n "$app_path" ]] ||
    die "Application not found in ${BUILD_DIR}. Run './scripts/linux.sh build' first."

  log "Running ${app_path}"
  exec "$app_path"
}

main() {
  local command="${1:-help}"
  [[ $# -le 1 ]] || die "Only one command is accepted."

  case "$command" in
    help|-h|--help)
      usage
      return
      ;;
    configure|build|run|dev)
      ;;
    *)
      usage >&2
      die "Unknown command: ${command}"
      ;;
  esac

  require_linux
  normalize_and_validate_options

  case "$command" in
    configure)
      configure_project
      ;;
    build)
      build_app
      ;;
    run)
      run_app
      ;;
    dev)
      configure_project
      build_app
      run_app
      ;;
  esac
}

main "$@"
