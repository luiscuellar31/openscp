#!/usr/bin/env bash

set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  ./scripts/release/bump-version.sh <version>

Arguments:
  <version>    Numeric version in MAJOR.MINOR.PATCH format
EOF
}

die() {
  echo "[bump-version] $*" >&2
  exit 1
}

[[ "${1:-}" == "-h" || "${1:-}" == "--help" ]] && {
  usage
  exit 0
}

[[ $# -eq 1 ]] || {
  usage
  die "Missing required <version> argument"
}

VERSION="$1"
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] ||
  die "Invalid version format '${VERSION}'. Expected x.x.x"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

cmake_file="${REPO_ROOT}/CMakeLists.txt"
snapcraft_file="${REPO_ROOT}/packaging/snap/snapcraft.yaml"

for required in \
  "$cmake_file" \
  "$snapcraft_file"; do
  [[ -f "$required" ]] || die "Required file not found: $required"
done

current_cmake_version="$(
  sed -n \
    's/^project(.*VERSION[[:space:]]*\([0-9][0-9.]*\).*/\1/p' \
    "$cmake_file" | head -n 1
)"
current_snap_version="$(
  sed -n \
    's/^version:[[:space:]]*"\([0-9][0-9.]*\)"[[:space:]]*$/\1/p' \
    "$snapcraft_file" | head -n 1
)"

[[ -n "$current_cmake_version" ]] ||
  die "Could not read the version from CMakeLists.txt"
[[ -n "$current_snap_version" ]] ||
  die "Could not read the version from snapcraft.yaml"
[[ "$current_cmake_version" == "$current_snap_version" ]] ||
  die "Version sources disagree: CMake=${current_cmake_version}, Snap=${current_snap_version}"

perl -i -pe \
  's#^(project\(OpenSCP LANGUAGES CXX VERSION )\d+\.\d+\.\d+(\))$#${1}'"$VERSION"'${2}#m' \
  "$cmake_file"

perl -i -pe \
  's#^(version:\s*")\d+\.\d+\.\d+(")$#${1}'"$VERSION"'${2}#m' \
  "$snapcraft_file"

grep -Fq "project(OpenSCP LANGUAGES CXX VERSION ${VERSION})" "$cmake_file" ||
  die "CMakeLists.txt verification failed"
grep -Fq "version: \"${VERSION}\"" "$snapcraft_file" ||
  die "snapcraft.yaml verification failed"

echo "[bump-version] Updated version to ${VERSION}"
echo "[bump-version] Files updated:"
echo "  - ${cmake_file}"
echo "  - ${snapcraft_file}"
