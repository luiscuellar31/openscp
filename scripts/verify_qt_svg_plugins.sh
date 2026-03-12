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
  --allow-runtime-provided  Do not fail if qsvg/qsvgicon/qxcb are missing from package payload.
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

svg_plugins=()
while IFS= read -r plugin_path; do
  svg_plugins+=("$plugin_path")
done < <(collect_plugins qsvg)

svg_icon_plugins=()
while IFS= read -r plugin_path; do
  svg_icon_plugins+=("$plugin_path")
done < <(collect_plugins qsvgicon)

platform_plugins=()
while IFS= read -r plugin_path; do
  platform_plugins+=("$plugin_path")
done < <(collect_plugins qxcb)

missing=()
if [[ ${#svg_plugins[@]} -eq 0 ]]; then
  missing+=("qsvg (imageformats)")
fi
if [[ ${#svg_icon_plugins[@]} -eq 0 ]]; then
  missing+=("qsvgicon (iconengines)")
fi
if [[ ${#platform_plugins[@]} -eq 0 ]]; then
  missing+=("qxcb (platforms)")
fi

if [[ ${#svg_plugins[@]} -gt 0 ]]; then
  log "Found qsvg plugin(s):"
  printf '%s\n' "${svg_plugins[@]}"
fi
if [[ ${#svg_icon_plugins[@]} -gt 0 ]]; then
  log "Found qsvgicon plugin(s):"
  printf '%s\n' "${svg_icon_plugins[@]}"
fi
if [[ ${#platform_plugins[@]} -gt 0 ]]; then
  log "Found qxcb platform plugin(s):"
  printf '%s\n' "${platform_plugins[@]}"
fi

if [[ ${#missing[@]} -eq 0 ]]; then
  log "Qt plugin presence validation passed (${CONTEXT})."
else
  if [[ $ALLOW_RUNTIME_PROVIDED -eq 1 ]]; then
    warn "Missing Qt plugin(s) in payload (${CONTEXT}): ${missing[*]}"
    warn "Continuing because --allow-runtime-provided is enabled."
  else
    die "Missing Qt plugin(s) in payload (${CONTEXT}): ${missing[*]}"
  fi
fi

if [[ $ALLOW_RUNTIME_PROVIDED -eq 1 ]]; then
  warn "Skipping ldd dependency validation because runtime-provided plugins are allowed (${CONTEXT})."
  exit 0
fi

command -v ldd >/dev/null 2>&1 || die "ldd is required for dependency validation (${CONTEXT})."
command -v file >/dev/null 2>&1 || die "file is required for dependency validation (${CONTEXT})."

declare -a targets
targets=()

while IFS= read -r exe; do
  targets+=("$exe")
done < <(find "$ROOT" -type f -name "openscp_hello" | sort -u)

targets+=("${svg_plugins[@]}")
targets+=("${svg_icon_plugins[@]}")
targets+=("${platform_plugins[@]}")

if [[ ${#targets[@]} -eq 0 ]]; then
  die "No ELF targets found for dependency validation (${CONTEXT})."
fi

unique_targets=()
while IFS= read -r target_path; do
  unique_targets+=("$target_path")
done < <(printf '%s\n' "${targets[@]}" | awk 'NF' | sort -u)

ldd_failures=0
for target in "${unique_targets[@]}"; do
  [[ -e "$target" ]] || continue
  if ! file -b "$target" | grep -q "ELF"; then
    continue
  fi

  ldd_out="$(ldd "$target" 2>&1 || true)"
  if [[ -z "$ldd_out" ]]; then
    warn "ldd produced no output for: $target"
    continue
  fi

  missing_lines="$(printf '%s\n' "$ldd_out" | grep -F "not found" || true)"
  if [[ -n "$missing_lines" ]]; then
    err "Missing shared libraries for: $target"
    printf '%s\n' "$missing_lines"
    ldd_failures=1
  fi
done

if [[ $ldd_failures -ne 0 ]]; then
  die "Dependency validation failed (${CONTEXT})."
fi

log "Qt runtime dependency validation passed (${CONTEXT})."
