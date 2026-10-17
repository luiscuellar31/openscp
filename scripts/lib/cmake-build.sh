#!/usr/bin/env bash
# CMake build settings shared by the build and packaging scripts.

# How many compile jobs to run: JOBS when set, otherwise the CPU count.
# `cmake --build -j` without a number runs `make -j` without a limit.
build_jobs() {
  if [[ -n "${JOBS:-}" ]]; then
    printf '%s\n' "$JOBS"
    return
  fi
  getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null ||
    printf '2\n'
}

# Whether to configure `build_dir` with Ninja: only a new build directory,
# since CMake keeps the generator a cache was created with, only without a
# CMAKE_GENERATOR chosen by the caller, and only when ninja is installed.
use_ninja_generator() {
  local build_dir="$1"
  [[ -z "${CMAKE_GENERATOR:-}" && ! -f "${build_dir}/CMakeCache.txt" ]] &&
    command -v ninja >/dev/null 2>&1
}
