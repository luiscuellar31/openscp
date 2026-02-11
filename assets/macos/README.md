# Local macOS Packaging (Unsigned app/pkg/dmg)

This guide explains how to build the `.app` and create unsigned macOS artifacts (`.zip` app, `.pkg`, `.dmg`) for local testing/distribution.

- Target app bundle: `build/OpenSCP.app`
- Output artifacts:
  - `dist/OpenSCP-<version>-<arch>-UNSIGNED.zip` (app bundle zipped)
  - `dist/OpenSCP-<version>-<arch>-UNSIGNED.pkg`
  - `dist/OpenSCP-<version>-<arch>-UNSIGNED.dmg`
- Scripts: `scripts/macos.sh` (simple entrypoint) and `scripts/package_mac.sh` (advanced)

## Prerequisites

- Qt 6.8.3 (official installer), not Conda. The script prioritizes:
  - `/Users/luiscuellar/Qt/6.8.3/macos/bin/macdeployqt`
  - or `Qt6_DIR=/Users/luiscuellar/Qt/6.8.3/macos/lib/cmake/Qt6`
- Homebrew libraries for build/runtime (copied into the bundle and rewritten):
  - `brew install libssh2 openssl@3`
- CMake 3.22+, a C++20 compiler.

Tip: The script clears env vars like `QT_PLUGIN_PATH` and `DYLD_*` to avoid pulling plugins from Conda/Homebrew.

## Build & Package (Unsigned)

```bash
# From the repository root
./scripts/macos.sh app   # ZIP containing OpenSCP.app
./scripts/macos.sh pkg   # PKG installer
./scripts/macos.sh dmg   # DMG
# all:
./scripts/macos.sh dist
```

What it does:
- Builds Release with CMake and produces `build/OpenSCP.app`.
- Runs `macdeployqt` to bundle Qt frameworks/plugins.
- Bundles non‑Qt deps (`libssh2`, `libcrypto`) into `Contents/Frameworks` and fixes `install_name_tool` + `@rpath`.
- Creates the selected artifact(s) under `dist/` plus `*.sha256`.
- Prints release notes snippet when DMG is generated.

## Artifacts and SHA256

- App ZIP: `dist/OpenSCP-<version>-<arch>-UNSIGNED.zip`
- PKG: `dist/OpenSCP-<version>-<arch>-UNSIGNED.pkg`
- DMG: `dist/OpenSCP-<version>-<arch>-UNSIGNED.dmg`
- Checksum: same file with `.sha256` suffix.

## Architectures (Intel vs Apple Silicon)

Default build targets `arm64`. To build for Intel Macs (`x86_64`) or a universal binary, set the architecture before running the script:

```bash
# Intel-only
export CMAKE_OSX_ARCHITECTURES=x86_64

# Universal (requires universal Qt and deps)
export CMAKE_OSX_ARCHITECTURES='arm64;x86_64'

# Make sure Qt matches your target arch; e.g. for Intel:
export Qt6_DIR=/path/to/Qt/6.8.3/macos/lib/cmake/Qt6

./scripts/macos.sh dist
```

Tip: When switching architectures, clean the build directory (`rm -rf build`) to avoid cache mismatches.

You can publish both files in a GitHub Release and copy/paste the notes the script prints at the end.

## How to Open an Unsigned App on macOS

- GUI: Right‑click `OpenSCP.app` in `/Applications` → Open → Open
- Terminal (remove quarantine):

```bash
xattr -dr com.apple.quarantine /Applications/OpenSCP.app
```

## Verify Internal Linkage (optional)

The script also validates and corrects linkage, but you can check manually:

```bash
otool -L build/OpenSCP.app/Contents/MacOS/OpenSCP | \
  grep -E 'libssh2|libcrypto|libssl|@executable_path'
```

Expect library references to be `@executable_path/../Frameworks/...`.

## Troubleshooting

- Script refuses `macdeployqt` from Conda:
  - Set `Qt6_DIR=/Users/luiscuellar/Qt/6.8.3/macos/lib/cmake/Qt6` or ensure the official Qt path exists at `/Users/luiscuellar/Qt/6.8.3/macos/bin/macdeployqt`.
- Missing `libssh2`/`openssl@3`:
  - `brew install libssh2 openssl@3`
- Still seeing Homebrew/Conda absolute paths in the binary:
  - Re‑run the script; it rewrites to `@executable_path/../Frameworks` where possible.

## Later: Signing/Notarization

This flow intentionally skips signing/notarization. If you later obtain a Developer ID certificate and an Apple API key, run `scripts/package_mac.sh` directly and configure:
- `APPLE_IDENTITY`, `APPLE_TEAM_ID`
- `APPLE_API_KEY_ID`, `APPLE_API_ISSUER_ID`, `APPLE_API_KEY_P8`

The script will then sign, notarize, and staple the DMG automatically.
