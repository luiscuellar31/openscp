# Local macOS Packaging (Unsigned app/pkg/dmg)

This guide explains how to build the `.app` and create unsigned macOS artifacts (`.zip` app, `.pkg`, `.dmg`) for local testing/distribution.

- Target app bundle: `build/OpenSCP.app`
- Output artifacts:
    - `dist/OpenSCP-<version>-<arch>-UNSIGNED.zip` (app bundle zipped)
    - `dist/OpenSCP-<version>-<arch>-UNSIGNED.pkg`
    - `dist/OpenSCP-<version>-<arch>-UNSIGNED.dmg`
- Scripts: `scripts/macos.sh` (simple entrypoint) and `scripts/package_mac.sh` (advanced)

## Prerequisites

- A compatible Qt 6.x from the official installer, not Conda. Qt 6.8.3 is
  recommended and pinned for official self-contained artifacts. The script
  prioritizes:
    - `QT_PREFIX` / `Qt6_DIR` if provided
    - or auto-detection under `$HOME/Qt/<version>/macos`
- If your Qt is in another location, set:
    - `QT_PREFIX=/path/to/Qt/<version>/macos`
    - or `Qt6_DIR=/path/to/Qt/<version>/macos/lib/cmake/Qt6`
- Non-Qt development libraries. Homebrew is convenient for local builds:
    - `brew install libssh2 openssl@3 tinyxml2`
  Official artifacts instead use `scripts/ci/build_macos_release_deps.sh` so
  every included library has the macOS 12 deployment target.
- CMake 3.22+, a C++20 compiler.

Tip: The script clears env vars like `QT_PLUGIN_PATH` and `DYLD_*` to avoid pulling plugins from Conda/Homebrew.

## Daily Development Workflow

Use `scripts/macos.sh` for a consistent local loop:

```bash
# configure + build + open app bundle
./scripts/macos.sh dev

# step-by-step
./scripts/macos.sh configure
./scripts/macos.sh build
./scripts/macos.sh run
```

`run` opens `build/OpenSCP.app`. If LaunchServices fails, the script falls back to running the app binary directly with Qt env hints.

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
- Bundles non‑Qt deps (`libssh2`, `libcrypto`, `tinyxml2`) into `Contents/Frameworks` and fixes `install_name_tool` + `@rpath`.
- Creates the selected artifact(s) under `dist/` plus `*.sha256`.
- Prints release notes snippet when DMG is generated.

`scripts/package_mac.sh` is the orchestration entrypoint. Its signing helpers
live in `scripts/macos/package_signing.sh`, while artifact creation and
notarization live in `scripts/macos/package_artifacts.sh`; source the
orchestrator rather than invoking those internal modules directly.

## Artifacts and SHA256

- App ZIP: `dist/OpenSCP-<version>-<arch>-UNSIGNED.zip`
- PKG: `dist/OpenSCP-<version>-<arch>-UNSIGNED.pkg`
- DMG: `dist/OpenSCP-<version>-<arch>-UNSIGNED.dmg`
- Checksum: same file with `.sha256` suffix.

## Architectures (Intel vs Apple Silicon)

By default the build uses the current Mac's native architecture. To build for
another architecture or a universal binary, set it before running the script:

```bash
# Intel-only
export CMAKE_OSX_ARCHITECTURES=x86_64

# Universal (requires universal Qt and deps)
export CMAKE_OSX_ARCHITECTURES='arm64;x86_64'

# Make sure Qt matches your target arch; e.g. for Intel:
export Qt6_DIR=/path/to/Qt/<version>/macos/lib/cmake/Qt6

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
    grep -E 'libssh2|libcrypto|libssl|tinyxml2|@executable_path'
```

Expect library references to be `@executable_path/../Frameworks/...`.

## Troubleshooting

- Script refuses `macdeployqt` from Conda:
    - Set `Qt6_DIR=$HOME/Qt/<version>/macos/lib/cmake/Qt6` or set `QT_PREFIX=$HOME/Qt/<version>/macos`.
- Missing `libssh2`/`openssl@3`/`tinyxml2`:
    - `brew install libssh2 openssl@3 tinyxml2`
- Linker says a Homebrew library was built for a newer macOS version:
    - Every bundled library must support `MINIMUM_SYSTEM_VERSION`. Rebuild that
      dependency for the intended deployment target or raise
      `MINIMUM_SYSTEM_VERSION` to the artifact's actual minimum. The bundle
      verifier intentionally rejects an inconsistent package.
    - To reproduce the official macOS 12 dependency baseline, run
      `MACOSX_DEPLOYMENT_TARGET=12.0 CMAKE_OSX_ARCHITECTURES=$(uname -m) ./scripts/ci/build_macos_release_deps.sh /absolute/prefix`
      and export `OPENSCP_DEPENDENCY_PREFIX`, `OPENSSL_ROOT_DIR`,
      `CMAKE_PREFIX_PATH`, and `PKG_CONFIG_PATH` for that prefix.
- Still seeing Homebrew/Conda absolute paths in the binary:
    - Re‑run the script; it rewrites to `@executable_path/../Frameworks` where possible.

## Later: Signing/Notarization

This flow intentionally skips signing/notarization. If you later obtain a Developer ID certificate and an Apple API key, run `scripts/package_mac.sh` directly and configure:
- `APPLE_IDENTITY`, `APPLE_TEAM_ID`
- `APPLE_API_KEY_ID`, `APPLE_API_ISSUER_ID`, `APPLE_API_KEY_P8`

The script will then sign, notarize, and staple the DMG automatically.
