#!/usr/bin/env bash

set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROJECT_DIR="${PROJECT_DIR:-${REPO_DIR}/packaging/snap}"
CHECKER="${REPO_DIR}/scripts/verify_qt_svg_plugins.sh"
SNAPCRAFT="${SNAPCRAFT:-snapcraft}"

log() { printf "\033[1;34m[snap]\033[0m %s\n" "$*"; }
err() { printf "\033[1;31m[err ]\033[0m %s\n" "$*"; }
die() { err "$*"; exit 1; }

ensure_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "Missing required tool: $1"
}

validate_snap_icon() {
  local icon_file="${PROJECT_DIR}/icon.png"
  [[ -f "$icon_file" ]] || die "Required snap icon not found: $icon_file"
  local size_bytes
  size_bytes="$(wc -c < "$icon_file" | tr -d ' ')"
  if [[ "$size_bytes" -gt 262144 ]]; then
    die "snap icon exceeds 256KB (${size_bytes} bytes): $icon_file"
  fi
}

uses_kde_neon_extension() {
  local manifest="${PROJECT_DIR}/snapcraft.yaml"
  [[ -f "$manifest" ]] || return 1
  grep -q "kde-neon-6" "$manifest"
}

main() {
  [[ "$(uname -s)" == "Linux" ]] || die "Snap packaging must run on Linux."
  [[ -d "$PROJECT_DIR" ]] || die "Snap project directory not found: $PROJECT_DIR"
  [[ -x "$CHECKER" ]] || die "SVG plugin checker not found/executable: $CHECKER"

  ensure_cmd "$SNAPCRAFT"
  ensure_cmd unsquashfs
  validate_snap_icon

  log "Building snap from: $PROJECT_DIR"
  pushd "$PROJECT_DIR" >/dev/null
  "$SNAPCRAFT" "$@"

  local snap_file
  snap_file="$(ls -1t ./*.snap 2>/dev/null | head -n1 || true)"
  [[ -n "$snap_file" ]] || die "No .snap artifact was produced."
  log "Produced snap: $snap_file"
  popd >/dev/null

  local tmp_dir
  tmp_dir="$(mktemp -d)"
  trap 'rm -rf "$tmp_dir"' EXIT

  log "Extracting snap for plugin validation"
  unsquashfs -f -d "${tmp_dir}/root" "${PROJECT_DIR}/$(basename "$snap_file")" >/dev/null
  if uses_kde_neon_extension; then
    log "Detected kde-neon-6 extension; allowing runtime-provided SVG plugins."
    "$CHECKER" --allow-runtime-provided --context snap "${tmp_dir}/root"
  else
    "$CHECKER" --context snap "${tmp_dir}/root"
  fi

  log "Done: ${PROJECT_DIR}/$(basename "$snap_file")"
}

main "$@"
