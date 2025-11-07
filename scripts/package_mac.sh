#!/usr/bin/env bash

set -euo pipefail

# OpenSCP macOS packaging script
# - Assumes OpenSCP.app is already produced by CMake (MACOSX_BUNDLE)
# - Runs macdeployqt to bundle Qt frameworks/plugins
# - Bundles non-Qt deps (libssh2, OpenSSL)
# - Code-signs (hardened runtime) recursively
# - Creates a compressed .dmg
# - Notarizes with notarytool and staples the ticket
#
# Configuration via environment variables (local-only usage):
#   APP_NAME               Default: "OpenSCP"
#   BUNDLE_ID              Default: "com.example.openscp"
#   MINIMUM_SYSTEM_VERSION Default: "12.0" (used only for Info.plist checks)
#   CMAKE_OSX_ARCHITECTURES Default: "arm64" (or "arm64;x86_64" for universal naming)
#   CMAKE_PREFIX_PATH      Path to your Qt 6 install root (if not in default search path)
#   Qt6_DIR                Path to Qt6 CMake config dir (…/lib/cmake/Qt6); used to derive Qt bin for macdeployqt
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
# Output:
#   dist/<APP_NAME>-<VERSION>-<ARCH>-UNSIGNED.dmg (hash .sha256 alongside)
#     where <ARCH> is arm64, x86_64, or arm64+x86_64

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${REPO_DIR}/build"
DIST_DIR="${REPO_DIR}/dist"

APP_NAME="${APP_NAME:-OpenSCP}"
BUNDLE_ID="${BUNDLE_ID:-com.example.openscp}"
MINIMUM_SYSTEM_VERSION="${MINIMUM_SYSTEM_VERSION:-12.0}"
ARCHS="${CMAKE_OSX_ARCHITECTURES:-arm64}"
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

# Helpers
log() { printf "\033[1;34m[pack]\033[0m %s\n" "$*"; }
warn() { printf "\033[1;33m[warn]\033[0m %s\n" "$*"; }
err() { printf "\033[1;31m[err ]\033[0m %s\n" "$*"; }
die() { err "$*"; exit 1; }

ensure_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "Missing required tool: $1"
}

discover_macdeployqt() {
  # 1) Force official Qt first
  local forced="/Users/luiscuellar/Qt/6.8.3/macos/bin/macdeployqt"
  if [[ -x "$forced" ]]; then
    QTPREFIX="$(cd "$(dirname "$forced")/.." && pwd)"
    echo "$forced"; return
  fi
  # 2) Qt6_DIR based lookup
  if [[ -n "${Qt6_DIR:-}" ]]; then
    local cand="${Qt6_DIR}/../../bin/macdeployqt"
    if [[ -x "$cand" ]]; then
      # Avoid conda/macdeployqt
      local real
      real="$(/usr/bin/realpath "$cand" 2>/dev/null || echo "$cand")"
      if [[ "$real" == *miniconda* ]]; then
        die "Refusing to use conda macdeployqt at: $real. Point Qt6_DIR to your official Qt (e.g., /Users/luiscuellar/Qt/6.8.3/macos/lib/cmake/Qt6)."
      fi
      QTPREFIX="$(cd "$(dirname "$cand")/.." && pwd)"
      echo "$cand"; return
    fi
  fi
  # 3) PATH fallback (reject miniconda)
  if command -v macdeployqt >/dev/null 2>&1; then
    local pathbin
    pathbin="$(command -v macdeployqt)"
    local real
    real="$(/usr/bin/realpath "$pathbin" 2>/dev/null || echo "$pathbin")"
    if [[ "$real" == *miniconda* ]]; then
      die "Found macdeployqt in conda path: $real. Please use official Qt macdeployqt or set Qt6_DIR to your Qt installation."
    fi
    QTPREFIX="$(cd "$(dirname "$pathbin")/.." && pwd)"
    echo "$pathbin"; return
  fi
  die "macdeployqt not found. Install Qt 6.8.3 and ensure macdeployqt is available."
}

