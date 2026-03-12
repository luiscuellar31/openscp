#!/usr/bin/env bash

set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MANIFEST="${MANIFEST:-${REPO_DIR}/packaging/flatpak/com.openscp.OpenSCP.yml}"
BUILD_DIR="${BUILD_DIR:-${REPO_DIR}/dist/flatpak/build-dir}"
REPO_OUT="${REPO_OUT:-${REPO_DIR}/dist/flatpak/repo}"
BUNDLE_OUT="${BUNDLE_OUT:-${REPO_DIR}/dist/OpenSCP.flatpak}"
APP_ID="${APP_ID:-com.openscp.OpenSCP}"
BRANCH="${BRANCH:-stable}"
RUNTIME_VERSION_OVERRIDE="${RUNTIME_VERSION_OVERRIDE:-}"
CHECKER="${REPO_DIR}/scripts/verify_qt_svg_plugins.sh"
APP_ICON="${APP_ICON:-${REPO_DIR}/assets/program/icon-openscp-256.png}"

log() { printf "\033[1;34m[flatpak]\033[0m %s\n" "$*"; }
err() { printf "\033[1;31m[err    ]\033[0m %s\n" "$*"; }
die() { err "$*"; exit 1; }

ensure_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "Missing required tool: $1"
}

validate_png_256() {
  local icon_file="$1"
  [[ -f "$icon_file" ]] || die "Required PNG icon not found: $icon_file"
  local meta
  meta="$(file -b "$icon_file" 2>/dev/null || true)"
  [[ "$meta" == PNG\ image\ data,* ]] ||
    die "Icon is not a PNG image: $icon_file"
  if [[ ! "$meta" =~ ([0-9]+)[[:space:]]x[[:space:]]([0-9]+) ]]; then
    die "Could not detect PNG dimensions for: $icon_file"
  fi
  local width="${BASH_REMATCH[1]}"
  local height="${BASH_REMATCH[2]}"
  if [[ "$width" != "256" || "$height" != "256" ]]; then
    die "Flatpak source icon must be 256x256 (got ${width}x${height}): $icon_file"
  fi
}

uses_external_qt_runtime() {
  local manifest="$1"
  local runtime
  runtime="$(
    awk -F':[[:space:]]*' '/^runtime:/{print $2; exit}' "$manifest" \
      | tr -d '"' \
      | tr -d "'"
  )"
  case "$runtime" in
    org.kde.Platform|org.gnome.Platform|org.freedesktop.Platform)
      return 0
      ;;
    *)
      return 1
      ;;
  esac
}

main() {
  [[ "$(uname -s)" == "Linux" ]] || die "Flatpak packaging must run on Linux."
  [[ -f "$MANIFEST" ]] || die "Manifest not found: $MANIFEST"
  [[ -x "$CHECKER" ]] || die "Qt runtime checker not found/executable: $CHECKER"

  ensure_cmd flatpak-builder
  ensure_cmd flatpak
  ensure_cmd file
  validate_png_256 "$APP_ICON"

  mkdir -p "$(dirname "$BUILD_DIR")" "$(dirname "$BUNDLE_OUT")" "$REPO_OUT"

  local build_manifest="$MANIFEST"
  local tmp_manifest=""
  if [[ -n "$RUNTIME_VERSION_OVERRIDE" ]]; then
    tmp_manifest="$(mktemp)"
    sed -E \
      "s|^runtime-version:[[:space:]]*\"?[^\"]+\"?[[:space:]]*$|runtime-version: \"${RUNTIME_VERSION_OVERRIDE}\"|" \
      "$MANIFEST" > "$tmp_manifest"
    build_manifest="$tmp_manifest"
    trap "rm -f '$tmp_manifest'" EXIT
    log "Using runtime-version override: $RUNTIME_VERSION_OVERRIDE"
  fi

  log "Building Flatpak from manifest: $build_manifest"
  flatpak-builder \
    --force-clean \
    --default-branch="$BRANCH" \
    --repo="$REPO_OUT" \
    "$@" \
    "$BUILD_DIR" \
    "$build_manifest"

  if uses_external_qt_runtime "$build_manifest"; then
    log "Detected external Flatpak runtime; allowing runtime-provided Qt plugins."
    "$CHECKER" --allow-runtime-provided --context flatpak "${BUILD_DIR}/files"
  else
    "$CHECKER" --context flatpak "${BUILD_DIR}/files"
  fi

  log "Bundling Flatpak: $BUNDLE_OUT"
  flatpak build-bundle "$REPO_OUT" "$BUNDLE_OUT" "$APP_ID" "$BRANCH"
  log "Done: $BUNDLE_OUT"
}

main "$@"
