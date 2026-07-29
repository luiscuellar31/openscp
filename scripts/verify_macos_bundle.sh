#!/usr/bin/env bash

set -euo pipefail

log() { printf "\033[1;34m[verify]\033[0m %s\n" "$*"; }
err() { printf "\033[1;31m[err ]\033[0m %s\n" "$*" >&2; }
die() { err "$*"; exit 1; }

usage() {
  cat <<'EOF'
Usage: ./scripts/verify_macos_bundle.sh <path-to-OpenSCP.app>

Validates:
- Info.plist contains a valid minimum macOS version and every bundled Mach-O
  supports that version
- Required Qt runtime files are present (including qcocoa platform plugin)
- App, plugin, and framework dependencies avoid machine-local absolute paths
- @rpath/@loader_path/@executable_path dependencies resolve within the bundle
EOF
}

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

version_is_greater() {
  local candidate="$1"
  local allowed="$2"
  awk -v candidate="$candidate" -v allowed="$allowed" 'BEGIN {
    candidate_count = split(candidate, candidate_parts, ".")
    allowed_count = split(allowed, allowed_parts, ".")
    count = candidate_count > allowed_count ? candidate_count : allowed_count
    for (part_index = 1; part_index <= count; ++part_index) {
      candidate_part = candidate_parts[part_index] + 0
      allowed_part = allowed_parts[part_index] + 0
      if (candidate_part > allowed_part)
        exit 0
      if (candidate_part < allowed_part)
        exit 1
    }
    exit 1
  }'
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
while IFS= read -r plugin_dylib; do
  targets+=("$plugin_dylib")
done < <(find "$PLUGINS_DIR" -type f -name '*.dylib' | sort)

while IFS= read -r framework_file; do
  [[ -n "$framework_file" ]] || continue
  if /usr/bin/file -b "$framework_file" | grep -q 'Mach-O'; then
    targets+=("$framework_file")
  fi
done < <(find "$FRAMEWORKS_DIR" -type f | sort)

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

log "macOS bundle validation passed: $APP_DIR"
