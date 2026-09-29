#!/usr/bin/env bash
set -euo pipefail

[[ $# -eq 4 ]] || {
  echo "Usage: $0 <Sparkle SDK> <ZIP archive> <version> <output appcast>" >&2
  exit 1
}
sdk="$1"
archive="$2"
version="$3"
output="$4"
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "Invalid version" >&2; exit 1; }
[[ -n "${OPENSCP_SPARKLE_PRIVATE_KEY:-}" ]] || { echo "Sparkle signing secret is missing" >&2; exit 1; }
working="$(mktemp -d)"
trap 'rm -rf "$working"' EXIT
cp "$archive" "$working/"
# Standard input keeps the secret out of process arguments and shell tracing.
printf '%s\n' "$OPENSCP_SPARKLE_PRIVATE_KEY" | \
  "$sdk/bin/generate_appcast" --ed-key-file - --maximum-deltas 0 \
  --download-url-prefix "https://github.com/luiscuellar31/openscp/releases/download/v${version}/" \
  --versions "$version" -o "$working/appcast.xml" "$working"
printf '%s\n' "$OPENSCP_SPARKLE_PRIVATE_KEY" | \
  "$sdk/bin/sign_update" --verify --ed-key-file - "$working/appcast.xml"
unset OPENSCP_SPARKLE_PRIVATE_KEY
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
python3 "$script_dir/verify-sparkle-appcast.py" \
  --archive "$archive" --appcast "$working/appcast.xml" --version "$version"
mkdir -p "$(dirname "$output")"
cp "$working/appcast.xml" "$output"
