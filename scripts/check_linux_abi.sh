#!/usr/bin/env bash

set -euo pipefail

log() { printf '\033[1;34m[abi]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[abi]\033[0m %s\n' "$*" >&2; exit 1; }

usage() {
  cat <<'EOF'
Usage: ./scripts/check_linux_abi.sh <ELF-file-or-directory> <max-glibc>

Checks every ELF object below the supplied path. It rejects GLIBC requirements
above the configured compatibility floor and verifies that any required
GLIBCXX symbol version is provided by the bundled libstdc++.so.6.

Set OPENSCP_REQUIRE_BUNDLED_LIBSTDCXX=0 only for non-self-contained builds.
EOF
}

[[ $# -eq 2 ]] || { usage; exit 2; }
ROOT_PATH="$1"
MAX_GLIBC="$2"
REQUIRE_BUNDLED_LIBSTDCXX="${OPENSCP_REQUIRE_BUNDLED_LIBSTDCXX:-1}"

[[ -e "$ROOT_PATH" ]] || die "Path does not exist: $ROOT_PATH"
[[ "$MAX_GLIBC" =~ ^[0-9]+\.[0-9]+$ ]] ||
  die "Invalid maximum GLIBC version: $MAX_GLIBC"
command -v readelf >/dev/null 2>&1 || die "Missing required tool: readelf"

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
      if (candidate_part > allowed_part) exit 0
      if (candidate_part < allowed_part) exit 1
    }
    exit 1
  }'
}

highest_version() {
  awk 'NF' | sort -Vu | tail -n1
}

elf_paths=()
if [[ -d "$ROOT_PATH" ]]; then
  while IFS= read -r -d '' candidate; do
    if readelf -h "$candidate" >/dev/null 2>&1; then
      elf_paths+=("$candidate")
    fi
  done < <(find -L "$ROOT_PATH" -type f -print0)
elif readelf -h "$ROOT_PATH" >/dev/null 2>&1; then
  elf_paths+=("$ROOT_PATH")
fi

[[ ${#elf_paths[@]} -gt 0 ]] || die "No ELF objects found under: $ROOT_PATH"

max_glibc_seen=""
max_glibcxx_required=""
glibc_failure=0

for elf_path in "${elf_paths[@]}"; do
  versions="$(readelf --version-info --wide "$elf_path" 2>/dev/null |
    grep -Eo 'GLIBC_[0-9]+(\.[0-9]+)+' | sed 's/^GLIBC_//' | sort -Vu || true)"
  if [[ -n "$versions" ]]; then
    elf_max="$(printf '%s\n' "$versions" | highest_version)"
    if [[ -z "$max_glibc_seen" ]] ||
       version_is_greater "$elf_max" "$max_glibc_seen"; then
      max_glibc_seen="$elf_max"
    fi
    if version_is_greater "$elf_max" "$MAX_GLIBC"; then
      printf '[abi] %s requires GLIBC_%s (maximum allowed: GLIBC_%s)\n' \
        "$elf_path" "$elf_max" "$MAX_GLIBC" >&2
      glibc_failure=1
    fi
  fi

  required_glibcxx="$(readelf --dyn-syms --wide "$elf_path" 2>/dev/null |
    awk '$7 == "UND"' |
    grep -Eo 'GLIBCXX_[0-9]+(\.[0-9]+)+' |
    sed 's/^GLIBCXX_//' | sort -Vu || true)"
  if [[ -n "$required_glibcxx" ]]; then
    elf_glibcxx_max="$(printf '%s\n' "$required_glibcxx" | highest_version)"
    if [[ -z "$max_glibcxx_required" ]] ||
       version_is_greater "$elf_glibcxx_max" "$max_glibcxx_required"; then
      max_glibcxx_required="$elf_glibcxx_max"
    fi
  fi
done

[[ $glibc_failure -eq 0 ]] || die "GLIBC compatibility check failed"

bundled_libstdcxx=""
if [[ -d "$ROOT_PATH" ]]; then
  bundled_libstdcxx="$(find -L "$ROOT_PATH" -type f -name 'libstdc++.so.6*' -print -quit || true)"
fi

if [[ -n "$max_glibcxx_required" ]]; then
  if [[ -z "$bundled_libstdcxx" ]]; then
    if [[ "$REQUIRE_BUNDLED_LIBSTDCXX" == "1" ]]; then
      die "GLIBCXX_${max_glibcxx_required} is required, but libstdc++.so.6 is not bundled"
    fi
    log "Maximum required GLIBCXX: ${max_glibcxx_required} (system runtime permitted)"
  else
    max_glibcxx_provided="$(readelf --version-info --wide "$bundled_libstdcxx" 2>/dev/null |
      grep -Eo 'GLIBCXX_[0-9]+(\.[0-9]+)+' |
      sed 's/^GLIBCXX_//' | highest_version)"
    [[ -n "$max_glibcxx_provided" ]] ||
      die "Could not read provided GLIBCXX versions from $bundled_libstdcxx"
    if version_is_greater "$max_glibcxx_required" "$max_glibcxx_provided"; then
      die "GLIBCXX_${max_glibcxx_required} is required, but bundled libstdc++ provides only GLIBCXX_${max_glibcxx_provided}"
    fi
    log "GLIBCXX required/provided: ${max_glibcxx_required}/${max_glibcxx_provided}"
  fi
fi

log "Checked ${#elf_paths[@]} ELF objects; maximum GLIBC requirement: ${max_glibc_seen:-none} (allowed: ${MAX_GLIBC})"
