# Building and Packaging OpenSCP

Run all commands from the repository root. OpenSCP requires CMake 3.22+, a
C++20 compiler, Qt 6 Widgets/SVG/Linguist Tools, libssh2, and OpenSSL. libcurl
enables FTP/FTPS; tinyxml2 plus libcurl enables WebDAV.

Official artifact baselines and protocol differences are documented in
[PLATFORM_COMPATIBILITY.md](PLATFORM_COMPATIBILITY.md).

## Linux

### Dependencies

Ubuntu 22.04:

```bash
sudo apt-get update
sudo apt-get install -y \
  cmake g++ ninja-build pkg-config \
  libssh2-1-dev libssl-dev libcurl4-openssl-dev libtinyxml2-dev \
  libsecret-1-dev libgl1-mesa-dev \
  qt6-base-dev libqt6svg6-dev qt6-tools-dev qt6-tools-dev-tools \
  qt6-l10n-tools
```

Ubuntu 24.04 uses `qt6-svg-dev` instead of `libqt6svg6-dev`:

```bash
sudo apt-get update
sudo apt-get install -y \
  cmake g++ ninja-build pkg-config \
  libssh2-1-dev libssl-dev libcurl4-openssl-dev libtinyxml2-dev \
  libsecret-1-dev libgl1-mesa-dev \
  qt6-base-dev qt6-svg-dev qt6-tools-dev qt6-tools-dev-tools \
  qt6-l10n-tools
```

If libcurl or tinyxml2 is absent, CMake reports which optional protocols were
disabled.

### Development build

```bash
./scripts/linux.sh dev
```

The same entrypoint exposes individual steps:

```bash
./scripts/linux.sh configure
./scripts/linux.sh build
./scripts/linux.sh run
```

Use `BUILD_TYPE=Debug`, `JOBS=<number>`, `CMAKE_PREFIX_PATH`, or `Qt6_DIR` when
the defaults are unsuitable. Run `./scripts/linux.sh help` for the complete
interface.

### Linux packages

Install each format's own host tools before running its command:

```bash
# linuxdeploy, linuxdeploy-plugin-qt, and appimagetool
./scripts/package/appimage.sh

# snapcraft, unsquashfs, and file
./scripts/package/snap.sh

# flatpak, flatpak-builder, and file
./scripts/package/flatpak.sh
```

AppImage uses `build-appimage/` by default so its release-oriented Qt and
CMake configuration cannot alter the development cache in `build/`. Set
`BUILD_DIR=/path/to/build` only when a different isolated directory is needed.

The packagers validate the required Qt SVG plugins. Official AppImages also
audit the bundled ELF ABI; the check can be run directly against an AppDir:

```bash
./scripts/verify/linux-abi.sh dist/OpenSCP.AppDir 2.28
```

Snap and Flatpak intentionally build only SFTP and SCP. AppImage release builds
include all five protocols. See each script's `--help` output for runtime,
architecture, and dependency overrides.

## macOS

### Dependencies

Use Qt 6 from the official installer rather than Conda. The scripts first use
`QT_PREFIX` or `Qt6_DIR`, then search `$HOME/Qt/<version>/macos`.

For a local build, Homebrew can provide the non-Qt dependencies:

```bash
brew install libssh2 openssl@3 tinyxml2
```

Official artifacts instead build pinned dependencies through
`scripts/ci/build_macos_release_deps.sh` so every bundled library shares the
macOS deployment target.

### Development build

```bash
./scripts/macos.sh dev
```

Or run the steps independently:

```bash
./scripts/macos.sh configure
./scripts/macos.sh build
./scripts/macos.sh run
```

The application bundle is written to `build/OpenSCP.app`. If Qt cannot be
detected, set one of:

```bash
export QT_PREFIX=/path/to/Qt/<version>/macos
export Qt6_DIR=/path/to/Qt/<version>/macos/lib/cmake/Qt6
```

### macOS packages

Local unsigned artifacts are generated with:

```bash
./scripts/macos.sh app   # zipped OpenSCP.app
./scripts/macos.sh pkg
./scripts/macos.sh dmg
./scripts/macos.sh dist  # all formats
```

Outputs and their `.sha256` files are written under `dist/`. The packaging
flow runs `macdeployqt`, bundles non-Qt dependencies, rewrites linkage, and
verifies the completed bundle.

By default it builds the current architecture. Set
`CMAKE_OSX_ARCHITECTURES=x86_64` for Intel or
`CMAKE_OSX_ARCHITECTURES='arm64;x86_64'` for a universal build; Qt and every
dependency must support the selected architectures.

Signing and notarization are optional. The advanced entrypoint
`scripts/package/macos.sh --help` documents `APPLE_IDENTITY`, Apple API-key
variables, and the skip controls used for local unsigned packages.

To inspect linkage manually:

```bash
otool -L build/OpenSCP.app/Contents/MacOS/OpenSCP | \
  grep -E 'libssh2|libcrypto|libssl|tinyxml2|@executable_path'
```

Bundled references should resolve through `@executable_path/../Frameworks`.

## Tests and quality checks

The standard local CI command configures all available test targets, builds
them, and runs CTest:

```bash
./scripts/check_ci_local.sh --clean --full --werror
```

Individual checks remain available when diagnosing a failure:

```bash
./scripts/checks/cpp-quality.sh --format
./scripts/checks/cpp-quality.sh --tidy --cppcheck \
  --build-dir build-ci-local
./scripts/checks/shell-quality.sh
```

## Protocol integration services

`scripts/ci/run_protocol_integration.sh` executes already-built FTP, explicit
FTPS, implicit FTPS, and HTTPS WebDAV test binaries against a prepared service
environment.

> [!WARNING]
> `scripts/ci/setup_protocol_services.sh` is a Linux CI helper for a disposable
> Ubuntu VM or dedicated ephemeral runner. It uses `sudo`, creates or reuses the
> `openscp_protocol_it` system user and resets its password, enables Apache
> modules under `/etc/apache2`, starts FTP/FTPS/WebDAV services, and creates
> root-owned state under `/tmp/openscp-protocol-it`. It does not stop those
> services, remove the user, or undo the Apache changes. Never run it on a
> normal workstation or shared host.

In an appropriate disposable environment:

```bash
./scripts/ci/setup_protocol_services.sh
./scripts/ci/run_protocol_integration.sh build
```

## Troubleshooting

- Qt is not found: set `CMAKE_PREFIX_PATH` or `Qt6_DIR` to one Qt 6 install.
- macOS selects Conda tools: set `QT_PREFIX` explicitly and remove conflicting
  `QT_PLUGIN_PATH`/`DYLD_*` values from the calling environment.
- A macOS dependency requires a newer system: rebuild it with the intended
  `MACOSX_DEPLOYMENT_TARGET`; the verifier correctly rejects mixed baselines.
- An AppImage misses a library or plugin: verify that recent `linuxdeploy`, its
  Qt plugin, and `appimagetool` are all available.
- Snap or Flatpak fails before compilation: install the declared runtime/SDK
  and confirm the build is running on Linux.
