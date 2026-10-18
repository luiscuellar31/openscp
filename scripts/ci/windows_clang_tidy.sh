#!/usr/bin/env bash

set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUN_CLANG_TIDY_URL="https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-17.0.1/clang-tools-extra/clang-tidy/tool/run-clang-tidy.py"
RUN_CLANG_TIDY_SHA256="556c708402e6c5c3a1788647323ff1e4160297ef24d6737cc044a79003f0eafc"

usage() {
    cat <<'EOF'
Usage: ./scripts/ci/windows_clang_tidy.sh [build directory]

Runs scripts/checks/cpp-quality.sh --tidy on the Windows code of a configured
Windows build (default: build), as the Windows CI job does, with the pinned
clang-tidy wheel of scripts/ci/clang-tidy-requirements.txt and LLVM 17.0.1's
run-clang-tidy.py. It runs in Git Bash, needs Python on PATH, and puts its
tools in clang-tidy under $RUNNER_TEMP, or under $TEMP outside GitHub Actions.
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
    exit 0
fi
[[ $# -le 1 ]] || { usage >&2; exit 2; }
BUILD_DIR="${1:-build}"
WORK_DIR="$(cygpath -m "${RUNNER_TEMP:-${TEMP:?Set RUNNER_TEMP or TEMP to a folder for the tools.}}")/clang-tidy"

mkdir -p "$WORK_DIR"
python -m venv "$WORK_DIR/venv"
"$WORK_DIR/venv/Scripts/python.exe" -m pip install --disable-pip-version-check --quiet \
    --require-hashes --only-binary=:all: \
    -r "$REPO_DIR/scripts/ci/clang-tidy-requirements.txt"
curl --fail --location --proto '=https' --silent --show-error \
    --output "$WORK_DIR/run-clang-tidy.py" "$RUN_CLANG_TIDY_URL"
printf '%s  %s\n' "$RUN_CLANG_TIDY_SHA256" "$WORK_DIR/run-clang-tidy.py" |
    sha256sum --check --quiet

cd "$REPO_DIR"
CLANG_TIDY_BIN="$WORK_DIR/venv/Lib/site-packages/clang_tidy/data/bin/clang-tidy.exe" \
    RUN_CLANG_TIDY_BIN="$WORK_DIR/run-clang-tidy.py" \
    TIDY_JOBS="${TIDY_JOBS:-${NUMBER_OF_PROCESSORS:-2}}" \
    ./scripts/checks/cpp-quality.sh --tidy --build-dir "$BUILD_DIR"
