# Linux Build & AppImage (Unsigned)

This guide explains how to build OpenSCP from source on Linux and how to package a portable AppImage for distribution.

- Binary target (from source): `build/openscp_hello`
- AppImage artifact: `dist/OpenSCP-<version>-<arch>.AppImage` and `… .sha256`
- Script: `scripts/package_appimage.sh`

## Prerequisites

- Qt 6.8.3
- libssh2 (OpenSSL 3 recommended)
- CMake 3.22+ and a C++20 compiler

For AppImage packaging, ensure these tools are available in your `PATH`:
- `linuxdeploy`
- `linuxdeploy-plugin-qt`
- `appimagetool`

Tip: If CMake cannot auto-detect Qt, point to your Qt 6 root using one of the env vars shown below.

## Build From Source

```bash
# From the repository root
rm -rf build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# Run
./build/openscp_hello
```

If Qt 6 is not auto‑detected, set one of:

```bash
CMAKE_PREFIX_PATH=/path/to/Qt/6.8.3/gcc_64 cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
# or
Qt6_DIR=/path/to/Qt/6.8.3/gcc_64/lib/cmake/Qt6 cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
```

## AppImage Packaging (Unsigned)

Produce a portable AppImage suitable for most Linux distributions:

```bash
# From the repository root
./scripts/package_appimage.sh
```

If Qt 6 is not auto‑detected by the script, provide the path:

```bash
CMAKE_PREFIX_PATH=/path/to/Qt/6.8.3/gcc_64 ./scripts/package_appimage.sh
# or
Qt6_DIR=/path/to/Qt/6.8.3/gcc_64/lib/cmake/Qt6 ./scripts/package_appimage.sh
```

Output:
- `dist/OpenSCP-<version>-<arch>.AppImage`
- `dist/OpenSCP-<version>-<arch>.AppImage.sha256`

## Desktop Entry (optional)

A launcher file is provided at `assets/linux/openscp.desktop`. Distros may use this file during packaging.
For local integration without packaging, you can copy it to `~/.local/share/applications/` and adjust `Exec=`/`Icon=` as needed.

## Verify Linked Libraries (optional)

You can check the runtime linkage of the built binary:

```bash
ldd ./build/openscp_hello | grep -E 'Qt6|libssh2|ssl|crypto' || true
```

## Troubleshooting

- CMake can’t find Qt 6:
  - Set `CMAKE_PREFIX_PATH` or `Qt6_DIR` to your Qt 6 installation.
- Qt plugin for linuxdeploy not found:
  - Ensure `linuxdeploy-plugin-qt` is in your `PATH`.
- AppImage missing dependencies:
  - The script uses linuxdeploy (+Qt plugin) to bundle Qt and other runtime libs into the AppDir; verify tool versions.

