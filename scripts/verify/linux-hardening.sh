#!/usr/bin/env bash
set -euo pipefail

usage() {
    printf '%s\n' "Usage: ./scripts/verify/linux-hardening.sh <ELF-executable>"
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
    exit 0
fi

if [[ $# -ne 1 || ! -f "$1" ]]; then
    usage >&2
    exit 2
fi

if ! command -v readelf >/dev/null 2>&1; then
    echo "readelf is required to verify ELF hardening" >&2
    exit 2
fi

binary="$1"
elf_header="$(readelf -hW "$binary")"
program_headers="$(readelf -lW "$binary")"
dynamic_section="$(readelf -dW "$binary")"

# grep, not rg: the AppImage builds in a Rocky Linux image without ripgrep.
if ! grep -Eq 'Type:[[:space:]]+DYN' <<<"$elf_header"; then
    echo "hardening check failed: executable is not PIE" >&2
    exit 1
fi

if ! grep -Eq 'GNU_RELRO' <<<"$program_headers"; then
    echo "hardening check failed: GNU_RELRO is missing" >&2
    exit 1
fi

if ! grep -Eq 'BIND_NOW|FLAGS.*NOW' <<<"$dynamic_section"; then
    echo "hardening check failed: immediate symbol binding is missing" >&2
    exit 1
fi

# A missing GNU_STACK makes grep fail, which must reach the message below
# instead of ending the script through set -e.
stack_header="$(grep -E 'GNU_STACK' <<<"$program_headers" || true)"
if [[ -z "$stack_header" ]] || grep -Eq 'GNU_STACK.*RWE' <<<"$stack_header"; then
    echo "hardening check failed: stack is executable or unspecified" >&2
    exit 1
fi

echo "ELF hardening verified: PIE, RELRO, NOW, non-executable stack"
