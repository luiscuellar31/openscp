#!/usr/bin/env bash

set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  scripts/bump_version.sh <version>

Arguments:
  <version>    Version in x.x.x format (for example: 1.0.0)
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
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

cmake_file="${REPO_ROOT}/CMakeLists.txt"
readme_en="${REPO_ROOT}/README.md"
readme_es="${REPO_ROOT}/README_ES.md"
snapcraft_file="${REPO_ROOT}/packaging/snap/snapcraft.yaml"
release_notes_script="${REPO_ROOT}/scripts/generate_release_notes.sh"
plist_template="${REPO_ROOT}/assets/macos/Info.plist.in"

for required in \
  "$cmake_file" \
  "$readme_en" \
  "$readme_es" \
  "$snapcraft_file" \
  "$release_notes_script" \
  "$plist_template"; do
  [[ -f "$required" ]] || die "Required file not found: $required"
done

perl -i -pe \
  's#^(project\(OpenSCP_hello LANGUAGES CXX VERSION )\d+\.\d+\.\d+(\))$#${1}'"$VERSION"'${2}#m' \
  "$cmake_file"

perl -i -pe \
  's@^## What OpenSCP Offers \(v\d+\.\d+\.\d+\)$@## What OpenSCP Offers (v'"$VERSION"')@m' \
  "$readme_en"

perl -i -pe \
  's@^## Lo que Ofrece OpenSCP \(v\d+\.\d+\.\d+\)$@## Lo que Ofrece OpenSCP (v'"$VERSION"')@m' \
  "$readme_es"

perl -i -pe \
  's#^(version:\s*")\d+\.\d+\.\d+(")$#${1}'"$VERSION"'${2}#m' \
  "$snapcraft_file"

perl -i -pe \
  's#(e\.g\.\s+v)\d+\.\d+\.\d+#${1}'"$VERSION"'#g' \
  "$release_notes_script"

perl -i -pe \
  's#(@VERSION_SHORT@\s+e\.g\.\s+)\d+\.\d+\.\d+#${1}'"$VERSION"'#g; s#(@VERSION@\s+e\.g\.\s+)\d+\.\d+\.\d+#${1}'"$VERSION"'#g' \
  "$plist_template"

grep -q "project(OpenSCP_hello LANGUAGES CXX VERSION ${VERSION})" "$cmake_file" ||
  die "CMakeLists.txt verification failed"
grep -q "## What OpenSCP Offers (v${VERSION})" "$readme_en" ||
  die "README.md verification failed"
grep -q "## Lo que Ofrece OpenSCP (v${VERSION})" "$readme_es" ||
  die "README_ES.md verification failed"
grep -q "version: \"${VERSION}\"" "$snapcraft_file" ||
  die "snapcraft.yaml verification failed"
grep -q "e.g. v${VERSION}" "$release_notes_script" ||
  die "generate_release_notes.sh verification failed"
grep -q "@VERSION_SHORT@           e.g. ${VERSION}" "$plist_template" ||
  die "Info.plist.in VERSION_SHORT verification failed"
grep -q "@VERSION@                 e.g. ${VERSION}" "$plist_template" ||
  die "Info.plist.in VERSION verification failed"

echo "[bump-version] Updated version to ${VERSION}"
echo "[bump-version] Files updated:"
echo "  - ${cmake_file}"
echo "  - ${readme_en}"
echo "  - ${readme_es}"
echo "  - ${snapcraft_file}"
echo "  - ${release_notes_script}"
echo "  - ${plist_template}"
