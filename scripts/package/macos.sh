#!/usr/bin/env bash

set -euo pipefail

# OpenSCP macOS packaging script
# - Assumes OpenSCP.app is already produced by CMake (MACOSX_BUNDLE)
# - Runs macdeployqt to bundle Qt frameworks/plugins
# - Bundles non-Qt deps (libssh2, OpenSSL, tinyxml2)
# - Code-signs (hardened runtime) recursively
# - Creates a compressed .dmg
# - Notarizes with notarytool and staples the ticket
#
# Configuration via environment variables (local-only usage):
#   APP_NAME               Default: "OpenSCP"
#   BUNDLE_ID              Default: "com.openscp.app"
#   MINIMUM_SYSTEM_VERSION Default: "12.0" (CMake deployment target + Info.plist)
#   CMAKE_OSX_ARCHITECTURES Default: current machine architecture
#                            (or "arm64;x86_64" for universal naming)
#   CMAKE_PREFIX_PATH      Path to your Qt 6 install root (if not in default search path)
#   OPENSCP_DEPENDENCY_PREFIX
#                          Optional prefix containing release-built libssh2,
#                          OpenSSL, and tinyxml2 libraries.
#   QT_PREFIX              Path to Qt install root (…/Qt/<version>/macos)
#   Qt6_DIR                Path to Qt6 CMake config dir (…/lib/cmake/Qt6); used to derive Qt bin for macdeployqt
#   OPENSCP_ENFORCE_RECOMMENDED_QT_VERSION
#                          Set to ON for official artifacts; local/community
#                          packages remain free to use a compatible Qt 6.
#   PACKAGE_FORMATS        Comma-separated outputs: app,pkg,dmg (default: dmg)
#   MACDEPLOYQT_DISABLE_PLUGIN_SCAN
#                          Set to 1 to pass -no-plugins to macdeployqt.
#                          Default: 0 (recommended). When disabled, the script
#                          still stages required plugin families manually.
#   PRUNE_OPTIONAL_QT_PLUGINS
#                          Set to 1 (default) to remove optional Qt plugins
#                          that OpenSCP does not use but may pull unresolved
#                          framework deps on some Qt builds (for example,
#                          libqtvirtualkeyboardplugin.dylib -> QtVirtualKeyboardQml).
#                          Set to 0 to keep all scanned plugins.
#
# Signing / notarization env vars:
#   APPLE_IDENTITY         Required for signing, e.g. "Developer ID Application: Your Name (TEAMID)"
#   APPLE_TEAM_ID          Your Apple Team ID (e.g. ABCDE12345)
#   ENTITLEMENTS_FILE      Default: assets/macos/entitlements.plist
#   SKIP_CODESIGN          Set to 1 to skip Developer ID signing (debug/local)
#   DO_ADHOC_SIGN          When SKIP_CODESIGN=1, do ad‑hoc signing with `codesign -s -` to avoid
#                          “Code Signature Invalid” at runtime (defaults to 1).
#
#   For notarization (API key method; local secrets on your machine):
#   APPLE_API_KEY_ID       e.g. ABCDEFGHIJ
#   APPLE_API_ISSUER_ID    e.g. 00000000-0000-0000-0000-000000000000
#   APPLE_API_KEY_P8       Contents of AuthKey_<KEYID>.p8 (as a secret)
#   SKIP_NOTARIZATION      Set to 1 to skip notarization
#
# Output (based on PACKAGE_FORMATS):
#   app -> dist/<APP_NAME>-<VERSION>-<ARCH>-UNSIGNED.zip
#   pkg -> dist/<APP_NAME>-<VERSION>-<ARCH>-UNSIGNED.pkg
#   dmg -> dist/<APP_NAME>-<VERSION>-<ARCH>-UNSIGNED.dmg
#   (hash .sha256 alongside each artifact)
#     where <ARCH> is arm64, x86_64, or arm64+x86_64

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${REPO_DIR}/build"
DIST_DIR="${REPO_DIR}/dist"

APP_NAME="${APP_NAME:-OpenSCP}"
BUNDLE_ID="${BUNDLE_ID:-com.openscp.app}"
MINIMUM_SYSTEM_VERSION="${MINIMUM_SYSTEM_VERSION:-12.0}"
ARCHS="${CMAKE_OSX_ARCHITECTURES:-$(uname -m)}"
PACKAGE_FORMATS="${PACKAGE_FORMATS:-dmg}"
ENTITLEMENTS_FILE="${ENTITLEMENTS_FILE:-${REPO_DIR}/assets/macos/entitlements.plist}"

APP_DIR="${BUILD_DIR}/${APP_NAME}.app"
CONTENTS_DIR="${APP_DIR}/Contents"
MACOS_DIR="${CONTENTS_DIR}/MacOS"
RESOURCES_DIR="${CONTENTS_DIR}/Resources"
FRAMEWORKS_DIR="${CONTENTS_DIR}/Frameworks"
PLUGINS_DIR="${CONTENTS_DIR}/PlugIns"

ICON_BASENAME="${APP_NAME}"
ICON_ICNS_PATH="${RESOURCES_DIR}/${ICON_BASENAME}.icns"
QT_CONF_SRC="${REPO_DIR}/assets/macos/qt.conf"
INFO_PLIST_OUT="${CONTENTS_DIR}/Info.plist"

# Will be set when discovering Qt/macdeployqt to help locate frameworks
QTPREFIX=""
MACDEPLOYQT_PATH=""
QT_HOST_WRAP_DIR=""

source "${REPO_DIR}/scripts/lib/macos/signing.sh"
source "${REPO_DIR}/scripts/lib/macos/artifacts.sh"

# Helpers
log() { printf "\033[1;34m[pack]\033[0m %s\n" "$*"; }
warn() { printf "\033[1;33m[warn]\033[0m %s\n" "$*"; }
err() { printf "\033[1;31m[err ]\033[0m %s\n" "$*"; }
die() { err "$*"; exit 1; }

usage() {
  cat <<'EOF'
Usage: ./scripts/package/macos.sh

Builds one or more macOS artifacts. Select formats with PACKAGE_FORMATS
(app,pkg,dmg) and configure signing, notarization, Qt, and architectures with
the environment variables documented at the top of this script and in
docs/BUILDING.md.
EOF
}

source "${REPO_DIR}/scripts/lib/macos/qt-host-tools.sh"

ensure_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "Missing required tool: $1"
}


