#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/../lib/version.sh"

log() { printf "\033[1;34m[verify]\033[0m %s\n" "$*"; }
err() { printf "\033[1;31m[err ]\033[0m %s\n" "$*" >&2; }
die() { err "$*"; exit 1; }

usage() {
  cat <<'EOF'
Usage: ./scripts/verify/macos-bundle.sh <path-to-OpenSCP.app>

Validates:
- Info.plist contains a valid minimum macOS version and every bundled Mach-O
  supports that version
- Required Qt runtime files are present (including qcocoa platform plugin)
- Qt frameworks contain runtime files only, without SDK headers/modules
- App, plugin, and framework dependencies avoid machine-local absolute paths
- @rpath/@loader_path/@executable_path dependencies resolve within the bundle
- Every bundled framework/library Mach-O is reachable from the app or a plugin
- Bundle size stays within MAX_BUNDLE_SIZE_MIB when that variable is set
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

[[ $# -eq 1 ]] || { usage; die "Expected exactly one argument"; }

APP_DIR="$1"
[[ -d "$APP_DIR" ]] || die "App bundle not found: $APP_DIR"

CONTENTS_DIR="${APP_DIR}/Contents"
MACOS_DIR="${CONTENTS_DIR}/MacOS"
FRAMEWORKS_DIR="${CONTENTS_DIR}/Frameworks"
PLUGINS_DIR="${CONTENTS_DIR}/PlugIns"
EXE_PATH="${MACOS_DIR}/OpenSCP"
INFO_PLIST_PATH="${CONTENTS_DIR}/Info.plist"
QT_CONF_PATH="${CONTENTS_DIR}/Resources/qt.conf"
COCOA_PLUGIN="${PLUGINS_DIR}/platforms/libqcocoa.dylib"

[[ -x "$EXE_PATH" ]] || die "Missing executable: $EXE_PATH"
[[ -f "$INFO_PLIST_PATH" ]] || die "Missing Info.plist: $INFO_PLIST_PATH"
[[ -f "$QT_CONF_PATH" ]] || die "Missing qt.conf: $QT_CONF_PATH"
[[ -f "$COCOA_PLUGIN" ]] || die "Missing Qt cocoa platform plugin: $COCOA_PLUGIN"

minimum_system_version="$(
  /usr/libexec/PlistBuddy -c 'Print :LSMinimumSystemVersion' \
    "$INFO_PLIST_PATH" 2>/dev/null || true
)"
[[ "$minimum_system_version" =~ ^[0-9]+(\.[0-9]+){1,2}$ ]] ||
  die "Invalid LSMinimumSystemVersion in Info.plist: '${minimum_system_version}'"

require_file() {
  local file="$1"
  [[ -e "$file" ]] || die "Missing required bundle file: $file"
}

require_file "${FRAMEWORKS_DIR}/QtCore.framework/Versions/A/QtCore"
require_file "${FRAMEWORKS_DIR}/QtGui.framework/Versions/A/QtGui"
require_file "${FRAMEWORKS_DIR}/QtWidgets.framework/Versions/A/QtWidgets"

development_content="$({
  find "$FRAMEWORKS_DIR" -type d \( -name Headers -o -name Modules \) -print
} | sort)"
if [[ -n "$development_content" ]]; then
  err "Development-only Qt framework content is bundled:"
  printf '%s\n' "$development_content" >&2
  die "Qt frameworks must contain runtime content only"
fi

path_exists() {
  local p="$1"
  if [[ -e "$p" ]]; then
    return 0
  fi
  local dir base
  dir="$(dirname "$p")"
  base="$(basename "$p")"
  (cd "$dir" 2>/dev/null && [[ -e "$(pwd -P)/${base}" ]])
}

