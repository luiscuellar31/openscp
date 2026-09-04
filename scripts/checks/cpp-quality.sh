#!/usr/bin/env bash

set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUN_FORMAT=0
RUN_TIDY=0
RUN_CPPCHECK=0
BUILD_DIR="${REPO_DIR}/build-analysis"

usage() {
    printf '%s\n' \
        "Usage: ./scripts/checks/cpp-quality.sh [options]" \
        "" \
        "Options:" \
        "  --format          Check all first-party C++ files" \
        "  --tidy            Run clang-tidy using a compilation database" \
        "  --cppcheck        Run cppcheck" \
        "  --build-dir <p>   Compilation database directory" \
        "  -h, --help        Show this help" \
        "" \
        "With no check options, all three checks run."
}

while [[ $# -gt 0 ]]; do
    case "$1" in
    --format)
        RUN_FORMAT=1
        shift
        ;;
    --tidy)
        RUN_TIDY=1
        shift
        ;;
    --cppcheck)
        RUN_CPPCHECK=1
        shift
        ;;
    --build-dir)
        [[ $# -ge 2 ]] || { printf '%s\n' "--build-dir requires a value" >&2; exit 2; }
        BUILD_DIR="$2"
        shift 2
        ;;
    -h|--help)
        usage
        exit 0
        ;;
    *)
        printf 'Unknown option: %s\n' "$1" >&2
        usage >&2
        exit 2
        ;;
    esac
done

if [[ "$RUN_FORMAT" -eq 0 && "$RUN_TIDY" -eq 0 && "$RUN_CPPCHECK" -eq 0 ]]; then
    RUN_FORMAT=1
    RUN_TIDY=1
    RUN_CPPCHECK=1
fi

cd "$REPO_DIR"

cpp_files=()
while IFS= read -r source_file; do
    case "$source_file" in
    *.cpp|*.cc|*.cxx)
        cpp_files+=("$source_file")
        ;;
    *.h|*.hh|*.hpp)
        cpp_files+=("$source_file")
        ;;
    esac
done < <(
    find core ui tests -type f \
        \( -name '*.cpp' -o -name '*.cc' -o -name '*.cxx' \
        -o -name '*.h' -o -name '*.hh' -o -name '*.hpp' \) \
        -print | LC_ALL=C sort
)

[[ "${#cpp_files[@]}" -gt 0 ]] || {
    printf '%s\n' "No first-party C++ files were found." >&2
    exit 1
}

uncatalogued_settings_keys="$(
    rg -n --pcre2 \
        '"(?:UI|Advanced|Transfer|Security|Network|Protocol|Terminal|Sites|SyncDialog|Shortcuts|History|Favorites)/[A-Za-z0-9_/%.-]+"' \
        ui --glob '*.{cpp,h,hpp}' --glob '!AppSettings.hpp' || true
)"
if [[ -n "$uncatalogued_settings_keys" ]]; then
    printf '%s\n' \
        "Fixed QSettings paths must be declared in ui/logic/common/AppSettings.hpp:" \
        "$uncatalogued_settings_keys" >&2
    exit 1
fi

find_llvm17_tool() {
    local tool_name="$1"
    local candidate=""
    for candidate in \
        "${tool_name}-17" \
        "/opt/homebrew/opt/llvm@17/bin/${tool_name}" \
        "$tool_name" \
        "/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/${tool_name}"; do
        if command -v "$candidate" >/dev/null 2>&1; then
            command -v "$candidate"
            return 0
        fi
        if [[ -x "$candidate" ]]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    return 1
}

require_llvm17() {
    local tool_path="$1"
    local version_line=""
    version_line="$($tool_path --version | sed -n '1p')"
    if [[ ! "$version_line" =~ (^|[^0-9])17\.[0-9] ]]; then
        printf 'Expected LLVM 17 for %s, got: %s\n' "$tool_path" "$version_line" >&2
        exit 1
    fi
}

if [[ "$RUN_FORMAT" -eq 1 ]]; then
    format_bin="${CLANG_FORMAT_BIN:-$(find_llvm17_tool clang-format || true)}"
    [[ -n "$format_bin" ]] || { printf '%s\n' "clang-format 17 was not found." >&2; exit 1; }
    require_llvm17 "$format_bin"
    "$format_bin" --dry-run --Werror "${cpp_files[@]}"