discover_macdeployqt() {
  # 1) QT_PREFIX (explicit) lookup
  if [[ -n "${QT_PREFIX:-}" ]]; then
    local from_prefix="${QT_PREFIX}/bin/macdeployqt"
    if [[ -x "$from_prefix" ]]; then
      QTPREFIX="$(cd "$(dirname "$from_prefix")/.." && pwd)"
      MACDEPLOYQT_PATH="$from_prefix"; return
    fi
  fi
  # 2) Qt6_DIR based lookup
  if [[ -n "${Qt6_DIR:-}" ]]; then
    local cand="${Qt6_DIR}/../../../bin/macdeployqt"
    if [[ -x "$cand" ]]; then
      # Avoid conda/macdeployqt
      local real
      real="$(/usr/bin/realpath "$cand" 2>/dev/null || echo "$cand")"
      if [[ "$real" == *miniconda* ]]; then
        die "Refusing to use conda macdeployqt at: $real. Point Qt6_DIR to your official Qt (e.g., \$HOME/Qt/<version>/macos/lib/cmake/Qt6)."
      fi
      QTPREFIX="$(cd "$(dirname "$cand")/.." && pwd)"
      MACDEPLOYQT_PATH="$cand"; return
    fi
  fi
  # 3) Auto-detect from $HOME/Qt/<version>/macos
  local home_prefix
  home_prefix="$(detect_qt_prefix_from_home || true)"
  if [[ -n "$home_prefix" && -x "${home_prefix}/bin/macdeployqt" ]]; then
    QTPREFIX="$home_prefix"
    MACDEPLOYQT_PATH="${home_prefix}/bin/macdeployqt"; return
  fi
  # 4) PATH fallback (reject miniconda)
  if command -v macdeployqt >/dev/null 2>&1; then
    local pathbin
    pathbin="$(command -v macdeployqt)"
    local real
    real="$(/usr/bin/realpath "$pathbin" 2>/dev/null || echo "$pathbin")"
    if [[ "$real" == *miniconda* ]]; then
      die "Found macdeployqt in conda path: $real. Please use official Qt macdeployqt or set Qt6_DIR to your Qt installation."
    fi
    QTPREFIX="$(cd "$(dirname "$pathbin")/.." && pwd)"
    MACDEPLOYQT_PATH="$pathbin"; return
  fi
  die "macdeployqt not found. Install Qt 6 and ensure macdeployqt is available (or set QT_PREFIX/Qt6_DIR)."
}

year() { date +%Y; }

version_key() {
  local v="${1:-0}"
  local a=0 b=0 c=0 d=0
  IFS='.' read -r a b c d <<<"$v"
  printf "%04d%04d%04d%04d" "${a:-0}" "${b:-0}" "${c:-0}" "${d:-0}"
}

detect_qt6_dir_from_home() {
  local best_dir=""
  local best_key=""
  local cand ver key
  shopt -s nullglob
  for cand in "${HOME}"/Qt/*/macos/lib/cmake/Qt6; do
    [[ -f "${cand}/Qt6Config.cmake" ]] || continue
    ver="$(sed -E 's#^.*/Qt/([^/]+)/macos/lib/cmake/Qt6$#\1#' <<<"$cand")"
    [[ "$ver" =~ ^[0-9]+(\.[0-9]+)*$ ]] || continue
    key="$(version_key "$ver")"
    if [[ -z "$best_key" || "$key" > "$best_key" ]]; then
      best_key="$key"
      best_dir="$cand"
    fi
  done
  shopt -u nullglob
  [[ -n "$best_dir" ]] && printf "%s\n" "$best_dir"
}

detect_qt_prefix_from_home() {
  local qt6_dir
  qt6_dir="$(detect_qt6_dir_from_home || true)"
  if [[ -n "$qt6_dir" ]]; then
    (cd "$qt6_dir/../../.." && pwd)
  fi
}

