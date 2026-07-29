#!/usr/bin/env bash

# Signing helpers sourced by scripts/package_mac.sh. The caller owns all
# configuration and logging functions.

sign_item() {
  local path="$1"
  [[ "${SKIP_CODESIGN:-0}" == "1" ]] && {
    warn "Skipping codesign: $path"
    return
  }
  [[ -z "${APPLE_IDENTITY:-}" ]] && die "APPLE_IDENTITY is not set"
  codesign --force --timestamp --options runtime \
    --entitlements "$ENTITLEMENTS_FILE" \
    --sign "${APPLE_IDENTITY}" "$path"
}

sign_app_bundle() {
  # Sign nested content first: dylibs, frameworks, plugins, then the app.
  shopt -s nullglob
  local items=()
  items+=("${FRAMEWORKS_DIR}"/*.dylib)
  items+=("${FRAMEWORKS_DIR}"/*.framework)
  items+=("${PLUGINS_DIR}"/**/*)
  local item
  for item in "${items[@]}"; do
    if [[ -e "$item" ]]; then
      sign_item "$item"
    fi
  done
  sign_item "$APP_DIR"
}

adhoc_sign_item() {
  local path="$1"
  codesign --force -s - --timestamp=none "$path" 2>/dev/null || true
}

adhoc_sign_bundle() {
  log "Ad-hoc signing bundle (no Developer ID)"
  shopt -s nullglob
  local framework_binary
  local framework
  for framework in "${FRAMEWORKS_DIR}"/*.framework; do
    [[ -d "$framework" ]] || continue
    local framework_name
    framework_name="$(basename "$framework" .framework)"
    if [[ -f "$framework/Versions/A/$framework_name" ]]; then
      framework_binary="$framework/Versions/A/$framework_name"
    else
      framework_binary="$framework/$framework_name"
    fi
    adhoc_sign_item "$framework_binary"
    adhoc_sign_item "$framework"
  done

  local dynamic_library
  for dynamic_library in "${FRAMEWORKS_DIR}"/*.dylib; do
    [[ -e "$dynamic_library" ]] && adhoc_sign_item "$dynamic_library"
  done

  local plugin
  for plugin in "${PLUGINS_DIR}"/**/*; do
    [[ -f "$plugin" ]] && adhoc_sign_item "$plugin"
  done
  adhoc_sign_item "$MACOS_DIR/${APP_NAME}"
  adhoc_sign_item "$APP_DIR"
}