resolve_dep_path() {
  local dep="$1"
  local owner="$2"
  case "$dep" in
    @rpath/*)
      printf "%s/%s\n" "$FRAMEWORKS_DIR" "${dep#@rpath/}"
      ;;
    @executable_path/*)
      printf "%s/%s\n" "$MACOS_DIR" "${dep#@executable_path/}"
      ;;
    @loader_path/*)
      local owner_dir
      owner_dir="$(cd "$(dirname "$owner")" && pwd)"
      printf "%s/%s\n" "$owner_dir" "${dep#@loader_path/}"
      ;;
    *)
      printf "\n"
      ;;
  esac
}

check_forbidden_absolute_refs() {
  local file="$1"
  local forbidden_regex='^/(opt/homebrew|usr/local/(Cellar|opt)|Users/runner|Users/|private/tmp|tmp|var/folders|.*miniconda.*|.*anaconda.*)'
  local deps
  deps="$(list_deps "$file")"
  local bad_refs
  bad_refs="$(printf "%s\n" "$deps" | grep -E "$forbidden_regex" || true)"
  if [[ -n "$bad_refs" ]]; then
    err "Forbidden absolute references in: $file"
    printf "%s\n" "$bad_refs" >&2
    return 1
  fi
  return 0
}

list_rpaths() {
  local file="$1"
  local out
  if ! out="$(otool -l "$file" 2>&1)"; then
    err "$out"
    die "otool failed while listing rpaths for: $file"
  fi
  if [[ "$out" == *"You have not agreed to the Xcode license agreements"* ]]; then
    die "otool is unavailable (Xcode license not accepted); cannot validate rpaths for $file"
  fi
  printf "%s\n" "$out" | awk '
    $1 == "cmd" && $2 == "LC_RPATH" { in_rpath=1; next }
    in_rpath && $1 == "path" { print $2; in_rpath=0 }
  '
}

check_forbidden_rpaths() {
  local file="$1"
  local rpaths
  rpaths="$(list_rpaths "$file")"
  local bad_rpaths
  bad_rpaths="$(
    printf "%s\n" "$rpaths" |
      grep -E '^/' |
      grep -Ev '^(/System/|/usr/lib/)' || true
  )"
  if [[ -n "$bad_rpaths" ]]; then
    err "Forbidden absolute rpath(s) in: $file"
    printf "%s\n" "$bad_rpaths" >&2
    return 1
  fi
  return 0
}

list_deps() {
  local file="$1"
  local out
  if ! out="$(otool -L "$file" 2>&1)"; then
    err "$out"
    die "otool failed for: $file"
  fi
  if [[ "$out" == *"You have not agreed to the Xcode license agreements"* ]]; then
    die "otool is unavailable (Xcode license not accepted); cannot validate $file"
  fi
  local deps
  # Universal binaries repeat the non-indented owner header for each
  # architecture. Dependency rows are indented; de-duplicate paths shared by
  # multiple slices.
  deps="$(
    printf "%s\n" "$out" |
      awk '/^[[:space:]]/ && NF > 0 {
        if (!seen[$1]++) print $1
      }'
  )"
  [[ -n "$deps" ]] || die "No dependencies were reported by otool for: $file"
  printf "%s\n" "$deps"
}

list_minimum_macos_versions() {
  local file="$1"
  local out
  if ! out="$(otool -l "$file" 2>&1)"; then
    err "$out"
    die "otool failed while reading deployment targets for: $file"
  fi
  printf "%s\n" "$out" | awk '
    $1 == "cmd" {
      in_build_version = ($2 == "LC_BUILD_VERSION")
      in_legacy_version = ($2 == "LC_VERSION_MIN_MACOSX")
      next
    }
    in_build_version && $1 == "minos" {
      print $2
      in_build_version = 0
      next
    }
    in_legacy_version && $1 == "version" {
      print $2
      in_legacy_version = 0
    }
  '
}


check_minimum_macos_version() {
  local file="$1"
  local versions
  versions="$(list_minimum_macos_versions "$file")"
  [[ -n "$versions" ]] ||
    die "No minimum macOS version was reported for: $file"

  local version
  while IFS= read -r version; do
    [[ "$version" =~ ^[0-9]+(\.[0-9]+){1,2}$ ]] ||
      die "Invalid Mach-O minimum macOS version in $file: '${version}'"
    if version_is_greater "$version" "$minimum_system_version"; then
      die "$(basename "$file") requires macOS ${version}, but Info.plist advertises ${minimum_system_version}"
    fi
  done <<<"$versions"
}

check_linkage() {
  local file="$1"
  local deps
  deps="$(list_deps "$file")"
  local dep resolved
  while IFS= read -r dep; do
    [[ -n "$dep" ]] || continue
    case "$dep" in
      /System/*|/usr/lib/*)
        continue
        ;;
      @rpath/*|@executable_path/*|@loader_path/*)
        resolved="$(resolve_dep_path "$dep" "$file")"
        if [[ -z "$resolved" ]] || ! path_exists "$resolved"; then
          die "Unresolved dependency in $(basename "$file"): ${dep} (expected at ${resolved})"
        fi
        ;;
      *)
        die "Unexpected non-system absolute dependency in $(basename "$file"): ${dep}"
        ;;
    esac
  done <<< "$deps"
}

targets=("$EXE_PATH")
runtime_roots=("$EXE_PATH")
while IFS= read -r plugin_dylib; do
  targets+=("$plugin_dylib")
  runtime_roots+=("$plugin_dylib")
done < <(find "$PLUGINS_DIR" -type f -name '*.dylib' | sort)

while IFS= read -r framework_file; do
  [[ -n "$framework_file" ]] || continue
  if /usr/bin/file -b "$framework_file" | grep -q 'Mach-O'; then
    targets+=("$framework_file")
  fi
done < <(find "$FRAMEWORKS_DIR" -type f | sort)

array_contains() {
  local needle="$1"
  shift
  local candidate=""
  for candidate in "$@"; do
    [[ "$candidate" == "$needle" ]] && return 0
  done
  return 1
}

canonical_path() {
  local path="$1"
  local directory=""
  directory="$(dirname "$path")"
  local filename=""
  filename="$(basename "$path")"
  (cd "$directory" 2>/dev/null && printf '%s/%s\n' "$(pwd -P)" "$filename") ||
    printf '%s\n' "$path"
}

check_macho_reachability() {
  # Bash 3.2 with `set -u` treats expansion of an empty array as unbound.
  # A non-path sentinel keeps the membership helper portable to stock macOS.
  local -a reachable=("__openscp_no_reachable_path__")
  local -a pending=("${runtime_roots[@]}")
  local owner="" owner_path="" dep="" resolved="" resolved_path=""
  local frameworks_path="" plugins_path=""
  frameworks_path="$(canonical_path "$FRAMEWORKS_DIR")"
  plugins_path="$(canonical_path "$PLUGINS_DIR")"

  while ((${#pending[@]})); do
    owner="${pending[0]}"
    if ((${#pending[@]} == 1)); then
      pending=()
    else
      pending=("${pending[@]:1}")
    fi
    owner_path="$(canonical_path "$owner")"
    if array_contains "$owner_path" "${reachable[@]}"; then
      continue
    fi
    reachable+=("$owner_path")

    while IFS= read -r dep; do
      [[ -n "$dep" ]] || continue
      resolved="$(resolve_dep_path "$dep" "$owner")"
      [[ -e "$resolved" ]] || continue
      resolved_path="$(canonical_path "$resolved")"
      case "$resolved_path" in
        "$frameworks_path"/*|"$plugins_path"/*)
          if /usr/bin/file -b "$resolved_path" | grep -q 'Mach-O' &&
             ! array_contains "$resolved_path" "${reachable[@]}"; then
            pending+=("$resolved_path")
          fi
          ;;
        *) ;;
      esac
    done < <(list_deps "$owner")
  done

  local -a unreachable=()
  local binary="" binary_path=""
  for binary in "${targets[@]}"; do
    binary_path="$(canonical_path "$binary")"
    if ! array_contains "$binary_path" "${reachable[@]}"; then
      unreachable+=("$binary")
    fi
  done

  if ((${#unreachable[@]})); then
    err "Unreachable Mach-O binaries are bundled:"
    printf '%s\n' "${unreachable[@]}" >&2
    die "Bundle contains runtime binaries that the app cannot reach"
  fi
}

check_bundle_size_budget() {
  local maximum_mib="${MAX_BUNDLE_SIZE_MIB:-0}"
  [[ "$maximum_mib" =~ ^[0-9]+$ ]] ||
    die "MAX_BUNDLE_SIZE_MIB must be a non-negative integer"
  ((maximum_mib > 0)) || return 0

  local total_bytes=""
  total_bytes="$(find "$APP_DIR" -type f -exec stat -f '%z' {} + |
    awk '{ total += $1 } END { printf "%.0f", total }')"
  local maximum_bytes=$((maximum_mib * 1024 * 1024))
  local actual_mib=""
  actual_mib="$(awk -v bytes="$total_bytes" 'BEGIN { printf "%.1f", bytes / 1048576 }')"
  log "Bundle size: ${actual_mib} MiB (budget: ${maximum_mib} MiB)"
  ((total_bytes <= maximum_bytes)) ||
    die "Bundle size ${actual_mib} MiB exceeds ${maximum_mib} MiB budget"
}

log "Checking deployment targets for ${#targets[@]} Mach-O binaries"
for bin in "${targets[@]}"; do
  check_minimum_macos_version "$bin"
done

log "Checking Mach-O linkage for ${#targets[@]} binaries"
for bin in "${targets[@]}"; do
  check_forbidden_absolute_refs "$bin"
  check_forbidden_rpaths "$bin"
  check_linkage "$bin"
done

log "Checking Mach-O reachability"
check_macho_reachability
check_bundle_size_budget

log "macOS bundle validation passed: $APP_DIR"
