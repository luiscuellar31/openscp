#!/usr/bin/env bash

set -euo pipefail

log() { printf "\033[1;34m[verify]\033[0m %s\n" "$*"; }
warn() { printf "\033[1;33m[warn  ]\033[0m %s\n" "$*"; }
err() { printf "\033[1;31m[err   ]\033[0m %s\n" "$*"; }
die() { err "$*"; exit 1; }

usage() {
  cat << 'EOF'
Usage:
  verify_qt_svg_plugins.sh [--allow-runtime-provided] [--context <name>] <package-root>

Options:
  --allow-runtime-provided  Do not fail if qsvg/qsvgicon are missing from package payload.
                            Use only when Qt plugins are expected from an external runtime.
  --context <name>          Optional label for diagnostics (e.g. appimage, snap, flatpak).
EOF
}

ALLOW_RUNTIME_PROVIDED=0
CONTEXT="package"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --allow-runtime-provided)
      ALLOW_RUNTIME_PROVIDED=1
      shift
      ;;
    --context)
      [[ $# -ge 2 ]] || die "Missing value for --context"
      CONTEXT="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    --)
      shift
      break
      ;;
    -*)
      die "Unknown option: $1"
      ;;
    *)
      break
      ;;
  esac
done

if [[ $# -ne 1 ]]; then
  usage
  die "Expected exactly one <package-root> argument"
fi

ROOT="$1"
[[ -d "$ROOT" ]] || die "Directory not found: $ROOT"

collect_plugins() {
  local plugin_name="$1"
  find "$ROOT" \( -type f -o -type l \) \
    \( -name "lib${plugin_name}.so*" -o -name "${plugin_name}.so*" -o \
       -name "lib${plugin_name}.dylib" -o -name "${plugin_name}.dylib" \) \
    | sort -u
}

mapfile -t svg_plugins < <(collect_plugins qsvg)
mapfile -t svg_icon_plugins < <(collect_plugins qsvgicon)

missing=()
if [[ ${#svg_plugins[@]} -eq 0 ]]; then
  missing+=("qsvg (imageformats)")
fi
if [[ ${#svg_icon_plugins[@]} -eq 0 ]]; then
  missing+=("qsvgicon (iconengines)")
fi

if [[ ${#svg_plugins[@]} -gt 0 ]]; then
  log "Found qsvg plugin(s):"
  printf '%s\n' "${svg_plugins[@]}"
fi
if [[ ${#svg_icon_plugins[@]} -gt 0 ]]; then
  log "Found qsvgicon plugin(s):"
  printf '%s\n' "${svg_icon_plugins[@]}"
fi

if [[ ${#missing[@]} -eq 0 ]]; then
  log "Qt SVG plugins validation passed (${CONTEXT})."
  exit 0
fi

if [[ $ALLOW_RUNTIME_PROVIDED -eq 1 ]]; then
  warn "Missing Qt SVG plugin(s) in payload (${CONTEXT}): ${missing[*]}"
  warn "Continuing because --allow-runtime-provided is enabled."
  exit 0
fi

die "Missing Qt SVG plugin(s) in payload (${CONTEXT}): ${missing[*]}"