year() { date +%Y; }

detect_version() {
  # Try to extract from top-level CMakeLists.txt: project(... VERSION x.y.z)
  local ver
  ver=$(sed -n 's/^project(.*VERSION[[:space:]]*\([0-9][0-9.]*\).*/\1/p' "${REPO_DIR}/CMakeLists.txt" | head -n1 || true)
  if [[ -z "$ver" ]]; then ver="0.0.0"; fi
  echo "$ver"
}

join_archs() {
  local s="$1"; echo "${s//;/+}"
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

ensure_rpath() {
  local bin="$1"; local rpath='@executable_path/../Frameworks'
  if ! otool -l "$bin" | grep -q "$rpath"; then
    install_name_tool -add_rpath "$rpath" "$bin"
  fi
}

redirect_dep_to_rpath() {
  local bin="$1"; local old="$2"; local base
  base="$(basename "$old")"
  install_name_tool -change "$old" "@rpath/${base}" "$bin"
}

sign_item() {
  local path="$1"
  [[ "${SKIP_CODESIGN:-0}" == "1" ]] && { warn "Skipping codesign: $path"; return; }
  [[ -z "${APPLE_IDENTITY:-}" ]] && die "APPLE_IDENTITY is not set"
  codesign --force --timestamp --options runtime \
    --entitlements "$ENTITLEMENTS_FILE" \
    --sign "${APPLE_IDENTITY}" "$path"
}

sign_app_bundle() {
  # Sign nested content first: dylibs, frameworks, plugins, then the app
  shopt -s nullglob
  local items=()
  items+=("${FRAMEWORKS_DIR}"/*.dylib)
  items+=("${FRAMEWORKS_DIR}"/*.framework)
  items+=("${PLUGINS_DIR}"/**/*)
  for i in "${items[@]}"; do
    if [[ -e "$i" ]]; then sign_item "$i"; fi
  done
  sign_item "$APP_DIR"
}

# Ad‑hoc sign everything to purge invalid upstream signatures (no cert required).
adhoc_sign_item() {
  local path="$1"
  codesign --force -s - --timestamp=none "$path" 2>/dev/null || true
}

adhoc_sign_bundle() {
  log "Ad‑hoc signing bundle (no Developer ID)"
  shopt -s nullglob
  # Framework inner binaries (Versions/A/<Name>) first
  local fwbin
  for fw in "${FRAMEWORKS_DIR}"/*.framework; do
    [[ -d "$fw" ]] || continue
    local name; name="$(basename "$fw" .framework)"
    if [[ -f "$fw/Versions/A/$name" ]]; then fwbin="$fw/Versions/A/$name"; else fwbin="$fw/$name"; fi
    adhoc_sign_item "$fwbin"
    adhoc_sign_item "$fw"
  done
  # Plain dylibs
  for dyl in "${FRAMEWORKS_DIR}"/*.dylib; do
    [[ -e "$dyl" ]] && adhoc_sign_item "$dyl"
  done
  # Plugins
  for p in "${PLUGINS_DIR}"/**/*; do
    [[ -f "$p" ]] && adhoc_sign_item "$p"
  done
  # App binary and container
  adhoc_sign_item "$MACOS_DIR/${APP_NAME}"
  adhoc_sign_item "$APP_DIR"
}

bundle_non_qt_deps() {
  # Ensure non-Qt libraries (libssh2, OpenSSL libcrypto) are present in Frameworks.
  # Newer macdeployqt may already copy them; in that case we skip copying and just ensure IDs/RPATH.
  mkdir -p "$FRAMEWORKS_DIR"

  local exe="$MACOS_DIR/${APP_NAME}"
  local want_libssh2="" want_libcrypto=""
  if otool -L "$exe" | grep -q "libssh2"; then want_libssh2=1; fi
  if otool -L "$exe" | grep -q "libcrypto"; then want_libcrypto=1; fi

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

  local libs_to_fix=()

  if [[ -n "$want_libssh2" ]]; then
    local existing
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
    # Set ID to @rpath/<name>
    if [[ -f "$existing" ]]; then
      install_name_tool -id "@rpath/$(basename "$existing")" "$existing" || true
    fi
  fi

  if [[ -n "$want_libcrypto" ]]; then
    local existing
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
    done < <(otool -L "$lib" | tail -n +2)
  done
}

# Copy a Qt *.framework from a source lib dir into the app bundle Frameworks
copy_qt_framework() {
  local src_root="$1"   # e.g., /Users/.../Qt/6.8.3/macos/lib
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

create_dmg() {
  local dmg_path="$1"; local volname="$2"; local src_app="$3"
  local staging
  staging="$(mktemp -d)"
  mkdir -p "$staging"
  cp -R "$src_app" "$staging/"
  ln -s /Applications "$staging/Applications"
  hdiutil create -quiet -fs HFS+ -volname "$volname" -srcfolder "$staging" -format UDZO -imagekey zlib-level=9 "$dmg_path"
  rm -rf "$staging"
}

notarize_and_staple() {
  local dmg="$1"
  [[ "${SKIP_NOTARIZATION:-0}" == "1" ]] && { warn "Skipping notarization"; return; }
  for v in APPLE_TEAM_ID APPLE_API_KEY_ID APPLE_API_ISSUER_ID APPLE_API_KEY_P8; do
    [[ -n "${!v:-}" ]] || die "Missing $v for notarization"
  done
  ensure_cmd xcrun
  local keyfile; keyfile="$(mktemp -t AuthKey).p8"
  chmod 600 "$keyfile"
  printf "%s" "${APPLE_API_KEY_P8}" > "$keyfile"
  log "Submitting for notarization (this may take a few minutes)"
  xcrun notarytool submit "$dmg" \
    --key "$keyfile" \
    --key-id "${APPLE_API_KEY_ID}" \
    --issuer "${APPLE_API_ISSUER_ID}" \
    --team-id "${APPLE_TEAM_ID}" \
    --wait
  rm -f "$keyfile"
  log "Stapling notarization ticket"
  xcrun stapler staple "$dmg"
}

main() {
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

  # Determine app version (from CMake) and architecture suffix
  local version arch_suffix
  version="${APP_VERSION:-$(detect_version)}"
  # Artifact suffix reflects requested architectures (e.g., arm64, x86_64, or arm64+x86_64)
  arch_suffix="$(join_archs "$ARCHS")"

  # Build the app (Release) — ensures OpenSCP.app exists
  log "Configuring and building (Release)"
  # Prefer official Qt prefix if available
  local qt_cfg_dir="${Qt6_DIR:-/Users/luiscuellar/Qt/6.8.3/macos/lib/cmake/Qt6}"
  local qt_prefix
  if [[ -d "$qt_cfg_dir" ]]; then
    qt_prefix="$(cd "$qt_cfg_dir/../.." && pwd)"
    log "Using Qt from: $qt_prefix"
    cmake -S "$REPO_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH="$qt_prefix" -DQt6_DIR="$qt_cfg_dir" \
      -DCMAKE_OSX_ARCHITECTURES="$ARCHS"
  else
    warn "Official Qt not found at $qt_cfg_dir; relying on system CMake find_package()"
    cmake -S "$REPO_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_OSX_ARCHITECTURES="$ARCHS"
  fi
  cmake --build "$BUILD_DIR" -j

  # Expect CMake to have produced build/OpenSCP.app already
  [[ -d "$APP_DIR" ]] || die "App bundle not found at $APP_DIR. Build it first: cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j"

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

  # Copy licenses inside the bundle for user visibility
  if [[ -d "${REPO_DIR}/docs/credits/LICENSES" ]]; then
    mkdir -p "$RESOURCES_DIR/licenses"
    cp -R "${REPO_DIR}/docs/credits/LICENSES" "$RESOURCES_DIR/licenses/"
    [[ -f "${REPO_DIR}/docs/credits/CREDITS.md" ]] && cp "${REPO_DIR}/docs/credits/CREDITS.md" "$RESOURCES_DIR/licenses/"
  fi

  # Copy full docs directory so About dialog can load ABOUT_LIBRARIES_* from docs/
  # Place it under Resources/docs to match the runtime search paths in AboutDialog.cpp
  if [[ -d "${REPO_DIR}/docs" ]]; then
    mkdir -p "$RESOURCES_DIR/docs"
    # Copy contents (including nested credits/LICENSES) to ensure internal references resolve
    cp -R "${REPO_DIR}/docs/." "$RESOURCES_DIR/docs/"
  fi

  # Clean env to avoid picking up conda/Homebrew plugin paths
  unset QT_PLUGIN_PATH QML2_IMPORT_PATH QML_IMPORT_PATH DYLD_FRAMEWORK_PATH DYLD_LIBRARY_PATH DYLD_FALLBACK_LIBRARY_PATH

  # macdeployqt to bundle Qt frameworks/plugins
  local mqt
  mqt="$(discover_macdeployqt)"
  log "Running macdeployqt at: $mqt"
  # Build macdeployqt command safely even with set -u and possibly empty extra args
  local libarg=()
  if [[ -n "$QTPREFIX" && -d "$QTPREFIX/lib" ]]; then
    libarg=( -libpath "$QTPREFIX/lib" )
  fi
  local cmd=("$mqt" "$APP_DIR" -always-overwrite -verbose=3)
  if ((${#libarg[@]:-0})); then
    cmd+=("${libarg[@]}")
  fi
  "${cmd[@]}" || die "macdeployqt failed"

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

  # Bundle non-Qt dependencies: libssh2 + OpenSSL (libcrypto)
  log "Bundling non-Qt dependencies"
  bundle_non_qt_deps

  # Validate and fix any lingering Homebrew/Conda refs
  log "Validating linkage for internal libraries"
  otool -L "$MACOS_DIR/${APP_NAME}" | grep -E 'libssh2|libcrypto|libssl|@executable_path' || true
  # Redirect any remaining absolute paths for our key libs found in the executable
  while read -r dep; do
    base=$(basename "$dep")
    if [[ -f "$FRAMEWORKS_DIR/$base" ]]; then
      install_name_tool -change "$dep" "@executable_path/../Frameworks/$base" "$MACOS_DIR/${APP_NAME}" || true
    fi
  done < <(otool -L "$MACOS_DIR/${APP_NAME}" | awk 'NR>1{print $1}' | grep -E '/(opt/homebrew|usr/local|miniconda).*(lib(ssh2|crypto|ssl).*)' || true)

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

  # Create DMG
  local dmg_path
  dmg_path="${DIST_DIR}/${APP_NAME}-${version}-${arch_suffix}-UNSIGNED.dmg"
  log "Creating DMG: $dmg_path"
  rm -f "$dmg_path"
  create_dmg "$dmg_path" "$APP_NAME" "$APP_DIR"

  # Generate SHA256 checksum file
  if command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$dmg_path" | awk '{print $1}' > "${dmg_path}.sha256"
  elif command -v openssl >/dev/null 2>&1; then
    openssl dgst -sha256 -r "$dmg_path" | awk '{print $1}' > "${dmg_path}.sha256"
  fi

  # Notarize and staple — completely skipped when SKIP_NOTARIZATION=1
  if [[ "${SKIP_CODESIGN:-0}" != "1" && "${SKIP_NOTARIZATION:-0}" != "1" ]]; then
    notarize_and_staple "$dmg_path"
  else
    warn "Skipping notarization (SKIP_CODESIGN or SKIP_NOTARIZATION enabled)"
  fi

  log "Done: $dmg_path"

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
}

main "$@"
