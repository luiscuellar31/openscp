#!/usr/bin/env bash
set -euo pipefail

[[ $# -eq 1 ]] || { echo "Usage: $0 <SDK directory>" >&2; exit 1; }
destination="$1"
mkdir -p "$destination"
archive="$(mktemp)"
trap 'rm -f "$archive"' EXIT
curl --fail --location --proto '=https' --proto-redir '=https' \
  https://github.com/sparkle-project/Sparkle/releases/download/2.10.0/Sparkle-2.10.0.tar.xz \
  --output "$archive"
printf '%s  %s\n' \
  c2bf58aa8387266ac179357b1415d6f2635f044da8be41042af32425dae6da0c \
  "$archive" | shasum -a 256 --check --strict
tar -xf "$archive" -C "$destination"
