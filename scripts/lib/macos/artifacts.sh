#!/usr/bin/env bash

# Artifact and notarization helpers sourced by scripts/package/macos.sh.

create_dmg() {
  local dmg_path="$1"
  local volume_name="$2"
  local source_app="$3"
  local staging
  staging="$(mktemp -d)"
  rm -f "$dmg_path"
  mkdir -p "$staging"
  cp -R "$source_app" "$staging/"
  ln -s /Applications "$staging/Applications"
  ensure_cmd hdiutil

  local attempt=1
  local maximum_attempts=3
  while (( attempt <= maximum_attempts )); do
    if hdiutil create -ov -fs HFS+ -volname "$volume_name" \
      -srcfolder "$staging" -format UDZO -imagekey zlib-level=9 \
      "$dmg_path"; then
      break
    fi
    warn "hdiutil create failed (attempt ${attempt}/${maximum_attempts})"
    rm -f "$dmg_path"
    if (( attempt == maximum_attempts )); then
      rm -rf "$staging"
      die "Unable to create DMG after ${maximum_attempts} attempts"
    fi
    sleep 2
    ((attempt++))
  done
  rm -rf "$staging"
}

create_app_zip() {
  local zip_path="$1"
  local source_app="$2"
  rm -f "$zip_path"
  ditto -c -k --sequesterRsrc --keepParent "$source_app" "$zip_path"
}

create_pkg() {
  local pkg_path="$1"
  local source_app="$2"
  local pkg_version="$3"
  ensure_cmd pkgbuild
  rm -f "$pkg_path"

  local staging
  local component_pkg
  local component_plist
  local package_identifier
  staging="$(mktemp -d)"
  component_pkg="${staging}/component.pkg"
  component_plist="${staging}/components.plist"
  package_identifier="${PKG_IDENTIFIER:-io.github.luiscuellar31.openscp.pkg}"
  cp -R "$source_app" "$staging/"
  pkgbuild --analyze --root "$staging" "$component_plist" >/dev/null

  # Local development copies must not cause Installer to skip the payload.
  if [[ -x /usr/libexec/PlistBuddy ]]; then
    /usr/libexec/PlistBuddy \
      -c 'Set :0:BundleIsVersionChecked false' \
      "$component_plist" || true
    /usr/libexec/PlistBuddy \
      -c 'Set :0:BundleIsRelocatable false' \
      "$component_plist" || true
  fi
  pkgbuild \
    --root "$staging" \
    --component-plist "$component_plist" \
    --identifier "$package_identifier" \
    --version "$pkg_version" \
    --install-location /Applications \
    "$component_pkg"
  mv "$component_pkg" "$pkg_path"
  rm -rf "$staging"
}

notarize_and_staple() {
  local dmg_path="$1"
  [[ "${SKIP_NOTARIZATION:-0}" == "1" ]] && {
    warn "Skipping notarization"
    return
  }
  local required_variable
  for required_variable in \
    APPLE_TEAM_ID APPLE_API_KEY_ID APPLE_API_ISSUER_ID APPLE_API_KEY_P8; do
    [[ -n "${!required_variable:-}" ]] ||
      die "Missing $required_variable for notarization"
  done
  ensure_cmd xcrun

  local key_file
  key_file="$(mktemp -t AuthKey).p8"
  chmod 600 "$key_file"
  printf "%s" "${APPLE_API_KEY_P8}" > "$key_file"
  log "Submitting for notarization (this may take a few minutes)"
  xcrun notarytool submit "$dmg_path" \
    --key "$key_file" \
    --key-id "${APPLE_API_KEY_ID}" \
    --issuer "${APPLE_API_ISSUER_ID}" \
    --team-id "${APPLE_TEAM_ID}" \
    --wait
  rm -f "$key_file"
  log "Stapling notarization ticket"
  xcrun stapler staple "$dmg_path"
}