detect_version() {
  # Try to extract from top-level CMakeLists.txt: project(... VERSION x.y.z)
  local ver
  ver=$(
    awk '
      BEGIN { in_project=0; buf="" }
      {
        if (!in_project && $0 ~ /^[[:space:]]*project[[:space:]]*\(/) {
          in_project=1;
          buf=$0;
        } else if (in_project) {
          buf=buf " " $0;
        }
        if (in_project && index($0, ")") > 0) {
          if (buf ~ /VERSION[[:space:]]*[0-9]+(\.[0-9]+)*/) {
            sub(/^.*VERSION[[:space:]]*/, "", buf);
            sub(/[^0-9.].*$/, "", buf);
            print buf;
            exit;
          }
          in_project=0;
          buf="";
        }
      }
    ' "${REPO_DIR}/CMakeLists.txt" | head -n1 || true
  )
  if [[ -z "$ver" ]]; then ver="0.0.0"; fi
  echo "$ver"
}

detect_bundle_version() {
  local plist="$1"
  [[ -f "$plist" ]] || return 0
  /usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$plist" 2>/dev/null || true
}

join_archs() {
  local s="$1"; echo "${s//;/+}"
}

normalize_formats() {
  # "app,pkg dmg" -> "app,pkg,dmg"
  local s="${1// /,}"
  while [[ "$s" == *",,"* ]]; do s="${s//,,/,}"; done
  s="${s#,}"
  s="${s%,}"
  echo "$s"
}

has_format() {
  local needle="$1"
  local haystack
  haystack="$(normalize_formats "${PACKAGE_FORMATS}")"
  [[ ",${haystack}," == *",${needle},"* ]]
}

write_sha256() {
  local artifact="$1"
  if command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$artifact" | awk '{print $1}' > "${artifact}.sha256"
  elif command -v openssl >/dev/null 2>&1; then
    openssl dgst -sha256 -r "$artifact" | awk '{print $1}' > "${artifact}.sha256"
  fi
}

generate_icns_from_png() {
  local src_png="$1"; local dst_icns="$2"
  ensure_cmd sips; ensure_cmd iconutil
  local tmp_iconset
  tmp_iconset="$(mktemp -d)"/icon.iconset
  mkdir -p "$tmp_iconset"
  # Required sizes for macOS iconset
  local sizes=(16 32 64 128 256 512)
  for sz in "${sizes[@]}"; do
    sips -z "$sz" "$sz" "$src_png" --out "$tmp_iconset/icon_${sz}x${sz}.png" >/dev/null
    local dbl=$((sz*2))
    sips -z "$dbl" "$dbl" "$src_png" --out "$tmp_iconset/icon_${sz}x${sz}@2x.png" >/dev/null
  done
  iconutil -c icns "$tmp_iconset" -o "$dst_icns"
  rm -rf "$(dirname "$tmp_iconset")"
}

list_deps() {
  local bin="$1"
  otool -L "$bin" | awk '
    /^[[:space:]]/ && NF > 0 {
      if (!seen[$1]++) print $1
    }
  '
}

list_binary_rpaths() {
  local bin="$1"
  otool -l "$bin" | awk '
    $1 == "cmd" && $2 == "LC_RPATH" { in_rpath=1; next }
    in_rpath && $1 == "path" { print $2; in_rpath=0 }
  '
}

sanitize_binary_rpaths() {
  local bin="$1"
  local required_rpath="$2"
  local existing_rpath=""

  while IFS= read -r existing_rpath; do
    [[ -n "$existing_rpath" ]] || continue
    case "$existing_rpath" in
      "$required_rpath"|@executable_path/*|@loader_path/*|@rpath/*)
        ;;
      /*)
        # Strip absolute rpaths so the bundle can be relocated.
        install_name_tool -delete_rpath "$existing_rpath" "$bin" >/dev/null 2>&1 || true
        ;;
      *)
        ;;
    esac
  done < <(list_binary_rpaths "$bin")

  if ! list_binary_rpaths "$bin" | grep -Fxq "$required_rpath"; then
    install_name_tool -add_rpath "$required_rpath" "$bin" || true
  fi
}

ensure_rpath() {
  local bin="$1"
  sanitize_binary_rpaths "$bin" "@executable_path/../Frameworks"
}

redirect_dep_to_rpath() {
  local bin="$1"; local old="$2"; local base
  base="$(basename "$old")"
  install_name_tool -change "$old" "@rpath/${base}" "$bin"
}

qt_framework_dep_to_bundle_rpath() {
  local dep="$1"
  # Example source dep:
  # /opt/homebrew/opt/qtbase/lib/QtGui.framework/Versions/A/QtGui
  # target:
  # @rpath/QtGui.framework/Versions/A/QtGui
  if [[ "$dep" =~ /((Qt[^/]+)\.framework)/Versions/[^/]+/(Qt[^/]+)$ ]]; then
    local fw_name="${BASH_REMATCH[2]}"
    local bin_name="${BASH_REMATCH[3]}"
    if [[ "$fw_name" == "$bin_name" ]]; then
      printf '@rpath/%s.framework/Versions/A/%s\n' "$fw_name" "$fw_name"
      return 0
    fi
  fi
  return 1
}

ensure_loader_framework_rpath() {
  local bin="$1"
  sanitize_binary_rpaths "$bin" "@loader_path/../../Frameworks"
}

rewrite_external_refs_to_bundle() {
  local bin="$1"
  [[ -f "$bin" ]] || return 0

  local dep=""
  while IFS= read -r dep; do
    [[ -n "$dep" ]] || continue
    case "$dep" in
      /opt/homebrew/*|/usr/local/*|/Users/runner/*|/private/tmp/*|/tmp/*|/var/folders/*)
        local mapped=""
        mapped="$(qt_framework_dep_to_bundle_rpath "$dep" || true)"
        if [[ -n "$mapped" ]]; then
          install_name_tool -change "$dep" "$mapped" "$bin" || true
          continue
        fi

        local base
        base="$(basename "$dep")"
        if [[ -e "$FRAMEWORKS_DIR/$base" ]]; then
          install_name_tool -change "$dep" "@rpath/$base" "$bin" || true
        fi
        ;;
      *)
        ;;
    esac
  done < <(list_deps "$bin")
}

bundle_non_qt_deps() {
  # Ensure non-Qt libraries (libssh2, OpenSSL libcrypto, tinyxml2) are present in Frameworks.
  # Newer macdeployqt may already copy them; in that case we skip copying and just ensure IDs/RPATH.
  mkdir -p "$FRAMEWORKS_DIR"

  local exe="$MACOS_DIR/${APP_NAME}"
  local dependency_prefix="${OPENSCP_DEPENDENCY_PREFIX:-}"
  if [[ -n "$dependency_prefix" && ! -d "$dependency_prefix/lib" ]]; then
    die "OPENSCP_DEPENDENCY_PREFIX has no lib directory: $dependency_prefix"
  fi
  local want_libssh2="" want_libcrypto="" want_tinyxml2=""
  if otool -L "$exe" | grep -q "libssh2"; then want_libssh2=1; fi
  if otool -L "$exe" | grep -q "libcrypto"; then want_libcrypto=1; fi
  if otool -L "$exe" | grep -q "tinyxml2"; then want_tinyxml2=1; fi

  # helper to maybe copy a dylib if missing
maybe_copy() {
  local src_path="$1"; local dest_dir="$2"; local dest_path
  dest_path="$dest_dir/$(basename "$src_path")"
  # Dereference symlinks to avoid Cellar paths inside the bundle
  if [[ -L "$src_path" ]]; then
    src_path="$(/usr/bin/readlink "$src_path" || echo "$src_path")"
  fi
  if [[ -e "$dest_path" && -L "$dest_path" ]]; then
    rm -f "$dest_path"
  fi
  if [[ ! -f "$dest_path" ]]; then
    cp -L "$src_path" "$dest_dir/"
  fi
  echo "$dest_path"
}

copy_from_dependency_prefix() {
  local pattern="$1"
  local src_path="" install_name="" dest_basename="" dest_path=""
  src_path=$(find "$dependency_prefix/lib" -maxdepth 1 -type f -name "$pattern" | sort | head -n1 || true)
  [[ -n "$src_path" ]] ||
    die "No library matching $pattern found under $dependency_prefix/lib"

  # Versioned source files commonly have an ABI install-name with a shorter
  # basename (for example libssh2.1.dylib). Preserve that ABI name so existing
  # executable references resolve to the newly copied release library.
  install_name=$(otool -D "$src_path" | awk 'NR == 2 { print $1; exit }')
  dest_basename=$(basename "${install_name:-$src_path}")
  dest_path="$FRAMEWORKS_DIR/$dest_basename"

  find "$FRAMEWORKS_DIR" -maxdepth 1 \( -type f -o -type l \) \
    -name "$pattern" -delete
  cp -L "$src_path" "$dest_path"
  printf '%s\n' "$dest_path"
}

  local libs_to_fix=()

  if [[ -n "$want_libssh2" ]]; then
    local existing
    if [[ -n "$dependency_prefix" ]]; then
      existing=$(copy_from_dependency_prefix 'libssh2*.dylib')
      libs_to_fix+=("$existing")
    else
      existing=$(ls "$FRAMEWORKS_DIR"/libssh2*.dylib 2>/dev/null | head -n1 || true)
      if [[ -n "$existing" ]]; then
        libs_to_fix+=("$existing")
      else
        local libdir="" src=""
        libdir=$(pkg-config --variable=libdir libssh2 2>/dev/null || true)
        if [[ -z "$libdir" ]]; then
          for d in /opt/homebrew/opt/libssh2/lib /opt/homebrew/lib /usr/local/opt/libssh2/lib /usr/local/lib; do
            [[ -d "$d" ]] && libdir="$d" && break
          done
        fi
        src=$(ls "$libdir"/libssh2*.dylib 2>/dev/null | head -n1 || true)
        [[ -z "$src" ]] && die "libssh2 dylib not found (looked under $libdir). Install with 'brew install libssh2'."
        existing=$(maybe_copy "$src" "$FRAMEWORKS_DIR")
        libs_to_fix+=("$existing")
        # Redirect the executable's reference if it still points to Homebrew path
        if otool -L "$exe" | grep -q "$src"; then
          redirect_dep_to_rpath "$exe" "$src"
        fi
      fi
    fi
    # Set ID to @rpath/<name>
    if [[ -f "$existing" ]]; then
      install_name_tool -id "@rpath/$(basename "$existing")" "$existing" || true
    fi
  fi

  if [[ -n "$want_libcrypto" ]]; then
    local existing
    if [[ -n "$dependency_prefix" ]]; then
      existing=$(copy_from_dependency_prefix 'libcrypto*.dylib')
      libs_to_fix+=("$existing")
    else
      existing=$(ls "$FRAMEWORKS_DIR"/libcrypto*.dylib 2>/dev/null | head -n1 || true)
      if [[ -n "$existing" ]]; then
        libs_to_fix+=("$existing")
      else
        local src=""
        if command -v brew >/dev/null 2>&1; then
          local pfx
          pfx=$(brew --prefix openssl@3 2>/dev/null || true)
          if [[ -n "$pfx" && -d "$pfx/lib" ]]; then src=$(ls "$pfx/lib"/libcrypto*.dylib 2>/dev/null | head -n1 || true); fi
        fi
        if [[ -z "$src" ]]; then
          for d in /opt/homebrew/opt/openssl@3/lib /usr/local/opt/openssl@3/lib; do
            [[ -d "$d" ]] && src=$(ls "$d"/libcrypto*.dylib 2>/dev/null | head -n1 || true)
            [[ -n "$src" ]] && break
          done
        fi
        [[ -z "$src" ]] && die "OpenSSL libcrypto dylib not found. Install with 'brew install openssl@3'."
        existing=$(maybe_copy "$src" "$FRAMEWORKS_DIR")
        libs_to_fix+=("$existing")
        if otool -L "$exe" | grep -q "$src"; then
          redirect_dep_to_rpath "$exe" "$src"
        fi
      fi
    fi
    if [[ -f "$existing" ]]; then
      install_name_tool -id "@rpath/$(basename "$existing")" "$existing" || true
    fi
  fi

  # macdeployqt may stage libssl next to libcrypto even when it is not a direct
  # executable dependency. If a release prefix is authoritative, replace that
  # copy as well so no newer Homebrew Mach-O remains in the bundle.
  if [[ -n "$dependency_prefix" ]] &&
     compgen -G "$FRAMEWORKS_DIR/libssl*.dylib" >/dev/null 2>&1; then
    local bundled_libssl
    bundled_libssl=$(copy_from_dependency_prefix 'libssl*.dylib')
    libs_to_fix+=("$bundled_libssl")
    install_name_tool -id "@rpath/$(basename "$bundled_libssl")" \
      "$bundled_libssl" || true
  fi

  if [[ -n "$want_tinyxml2" ]]; then
    local existing
    if [[ -n "$dependency_prefix" ]]; then
      existing=$(copy_from_dependency_prefix 'libtinyxml2*.dylib')
      libs_to_fix+=("$existing")
    else
      existing=$(ls "$FRAMEWORKS_DIR"/libtinyxml2*.dylib 2>/dev/null | head -n1 || true)
      if [[ -n "$existing" ]]; then
        libs_to_fix+=("$existing")
      else
        local libdir="" src=""
        libdir=$(pkg-config --variable=libdir tinyxml2 2>/dev/null || true)
        if [[ -z "$libdir" ]] && command -v brew >/dev/null 2>&1; then
          local pfx
          pfx=$(brew --prefix tinyxml2 2>/dev/null || true)
          if [[ -n "$pfx" && -d "$pfx/lib" ]]; then
            libdir="$pfx/lib"
          fi
        fi
        if [[ -z "$libdir" ]]; then
          for d in /opt/homebrew/opt/tinyxml2/lib /usr/local/opt/tinyxml2/lib /opt/homebrew/lib /usr/local/lib; do
            [[ -d "$d" ]] && libdir="$d" && break
          done
        fi
        src=$(ls "$libdir"/libtinyxml2*.dylib 2>/dev/null | head -n1 || true)
        [[ -z "$src" ]] && die "tinyxml2 dylib not found (looked under $libdir). Install with 'brew install tinyxml2'."
        existing=$(maybe_copy "$src" "$FRAMEWORKS_DIR")
        libs_to_fix+=("$existing")
        if otool -L "$exe" | grep -q "$src"; then
          redirect_dep_to_rpath "$exe" "$src"
        fi
      fi
    fi
    if [[ -f "$existing" ]]; then
      install_name_tool -id "@rpath/$(basename "$existing")" "$existing" || true
    fi
  fi

  # Ensure the executable has the standard rpath to find Frameworks
  ensure_rpath "$exe"

  # For any libraries we copied or detected, redirect their internal deps that point to Homebrew paths to @rpath
  for lib in "${libs_to_fix[@]}"; do
    [[ -f "$lib" ]] || continue
    while read -r line; do
      local dep
      dep=$(echo "$line" | awk '{print $1}')
      if [[ "$dep" == /opt/homebrew/* || "$dep" == /usr/local/* ]]; then
        local base; base=$(basename "$dep")
        if [[ -e "$FRAMEWORKS_DIR/$base" ]]; then
          install_name_tool -change "$dep" "@rpath/$base" "$lib" || true
        fi
      fi
    done < <(list_deps "$lib")
    rewrite_external_refs_to_bundle "$lib"
  done
  rewrite_external_refs_to_bundle "$exe"
}

# Copy a Qt *.framework from a source lib dir into the app bundle Frameworks
copy_qt_framework() {
  local src_root="$1"   # e.g., /Users/.../Qt/<version>/macos/lib
  local fw_name="$2"    # e.g., QtWidgets
  local src="$src_root/${fw_name}.framework"
  local dst="$FRAMEWORKS_DIR/${fw_name}.framework"
  [[ -d "$src" ]] || return 0
  rm -rf "$dst"
  # Use ditto if available to preserve framework structure; fallback to cp -R
  if command -v ditto >/dev/null 2>&1; then
    ditto "$src" "$dst"
  else
    cp -R "$src" "$dst"
  fi
  # Fix install name id to use @rpath
  local binpath
  if [[ -f "$dst/Versions/A/${fw_name}" ]]; then
    binpath="$dst/Versions/A/${fw_name}"
  elif [[ -f "$dst/${fw_name}" ]]; then
    binpath="$dst/${fw_name}"
  fi
  if [[ -n "$binpath" ]]; then
    install_name_tool -id "@rpath/${fw_name}.framework/Versions/A/${fw_name}" "$binpath" || true
  fi
}

# Ensure critical Qt runtime plugin families are present in the bundle.
# This acts as a safety net on top of what macdeployqt already deploys.
list_qt_plugins_roots() {
  local candidate=""
  local hbqt=""
  local seen=""

  emit_root_if_new() {
    local root="$1"
    [[ -n "$root" && -d "$root" ]] || return 0
    if [[ -n "$seen" ]] && printf '%s' "$seen" | grep -Fqx "$root"; then
      return 0
    fi
    seen="${seen}${root}"$'\n'
    printf '%s\n' "$root"
  }

  if [[ -n "$QTPREFIX" ]]; then
    for candidate in \
      "$QTPREFIX/plugins" \
      "$QTPREFIX/lib/qt/plugins" \
      "$QTPREFIX/lib/qt6/plugins" \
      "$QTPREFIX/share/qt/plugins" \
      "$QTPREFIX/share/qt6/plugins"
    do
      emit_root_if_new "$candidate"
    done
  fi

  if command -v qtpaths6 >/dev/null 2>&1; then
    candidate="$(qtpaths6 --query QT_INSTALL_PLUGINS 2>/dev/null || true)"
    emit_root_if_new "$candidate"
  fi
  if command -v qtpaths >/dev/null 2>&1; then
    candidate="$(qtpaths --query QT_INSTALL_PLUGINS 2>/dev/null || true)"
    emit_root_if_new "$candidate"
  fi
  if command -v qmake6 >/dev/null 2>&1; then
    candidate="$(qmake6 -query QT_INSTALL_PLUGINS 2>/dev/null || true)"
    emit_root_if_new "$candidate"
  fi
  if command -v qmake >/dev/null 2>&1; then
    candidate="$(qmake -query QT_INSTALL_PLUGINS 2>/dev/null || true)"
    emit_root_if_new "$candidate"
  fi

  if command -v brew >/dev/null 2>&1; then
    hbqt="$(brew --prefix qt 2>/dev/null || brew --prefix qt@6 2>/dev/null || true)"
    if [[ -n "$hbqt" ]]; then
      for candidate in \
        "$hbqt/plugins" \
        "$hbqt/lib/qt/plugins" \
        "$hbqt/lib/qt6/plugins" \
        "$hbqt/share/qt/plugins" \
        "$hbqt/share/qt6/plugins"
      do
        emit_root_if_new "$candidate"
      done
    fi
  fi
}

find_qt_plugins_root() {
  local candidate=""
  local first=""
  while IFS= read -r candidate; do
    [[ -n "$candidate" ]] || continue
    if [[ -z "$first" ]]; then
      first="$candidate"
    fi
  done < <(list_qt_plugins_roots)
  if [[ -n "$first" ]]; then
    echo "$first"
    return 0
  fi
  return 1
}

find_qt_plugins_root_for_subdir() {
  local subdir="$1"
  local sentinel="$2"
  local candidate=""
  local subpath=""
  local match=""
  local fallback=""
  while IFS= read -r candidate; do
    [[ -n "$candidate" ]] || continue
    subpath="${candidate}/${subdir}"
    [[ -d "$subpath" ]] || continue
    if [[ -f "$subpath/lib${sentinel}.dylib" || -f "$subpath/${sentinel}.dylib" ]]; then
      if [[ -z "$match" ]]; then
        match="$candidate"
      fi
      continue
    fi
    # Keep a fallback for plugin dirs that exist but use a different plugin name.
    if find "$subpath" -maxdepth 1 -type f -name '*.dylib' | grep -q .; then
      if [[ -z "$fallback" ]]; then
        fallback="$candidate"
      fi
    fi
  done < <(list_qt_plugins_roots)
  if [[ -n "$match" ]]; then
    echo "$match"
    return 0
  fi
  if [[ -n "$fallback" ]]; then
    echo "$fallback"
    return 0
  fi
  return 1
}

ensure_qt_plugin_subdir() {
  local subdir="$1"
  local sentinel="$2"
  local description="$3"
  local required="${4:-1}"
  local dest_dir="$PLUGINS_DIR/$subdir"
  if [[ -f "$dest_dir/lib${sentinel}.dylib" || -f "$dest_dir/${sentinel}.dylib" ]]; then
    return
  fi

  local plugins_root=""
  plugins_root="$(find_qt_plugins_root_for_subdir "$subdir" "$sentinel" || true)"
  if [[ -n "$plugins_root" && -d "$plugins_root/$subdir" ]]; then
    local src="$plugins_root/$subdir"
    local src_plugin=""
    if [[ -f "$src/lib${sentinel}.dylib" ]]; then
      src_plugin="$src/lib${sentinel}.dylib"
    elif [[ -f "$src/${sentinel}.dylib" ]]; then
      src_plugin="$src/${sentinel}.dylib"
    fi

    if [[ -z "$src_plugin" ]]; then
      if [[ "$required" == "1" ]]; then
        die "Required ${description} plugin '${sentinel}' was not found in: $src"
      fi
      warn "Optional ${description} plugin '${sentinel}' not found in: $src"
      return
    fi

    log "Ensuring ${description} plugin from: $src_plugin"
    mkdir -p "$dest_dir"
    # Copy only the required plugin to avoid bundling optional plugins that can
    # introduce undeployed third-party dependencies (for example, qjp2 -> jasper).
    cp -L "$src_plugin" "$dest_dir/"

    # Normalize plugin linkage to avoid leaking absolute Homebrew paths.
    local plugin_bin
    plugin_bin="$dest_dir/$(basename "$src_plugin")"
    rewrite_external_refs_to_bundle "$plugin_bin"
    ensure_loader_framework_rpath "$plugin_bin"

    if [[ ! -f "$dest_dir/lib${sentinel}.dylib" && ! -f "$dest_dir/${sentinel}.dylib" ]]; then
      if [[ "$required" == "1" ]]; then
        die "Failed to stage ${description} plugin '${sentinel}' from: ${src}"
      fi
      warn "Could not stage optional ${description} plugin '${sentinel}' from: ${src}"
    fi
  else
    warn "Could not locate ${description} plugins; ${sentinel} may be unavailable"
    local roots
    roots="$(list_qt_plugins_roots | tr '\n' ' ' | sed 's/[[:space:]]*$//')"
    [[ -n "$roots" ]] && warn "Searched Qt plugin roots: $roots"
    if [[ "$required" == "1" ]]; then
      die "Missing required ${description} plugin '${sentinel}' for macOS bundle"
    fi
  fi
}

ensure_qt_support_plugins() {
  ensure_qt_plugin_subdir "platforms" "qcocoa" "Qt platform"
  ensure_qt_plugin_subdir "imageformats" "qsvg" "Qt imageformats" 0
  ensure_qt_plugin_subdir "iconengines" "qsvgicon" "Qt iconengines"
  ensure_qt_plugin_subdir "styles" "qmacstyle" "Qt macOS style" 0
}

prune_optional_qt_plugins() {
  if [[ "${PRUNE_OPTIONAL_QT_PLUGINS:-1}" != "1" ]]; then
    return
  fi

  # OpenSCP does not use Qt Virtual Keyboard on desktop. Some Qt distributions
  # (notably certain Homebrew builds) can ship this plugin with dependencies
  # that are not deployed by macdeployqt, causing strict linkage verification
  # failures even though the plugin is never loaded by OpenSCP.
  local vk_plugin="${PLUGINS_DIR}/platforminputcontexts/libqtvirtualkeyboardplugin.dylib"
  if [[ -f "$vk_plugin" ]]; then
    warn "Pruning optional plugin: ${vk_plugin}"
    rm -f "$vk_plugin"
    local pic_dir="${PLUGINS_DIR}/platforminputcontexts"
    if [[ -d "$pic_dir" ]]; then
      if ! find "$pic_dir" -type f -name '*.dylib' | grep -q .; then
        rmdir "$pic_dir" 2>/dev/null || true
      fi
    fi
  fi
}

is_required_qt_plugin_binary() {
  local plugin="$1"
  case "$plugin" in
    */platforms/libqcocoa.dylib|*/platforms/qcocoa.dylib|*/iconengines/libqsvgicon.dylib|*/iconengines/qsvgicon.dylib)
      return 0
      ;;
    *)
      return 1
      ;;
  esac
}