fi

if [[ "$RUN_TIDY" -eq 1 ]]; then
    tidy_bin="${CLANG_TIDY_BIN:-$(find_llvm17_tool clang-tidy || true)}"
    [[ -n "$tidy_bin" ]] || { printf '%s\n' "clang-tidy 17 was not found." >&2; exit 1; }
    require_llvm17 "$tidy_bin"
    run_tidy_bin="${RUN_CLANG_TIDY_BIN:-}"
    if [[ -z "$run_tidy_bin" ]]; then
        for candidate in \
            run-clang-tidy-17 \
            /opt/homebrew/opt/llvm@17/bin/run-clang-tidy \
            run-clang-tidy; do
            if command -v "$candidate" >/dev/null 2>&1; then
                run_tidy_bin="$(command -v "$candidate")"
                break
            fi
            if [[ -x "$candidate" ]]; then
                run_tidy_bin="$candidate"
                break
            fi
        done
    fi
    [[ -n "$run_tidy_bin" ]] || { printf '%s\n' "run-clang-tidy 17 was not found." >&2; exit 1; }
    [[ -f "$BUILD_DIR/compile_commands.json" ]] || {
        printf 'Compilation database not found: %s/compile_commands.json\n' "$BUILD_DIR" >&2
        exit 1
    }
    tidy_extra_args=()
    if [[ "$(uname -s)" == "Darwin" ]]; then
        sdk_path="$(xcrun --show-sdk-path)"
        sdk_cxx_headers="${sdk_path}/usr/include/c++/v1"
        apple_clang_resource_dir="$(xcrun clang -print-resource-dir)"
        apple_clang_builtin_headers="${apple_clang_resource_dir}/include"
        [[ -d "$sdk_cxx_headers" ]] || {
            printf 'Apple libc++ headers not found: %s\n' "$sdk_cxx_headers" >&2
            exit 1
        }
        [[ -d "$apple_clang_resource_dir" ]] || {
            printf 'Apple Clang resource directory not found: %s\n' \
                "$apple_clang_resource_dir" >&2
            exit 1
        }
        [[ -d "$apple_clang_builtin_headers" ]] || {
            printf 'Apple Clang builtin headers not found: %s\n' \
                "$apple_clang_builtin_headers" >&2
            exit 1
        }
        # Homebrew LLVM 17 ships a libc++ without std::jthread. clang-tidy
        # must parse with the same Apple libc++ and builtin headers used by the
        # application build.
        tidy_extra_args+=(
            "-extra-arg-before=-nostdinc++"
            "-extra-arg-before=-isystem"
            "-extra-arg-before=${sdk_cxx_headers}"
            "-extra-arg-before=-isystem"
            "-extra-arg-before=${apple_clang_builtin_headers}"
            "-extra-arg=-resource-dir=${apple_clang_resource_dir}"
            "-extra-arg=-isysroot"
            "-extra-arg=${sdk_path}"
        )
    fi
    source_root_regex="$(printf '%s\n' "$REPO_DIR" | sed 's#[.[\*^$()+?{|\\/]#\\&#g')"
    "$run_tidy_bin" \
        -clang-tidy-binary "$tidy_bin" \
        -p "$BUILD_DIR" \
        -j "${TIDY_JOBS:-2}" \
        -quiet \
        -header-filter='(^|/)(core|ui|tests)/' \
        "${tidy_extra_args[@]}" \
        "^${source_root_regex}/(core/src|ui|tests)/.*\\.(cpp|cc|cxx)$"
fi

if [[ "$RUN_CPPCHECK" -eq 1 ]]; then
    command -v cppcheck >/dev/null 2>&1 || { printf '%s\n' "cppcheck was not found." >&2; exit 1; }
    cppcheck \
        --std=c++20 \
        --enable=warning,performance,portability \
        --error-exitcode=1 \
        --inline-suppr \
        --suppress=missingIncludeSystem \
        --suppress=unknownMacro \
        --quiet \
        core/include core/src ui tests
fi
