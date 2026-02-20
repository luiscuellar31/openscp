# Linux Build and Packaging

This guide explains how to build OpenSCP from source on Linux and package it as:

- AppImage
- Snap
- Flatpak

Main artifacts:

- Source binary: `build/openscp_hello`
- AppImage: `dist/OpenSCP-<version>-<arch>.AppImage`
- Snap: `packaging/snap/*.snap`
- Flatpak bundle: `dist/OpenSCP.flatpak`

## Prerequisites

- Qt 6.x (tested with 6.8.3)
- libssh2 (OpenSSL 3 recommended)
- CMake 3.22+ and a C++20 compiler

Packaging tools:

- AppImage: `linuxdeploy`, `linuxdeploy-plugin-qt`, `appimagetool`
- Snap: `snapcraft`, `unsquashfs`
- Flatpak: `flatpak`, `flatpak-builder`

Tip: if CMake cannot auto-detect Qt, set `CMAKE_PREFIX_PATH` or `Qt6_DIR`.

## Build From Source

```bash
rm -rf build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/openscp_hello
```

If Qt 6 is not auto-detected:

```bash
CMAKE_PREFIX_PATH=/path/to/Qt/<version>/gcc_64 cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
# or
Qt6_DIR=/path/to/Qt/<version>/gcc_64/lib/cmake/Qt6 cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
```

## AppImage Packaging

```bash
./scripts/package_appimage.sh
```

Optional Qt path override:

```bash
CMAKE_PREFIX_PATH=/path/to/Qt/<version>/gcc_64 ./scripts/package_appimage.sh
# or
Qt6_DIR=/path/to/Qt/<version>/gcc_64/lib/cmake/Qt6 ./scripts/package_appimage.sh
```

The script now validates Qt SVG plugins (`qsvg` and `qsvgicon`) inside the AppDir before completing.

## Snap Packaging

Manifest and assets:

- `packaging/snap/snapcraft.yaml`
- `packaging/snap/openscp-snap.desktop`
- `packaging/snap/icon.png` (store/snap launcher icon, 256x256)

The Snap desktop entry uses an explicit sandbox icon path:

- `Icon=${SNAP}/meta/gui/icon.png`

Build the snap (Linux only):

```bash
./scripts/package_snap.sh
```

If your environment requires direct host builds:

```bash
./scripts/package_snap.sh --destructive-mode
```

This script:

- builds using `snapcraft`
- extracts the generated `.snap`
- validates `qsvg` and `qsvgicon` (runtime-aware when `kde-neon-6` is used)

## Flatpak Packaging

Manifest:

- `packaging/flatpak/com.openscp.OpenSCP.yml`

Build and bundle (Linux only):

```bash
./scripts/package_flatpak.sh
```

To auto-install missing SDK/runtime dependencies from Flathub during build:

```bash
./scripts/package_flatpak.sh --install-deps-from=flathub
```

To force a different Flatpak runtime branch without editing the manifest:

```bash
RUNTIME_VERSION_OVERRIDE=6.9 ./scripts/package_flatpak.sh
```

This script:

- builds the Flatpak with `flatpak-builder`
- validates `qsvg` and `qsvgicon` (runtime-aware for platform runtimes)
- creates `dist/OpenSCP.flatpak`

## Desktop Entry (General)

The generic desktop file for distro/system packaging remains:

- `assets/linux/openscp.desktop`

For local integration without packaging, copy it to:

- `~/.local/share/applications/`

and adjust `Exec=` and `Icon=` as needed.

## Manual Qt SVG Plugin Check

You can validate any packaged tree manually:

```bash
./scripts/verify_qt_svg_plugins.sh /path/to/package-root
```

Runtime-aware mode (for packages that rely on external Qt runtimes):

```bash
./scripts/verify_qt_svg_plugins.sh --allow-runtime-provided --context snap /path/to/package-root
```

## Verify Linked Libraries (Optional)

```bash
ldd ./build/openscp_hello | grep -E 'Qt6|libssh2|ssl|crypto' || true
```

## Troubleshooting

- CMake cannot find Qt 6:
  - Set `CMAKE_PREFIX_PATH` or `Qt6_DIR`.
- AppImage misses dependencies:
  - Verify `linuxdeploy` and the Qt plugin are installed and recent.
- Snap/Flatpak build fails:
  - Confirm you are building on Linux with required host tooling installed.
