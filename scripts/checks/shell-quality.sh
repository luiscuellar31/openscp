#!/usr/bin/env bash

set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

usage() {
    cat <<'EOF'
Usage: ./scripts/checks/shell-quality.sh

Checks every repository shell script with bash syntax validation, exercises
the help path of user-facing commands, and runs ShellCheck error diagnostics
when ShellCheck is installed.
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
    exit 0
fi
[[ $# -eq 0 ]] || { usage >&2; exit 2; }

shell_files=()
while IFS= read -r shell_file; do
    shell_files+=("$shell_file")
done < <(find "$REPO_DIR/scripts" -type f -name '*.sh' | LC_ALL=C sort)

[[ ${#shell_files[@]} -gt 0 ]] || {
    printf '%s\n' "No shell scripts were found." >&2
    exit 1
}

for shell_file in "${shell_files[@]}"; do
    bash -n "$shell_file"
done

check_help() {
    "$@" >/dev/null
}

check_help "$REPO_DIR/scripts/macos.sh" help
check_help "$REPO_DIR/scripts/linux.sh" help
check_help "$REPO_DIR/scripts/check_ci_local.sh" --help
check_help "$REPO_DIR/scripts/checks/cpp-quality.sh" --help
check_help "$REPO_DIR/scripts/package/appimage.sh" --help
check_help "$REPO_DIR/scripts/package/flatpak.sh" --help
check_help "$REPO_DIR/scripts/package/macos.sh" --help
check_help "$REPO_DIR/scripts/package/snap.sh" --help
check_help "$REPO_DIR/scripts/release/bump-version.sh" --help
check_help "$REPO_DIR/scripts/release/release-notes.sh" --help
check_help "$REPO_DIR/scripts/verify/linux-abi.sh" --help
check_help "$REPO_DIR/scripts/verify/linux-hardening.sh" --help
check_help "$REPO_DIR/scripts/verify/macos-bundle.sh" --help
check_help "$REPO_DIR/scripts/verify/qt-svg-plugins.sh" --help

if command -v shellcheck >/dev/null 2>&1; then
    shellcheck --severity=error "${shell_files[@]}"
else
    printf '%s\n' "ShellCheck not found; skipped ShellCheck diagnostics."
fi

printf 'Validated %d shell scripts.\n' "${#shell_files[@]}"
