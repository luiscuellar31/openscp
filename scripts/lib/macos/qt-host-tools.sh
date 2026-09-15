#!/usr/bin/env bash

# Shared by local builds, CI checks and packaging on Apple Silicon. Callers
# provide warn() and die() so diagnostics retain each script's own prefix.

openscp_qt_tool_runs() {
  { ( "$@" ) >/dev/null 2>&1; } 2>/dev/null
}

openscp_create_qt_x86_wrapper() {
  local target="$1"
  local real_bin="$2"
  printf '#!/usr/bin/env bash\nexec /usr/bin/arch -x86_64 %q "$@"\n' \
    "$real_bin" > "$target"
  chmod +x "$target"
}

setup_qt_host_wrappers_if_needed() {
  local qt_prefix="$1"
  local build_dir="$2"
  local require_lrelease="${3:-1}"
  local failure_mode="${4:-die}"
  QT_HOST_WRAP_DIR=""

  [[ "$(uname -s)" == "Darwin" ]] || return 0
  [[ "$(uname -m)" == "arm64" ]] || return 0
  [[ -n "$qt_prefix" ]] || return 0

  local uic="${qt_prefix}/libexec/uic"
  local rcc="${qt_prefix}/libexec/rcc"
  local moc="${qt_prefix}/libexec/moc"
  local lrelease="${qt_prefix}/libexec/lrelease"
  if [[ ! -x "$lrelease" ]]; then
    lrelease="${qt_prefix}/bin/lrelease"
  fi
  [[ -x "$uic" && -x "$rcc" && -x "$moc" ]] || return 0
  if [[ ! -x "$lrelease" ]]; then
    [[ "$require_lrelease" == "1" ]] && return 0
    lrelease=""
  fi

  if openscp_qt_tool_runs "$uic" -h &&
     openscp_qt_tool_runs "$rcc" -h &&
     openscp_qt_tool_runs "$moc" -h &&
     { [[ -z "$lrelease" ]] || openscp_qt_tool_runs "$lrelease" -version; }; then
    return 0
  fi
  if ! openscp_qt_tool_runs /usr/bin/arch -x86_64 "$uic" -h ||
     ! openscp_qt_tool_runs /usr/bin/arch -x86_64 "$rcc" -h ||
     ! openscp_qt_tool_runs /usr/bin/arch -x86_64 "$moc" -h ||
     { [[ -n "$lrelease" ]] &&
       ! openscp_qt_tool_runs /usr/bin/arch -x86_64 "$lrelease" -version; }; then
    local message="Qt host tools are not runnable natively or through Rosetta under ${qt_prefix}"
    if [[ "$failure_mode" == "die" ]]; then
      die "$message"
    else
      warn "$message"
    fi
    return 0
  fi

  QT_HOST_WRAP_DIR="${build_dir}/qt-tools-wrap"
  mkdir -p "$QT_HOST_WRAP_DIR"
  openscp_create_qt_x86_wrapper "${QT_HOST_WRAP_DIR}/uic" "$uic"
  openscp_create_qt_x86_wrapper "${QT_HOST_WRAP_DIR}/rcc" "$rcc"
  openscp_create_qt_x86_wrapper "${QT_HOST_WRAP_DIR}/moc" "$moc"
  if [[ -n "$lrelease" ]]; then
    openscp_create_qt_x86_wrapper "${QT_HOST_WRAP_DIR}/lrelease" "$lrelease"
  fi
  warn "Qt host tools are not runnable natively; using their x86_64 slices through Rosetta."
}