sanitize_qt_plugins_linkage() {
  local forbidden_regex='^/(opt/homebrew|usr/local/(Cellar|opt)|Users/runner|private/tmp|tmp|var/folders|.*miniconda.*|.*anaconda.*)'
  local plugin=""
  while IFS= read -r plugin; do
    [[ -n "$plugin" ]] || continue

    rewrite_external_refs_to_bundle "$plugin"
    ensure_loader_framework_rpath "$plugin"

    local bad_refs=""
    bad_refs="$(list_deps "$plugin" | grep -E "$forbidden_regex" || true)"
    if [[ -n "$bad_refs" ]]; then
      if is_required_qt_plugin_binary "$plugin"; then
        err "Required Qt plugin has unresolved external refs: $plugin"
        printf '%s\n' "$bad_refs" >&2
        die "Cannot continue with unresolved required plugin dependencies"
      fi
      warn "Pruning optional plugin with unresolved external refs: $plugin"
      printf '%s\n' "$bad_refs" >&2
      rm -f "$plugin"
    fi
  done < <(find "$PLUGINS_DIR" -type f -name '*.dylib' | sort)

  # Best-effort cleanup for empty plugin directories after pruning.
  find "$PLUGINS_DIR" -type d -empty -delete 2>/dev/null || true
}

main() {
  if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
    return 0
  fi
  [[ $# -eq 0 ]] || die "Unexpected argument: $1"

  mkdir -p "$BUILD_DIR" "$DIST_DIR"

  # Auto-local defaults: if no Apple signing/notarization creds are present and
  # the caller didn't set SKIP_* explicitly, default to skipping them (local unsigned build).
  if [[ -z "${SKIP_CODESIGN:-}" && -z "${APPLE_IDENTITY:-}" ]]; then
    warn "APPLE_IDENTITY not set; defaulting SKIP_CODESIGN=1 (local/unsigned)"
    SKIP_CODESIGN=1
  fi
  if [[ -z "${SKIP_NOTARIZATION:-}" ]]; then
    missing_notar=()
    for v in APPLE_TEAM_ID APPLE_API_KEY_ID APPLE_API_ISSUER_ID APPLE_API_KEY_P8; do
      [[ -n "${!v:-}" ]] || missing_notar+=("$v")
    done
    if ((${#missing_notar[@]:-0})); then
      warn "Notarization credentials missing (${missing_notar[*]}). Defaulting SKIP_NOTARIZATION=1."
      SKIP_NOTARIZATION=1
    fi
    unset missing_notar
  fi

  # Determine architecture suffix and requested outputs.
  local version arch_suffix selected_formats
  # Artifact suffix reflects requested architectures (e.g., arm64, x86_64, or arm64+x86_64)
  arch_suffix="$(join_archs "$ARCHS")"
  selected_formats="$(normalize_formats "$PACKAGE_FORMATS")"
  PACKAGE_FORMATS="$selected_formats"
  [[ -n "$PACKAGE_FORMATS" ]] || die "PACKAGE_FORMATS is empty. Use one or more of: app,pkg,dmg"
  for fmt in ${PACKAGE_FORMATS//,/ }; do
    case "$fmt" in
      app|pkg|dmg) ;;
      *) die "Unsupported PACKAGE_FORMATS entry: '$fmt' (allowed: app,pkg,dmg)" ;;
    esac
  done

  # Build the app (Release) — ensures OpenSCP.app exists
  log "Configuring and building (Release)"
  # A previous packaging run may have left frameworks or plugins that are no
  # longer reachable by the current executable. Recreate only the generated
  # app bundle so stale dylibs cannot silently enter a new artifact.
  rm -rf "$APP_DIR"
  # Prefer explicit Qt env vars, then auto-detect under $HOME/Qt/<version>/macos
  local qt_cfg_dir="${Qt6_DIR:-${QT6_DIR:-}}"
  local -a dependency_cmake_args=()
  if [[ -n "${OPENSCP_DEPENDENCY_PREFIX:-}" ]]; then
    local dependency_prefix="${OPENSCP_DEPENDENCY_PREFIX}"
    local libssh2_link="${dependency_prefix}/lib/libssh2.dylib"
    [[ -f "${dependency_prefix}/include/libssh2.h" ]] ||
      die "Missing libssh2 headers under OPENSCP_DEPENDENCY_PREFIX"
    [[ -f "$libssh2_link" ]] ||
      die "Missing libssh2.dylib under OPENSCP_DEPENDENCY_PREFIX"
    dependency_cmake_args+=(
      "-DOPENSSL_ROOT_DIR=${dependency_prefix}"
      "-DOPENSSL_CRYPTO_LIBRARY=${dependency_prefix}/lib/libcrypto.dylib"
      "-DOPENSSL_SSL_LIBRARY=${dependency_prefix}/lib/libssl.dylib"
      "-DOPENSSL_INCLUDE_DIR=${dependency_prefix}/include"
      "-Dtinyxml2_DIR=${dependency_prefix}/lib/cmake/tinyxml2"
      "-DLIBSSH2_INC=${dependency_prefix}/include"
      "-DLIBSSH2_LIB=${libssh2_link}"
    )
  fi
  if [[ -z "$qt_cfg_dir" && -n "${QT_PREFIX:-}" ]]; then
    qt_cfg_dir="${QT_PREFIX}/lib/cmake/Qt6"
  fi
  if [[ -z "$qt_cfg_dir" ]]; then
    qt_cfg_dir="$(detect_qt6_dir_from_home || true)"
  fi
  local qt_prefix
  if [[ -d "$qt_cfg_dir" ]]; then
    qt_prefix="$(cd "$qt_cfg_dir/../../.." && pwd)"
    log "Using Qt from: $qt_prefix"
    setup_qt_host_wrappers_if_needed "$qt_prefix" "$BUILD_DIR"
    local cmake_prefix_path="$qt_prefix"
    if [[ -n "${CMAKE_PREFIX_PATH:-}" ]]; then
      cmake_prefix_path="${qt_prefix};${CMAKE_PREFIX_PATH}"
    fi
    local -a cmake_args=(
      -S "$REPO_DIR"
      -B "$BUILD_DIR"
      -DCMAKE_BUILD_TYPE=Release
      "-DCMAKE_PREFIX_PATH=${cmake_prefix_path}"
      "-DQt6_DIR=${qt_cfg_dir}"
      "-DBUNDLE_ID=${BUNDLE_ID}"
      "-DCMAKE_OSX_ARCHITECTURES=${ARCHS}"
      "-DCMAKE_OSX_DEPLOYMENT_TARGET=${MINIMUM_SYSTEM_VERSION}"
      "-DOPENSCP_ENFORCE_RECOMMENDED_QT_VERSION=${OPENSCP_ENFORCE_RECOMMENDED_QT_VERSION:-OFF}"
    )
    if [[ -n "$QT_HOST_WRAP_DIR" ]]; then
      cmake_args+=("-DCMAKE_AUTOUIC_EXECUTABLE=${QT_HOST_WRAP_DIR}/uic")
      cmake_args+=("-DCMAKE_AUTORCC_EXECUTABLE=${QT_HOST_WRAP_DIR}/rcc")
      cmake_args+=("-DCMAKE_AUTOMOC_EXECUTABLE=${QT_HOST_WRAP_DIR}/moc")
      cmake_args+=("-DOPENSCP_QT_HOST_TOOLS_DIR=${QT_HOST_WRAP_DIR}")
    fi
    if ((${#dependency_cmake_args[@]:-0})); then
      cmake_args+=("${dependency_cmake_args[@]}")
    fi
    cmake "${cmake_args[@]}"
  else
    if [[ -n "$qt_cfg_dir" ]]; then
      warn "Qt not found at $qt_cfg_dir; relying on system CMake find_package()"
    else
      warn "No Qt6_DIR/QT_PREFIX provided and no Qt found in \$HOME/Qt; relying on system CMake find_package()"
    fi
    local -a fallback_cmake_args=(
      -S "$REPO_DIR"
      -B "$BUILD_DIR"
      -DCMAKE_BUILD_TYPE=Release
      "-DBUNDLE_ID=${BUNDLE_ID}"
      "-DCMAKE_OSX_ARCHITECTURES=${ARCHS}"
      "-DCMAKE_OSX_DEPLOYMENT_TARGET=${MINIMUM_SYSTEM_VERSION}"
      "-DOPENSCP_ENFORCE_RECOMMENDED_QT_VERSION=${OPENSCP_ENFORCE_RECOMMENDED_QT_VERSION:-OFF}"
    )
    if ((${#dependency_cmake_args[@]:-0})); then
      fallback_cmake_args+=("${dependency_cmake_args[@]}")
    fi
    cmake "${fallback_cmake_args[@]}"
  fi
  cmake --build "$BUILD_DIR" -j

  # Expect CMake to have produced build/OpenSCP.app already
  [[ -d "$APP_DIR" ]] || die "App bundle not found at $APP_DIR. Build it first: cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j"

  # Use bundle version when available; fallback to CMake project version.
  version="$(detect_bundle_version "$INFO_PLIST_OUT")"
  if [[ -z "$version" ]]; then
    version="$(detect_version)"
  fi
  if [[ -n "${APP_VERSION:-}" && "${APP_VERSION}" != "$version" ]]; then
    warn "Ignoring APP_VERSION=${APP_VERSION}; using project version ${version} for artifact names."
  fi
  log "Packaging version: ${version}"

  # Ensure bundle key paths exist (Resources/Frameworks/PlugIns)
  mkdir -p "$RESOURCES_DIR" "$FRAMEWORKS_DIR" "$PLUGINS_DIR"

  # qt.conf
  if [[ -f "$QT_CONF_SRC" ]]; then
    cp "$QT_CONF_SRC" "$RESOURCES_DIR/qt.conf"
  fi

  # App icon: ensure exists in Resources (generate if missing)
  if [[ ! -f "$ICON_ICNS_PATH" ]]; then
    if [[ -f "${REPO_DIR}/assets/macos/${ICON_BASENAME}.icns" ]]; then
      cp "${REPO_DIR}/assets/macos/${ICON_BASENAME}.icns" "$ICON_ICNS_PATH"
    elif [[ -f "${REPO_DIR}/assets/program/icon-openscp-2048.png" ]]; then
      log "Generating ${ICON_BASENAME}.icns from PNG"
      generate_icns_from_png "${REPO_DIR}/assets/program/icon-openscp-2048.png" "$ICON_ICNS_PATH"
    fi
  fi

  # Also copy the original PNG into Resources/assets/program so AboutDialog's
  # filesystem fallback can find it if needed.
  if [[ -f "${REPO_DIR}/assets/program/icon-openscp-2048.png" ]]; then
    mkdir -p "$RESOURCES_DIR/assets/program"
    cp "${REPO_DIR}/assets/program/icon-openscp-2048.png" "$RESOURCES_DIR/assets/program/"
  fi

  # Copy licenses inside the bundle for user visibility
  if [[ -d "${REPO_DIR}/docs/credits/LICENSES" ]]; then
    mkdir -p "$RESOURCES_DIR/licenses"
    cp -R "${REPO_DIR}/docs/credits/LICENSES" "$RESOURCES_DIR/licenses/"
    [[ -f "${REPO_DIR}/docs/credits/CREDITS.md" ]] && cp "${REPO_DIR}/docs/credits/CREDITS.md" "$RESOURCES_DIR/licenses/"
  fi

  # Clean env to avoid picking up conda/Homebrew plugin paths
  unset QT_PLUGIN_PATH QML2_IMPORT_PATH QML_IMPORT_PATH DYLD_FRAMEWORK_PATH DYLD_LIBRARY_PATH DYLD_FALLBACK_LIBRARY_PATH

  # macdeployqt to bundle Qt frameworks/plugins
  local mqt
  discover_macdeployqt
  mqt="$MACDEPLOYQT_PATH"
  [[ -x "$mqt" ]] || die "Resolved macdeployqt is not executable: $mqt"
  log "Running macdeployqt at: $mqt"
  local disable_plugin_scan=0
  if [[ "${MACDEPLOYQT_DISABLE_PLUGIN_SCAN:-0}" == "1" ]]; then
    if find_qt_plugins_root >/dev/null 2>&1; then
      disable_plugin_scan=1
      warn "MACDEPLOYQT_DISABLE_PLUGIN_SCAN=1: disabling macdeployqt plugin scan"
    else
      warn "MACDEPLOYQT_DISABLE_PLUGIN_SCAN=1 but Qt plugins root was not found; keeping macdeployqt plugin scan enabled"
    fi
  else
    log "Using macdeployqt plugin scan (default)"
  fi
  # Build macdeployqt command safely even with set -u and possibly empty extra args
  local libarg=()
  if [[ -n "$QTPREFIX" && -d "$QTPREFIX/lib" ]]; then
    libarg=( "-libpath=$QTPREFIX/lib" )
  fi
  local cmd=("$mqt" "$APP_DIR" -always-overwrite -verbose=1)
  if [[ $disable_plugin_scan -eq 1 ]]; then
    cmd+=( -no-plugins )
  fi
  if ((${#libarg[@]:-0})); then
    cmd+=("${libarg[@]}")
  fi
  if ! "${cmd[@]}"; then
    # Some local Qt installs on Apple Silicon can require running Qt host tools
    # through Rosetta (seen as "Incompatible processor ... neon").
    warn "macdeployqt failed natively; retrying through Rosetta (x86_64)"
    local cmd_x86=(/usr/bin/arch -x86_64 "$mqt" "$APP_DIR" -always-overwrite -verbose=1)
    if [[ $disable_plugin_scan -eq 1 ]]; then
      cmd_x86+=( -no-plugins )
    fi
    if ((${#libarg[@]:-0})); then
      cmd_x86+=("${libarg[@]}")
    fi
    "${cmd_x86[@]}" || die "macdeployqt failed (native and Rosetta fallback)"
  fi

  # Force-copy essential Qt frameworks into the bundle from known prefixes
  if [[ -n "$QTPREFIX" && -d "$QTPREFIX/lib" ]]; then
    warn "Ensuring Qt frameworks from: $QTPREFIX/lib"
    for fw in QtCore QtGui QtWidgets QtPrintSupport; do
      copy_qt_framework "$QTPREFIX/lib" "$fw"
    done
  fi
  # Homebrew fallback if official prefix missing
  if [[ ! -e "$FRAMEWORKS_DIR/QtWidgets.framework/QtWidgets" && ! -e "$FRAMEWORKS_DIR/QtWidgets.framework/Versions/A/QtWidgets" ]]; then
    if command -v brew >/dev/null 2>&1; then
      local hbqt
      hbqt=$(brew --prefix qt 2>/dev/null || brew --prefix qt@6 2>/dev/null || true)
      if [[ -n "$hbqt" && -d "$hbqt/lib" ]]; then
        warn "Ensuring Qt frameworks from Homebrew: $hbqt/lib"
        for fw in QtCore QtGui QtWidgets QtPrintSupport; do
          copy_qt_framework "$hbqt/lib" "$fw"
        done
      fi
    fi
  fi

  # Ensure the specific plugin families we depend on are present in the bundle.
  ensure_qt_support_plugins
  prune_optional_qt_plugins

  # Bundle non-Qt dependencies: libssh2 + OpenSSL (libcrypto) + tinyxml2
  log "Bundling non-Qt dependencies"
  bundle_non_qt_deps

  # Final pass: prune optional plugins that still leak external references.
  log "Sanitizing bundled Qt plugins"
  sanitize_qt_plugins_linkage

  # Validate and fix any lingering Homebrew/Conda refs
  log "Validating linkage for internal libraries"
  otool -L "$MACOS_DIR/${APP_NAME}" | grep -E 'libssh2|libcrypto|libssl|tinyxml2|@executable_path' || true
  rewrite_external_refs_to_bundle "$MACOS_DIR/${APP_NAME}"

  # Sign (hardened runtime) — skipped entirely when SKIP_CODESIGN=1
  if [[ "${SKIP_CODESIGN:-0}" != "1" ]]; then
    log "Code signing bundle with identity: ${APPLE_IDENTITY:-<unset>}"
    sign_app_bundle
    codesign --verify --deep --strict --verbose=2 "$APP_DIR"
  else
    # Ad‑hoc sign by default to avoid Code Signature Invalid on modified frameworks
    if [[ "${DO_ADHOC_SIGN:-1}" == "1" ]]; then
      adhoc_sign_bundle
    else
      warn "SKIP_CODESIGN=1 and DO_ADHOC_SIGN=0: skipping all signing"
    fi
  fi

  local produced=()

  if has_format app; then
    local app_zip_path
    app_zip_path="${DIST_DIR}/${APP_NAME}-${version}-${arch_suffix}-UNSIGNED.zip"
    log "Creating app ZIP: $app_zip_path"
    create_app_zip "$app_zip_path" "$APP_DIR"
    write_sha256 "$app_zip_path"
    produced+=("$app_zip_path")
  fi

  if has_format pkg; then
    local pkg_path
    pkg_path="${DIST_DIR}/${APP_NAME}-${version}-${arch_suffix}-UNSIGNED.pkg"
    log "Creating PKG: $pkg_path"
    create_pkg "$pkg_path" "$APP_DIR" "$version"
    write_sha256 "$pkg_path"
    produced+=("$pkg_path")
    if [[ "${SKIP_CODESIGN:-0}" != "1" && "${SKIP_NOTARIZATION:-0}" != "1" ]]; then
      warn "Notarization flow is currently DMG-only; PKG was generated without notarization"
    fi
  fi

  if has_format dmg; then
    local dmg_path
    dmg_path="${DIST_DIR}/${APP_NAME}-${version}-${arch_suffix}-UNSIGNED.dmg"
    log "Creating DMG: $dmg_path"
    create_dmg "$dmg_path" "$APP_NAME" "$APP_DIR"
    write_sha256 "$dmg_path"
    produced+=("$dmg_path")

    # Notarize and staple — completely skipped when SKIP_NOTARIZATION=1
    if [[ "${SKIP_CODESIGN:-0}" != "1" && "${SKIP_NOTARIZATION:-0}" != "1" ]]; then
      notarize_and_staple "$dmg_path"
    else
      warn "Skipping notarization (SKIP_CODESIGN or SKIP_NOTARIZATION enabled)"
    fi

    # Print release notes snippet for GitHub Releases (unsigned app instructions)
    local sha
    if [[ -f "${dmg_path}.sha256" ]]; then sha="$(cat "${dmg_path}.sha256")"; else sha="(sha256 not generated)"; fi
    cat << 'EOF'

====================
GitHub Release Notes
====================

This macOS build is unsigned (for testing and advanced users).

Install:
- Open the DMG and drag OpenSCP.app into /Applications
- First launch: Apple may block it because the developer is not identified.

To open it anyway:
- GUI: Right‑click OpenSCP.app → Open → Open
- Terminal (to remove quarantine):
  xattr -dr com.apple.quarantine /Applications/OpenSCP.app

SHA256 (DMG):
EOF
    echo "${sha}  $(basename "$dmg_path")"
  fi

  log "Done. Produced artifacts:"
  if ((${#produced[@]:-0})); then
    for out in "${produced[@]}"; do
      log "  - $out"
    done
  else
    warn "No artifact generated (PACKAGE_FORMATS=${PACKAGE_FORMATS})"
  fi
}

main "$@"
