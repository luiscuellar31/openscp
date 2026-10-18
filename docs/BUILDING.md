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
The build scripts configure a new build directory with Ninja when it is
installed, and run as many compile jobs as there are CPUs; set `JOBS` to
change that number.

The AppImage packager checks that the bundled Qt SVG and platform plugins and
their libraries resolve, and that the packaged executable keeps its ELF
hardening (PIE, RELRO, immediate binding, non-executable stack). Snap and
Flatpak take Qt from their runtime, so they have no Qt plugins of their own to
check. Official AppImages also audit the bundled ELF ABI; the check can be run
directly against an AppDir:

```bash
./scripts/verify/linux-abi.sh dist/OpenSCP.AppDir 2.34
```

AppImages bundle `libstdc++.so.6` and use the target system's
`libgcc_s.so.1` to preserve the stated GLIBC compatibility floor.

Linux release CI pins Python 3.14.7 for the Qt installer because older Python
versions can misidentify the ARM `qttools` 7z archive as a ZIP. AppImage tools
use `APPIMAGE_EXTRACT_AND_RUN=1` on headless builders; the packager removes it
only when querying the finished image's update metadata, which needs no FUSE
and must not launch the application.

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
verifies the completed bundle. It builds in `build-macos-package/`, without
tests or demo mode, so the development cache in `build/` cannot change a
package; set `BUILD_DIR` to use another isolated directory.

Plugin scanning is disabled by default because the required macOS platform,
SVG icon, and style plugins are staged explicitly. Set
`MACDEPLOYQT_DISABLE_PLUGIN_SCAN=0` only for custom builds that need Qt's full
plugin scan.

The release workflow also rejects development-only Qt framework content,
unreachable Mach-O files, and bundles larger than its configured
`MAX_BUNDLE_SIZE_MIB` budget.

By default it builds the current architecture. Set
`CMAKE_OSX_ARCHITECTURES=x86_64` for Intel or
`CMAKE_OSX_ARCHITECTURES='arm64;x86_64'` for a universal build; Qt and every
dependency must support the selected architectures.

Signing and notarization are optional. The advanced entrypoint
`scripts/package/macos.sh --help` documents `APPLE_IDENTITY`, Apple API-key
variables, and the skip controls used for local unsigned packages.

To inspect linkage manually:

```bash
otool -L build-macos-package/OpenSCP.app/Contents/MacOS/OpenSCP | \
  grep -E 'libssh2|libcrypto|libssl|tinyxml2|@executable_path'
```

Bundled references should resolve through `@executable_path/../Frameworks`.

## Windows

### Dependencies

Windows 10 version 1809 or later, or Windows 11, on x64, with:

- Visual Studio 2022 or its Build Tools with the **Desktop development with
  C++** workload, which brings MSVC, the Windows SDK, CMake, and Ninja;
- Qt 6 for MSVC 2022 64-bit from the Qt installer, with Qt SVG and the Qt
  Linguist tools; official artifacts use Qt 6.11.2; and
- a clone of [vcpkg](https://github.com/microsoft/vcpkg). On the first
  configure, `vcpkg.json` makes it build libssh2, OpenSSL, libcurl, and
  tinyxml2 as static libraries for the `x64-windows-static-md` triplet, at the
  versions of its `builtin-baseline`. OpenSSL comes instead from the overlay
  port in `vcpkg-ports/openssl`, which keeps it on the 3.5 long-term support
  release that the macOS release builds use; its README explains how to move
  it to a newer one.

The scripts enter the Visual Studio developer environment themselves. They
take Qt from `QT_PREFIX` or `Qt6_DIR`, otherwise from the newest
`C:\Qt\<version>\msvc2022_64`, and need `VCPKG_ROOT`:

```powershell
$env:VCPKG_ROOT = 'C:\path\to\vcpkg'
$env:QT_PREFIX = 'C:\Qt\6.11.2\msvc2022_64'
```

### Development build

```powershell
.\scripts\windows.ps1 dev
```

Or run the steps independently:

```powershell
.\scripts\windows.ps1 configure
.\scripts\windows.ps1 build
.\scripts\windows.ps1 run
```

The executable is written to `build\OpenSCP.exe`. The build tree has no
deployed Qt, so `run` and the tests take Qt's DLLs from the Qt installation.
Settings go to INI files under `%APPDATA%\OpenSCP`, not to the registry.

### Windows packages

Packaging also needs Inno Setup 6 and WiX v5, which installs as a .NET tool:

```powershell
dotnet tool install --global wix --version 5.0.2
.\scripts\package\windows.ps1
```

The script runs `wix` from `PATH`, or the command in `OPENSCP_WIX`. It does not
read `WIX`, which the WiX v3 installer sets to its own folder.

It builds in `build-windows-package\`, stages the application in
`build-windows-package\stage\OpenSCP` with `windeployqt`, the Qt translations,
the Visual C++ runtime DLLs it imports, and the licenses, and checks that
folder with `scripts\verify\windows-package.ps1`: every binary must be x64
with ASLR, high-entropy ASLR, and DEP, `OpenSCP.exe` also with Control Flow
Guard and CET, and every imported DLL must be staged or part of Windows. It
then writes `dist\OpenSCP-<version>-x64-setup.exe` and
`dist\OpenSCP-<version>-x64.msi` with their `.sha256` files. Set
`PACKAGE_FORMATS=setup` or `msi` to build one; `.\scripts\package\windows.ps1
help` lists the other variables. Pull requests that change a PowerShell script
or the Windows packaging run it in the **Windows Packaging** workflow.

The setup installs for the current user by default and can install for all
users; it is the one that updates itself from the signed update manifest. The
MSI installs for all users, for deployment tools. For all users, both install
into the same folder, so neither installs there while the other is installed.
Neither is code-signed.

## Application icon

The icon is drawn by
[`scripts/icons/generate-icon.swift`](../scripts/icons/generate-icon.swift)
from two public-domain Tango drawings, in three styles: `standard`, the
free-standing drawing, and `macos-light` and `macos-dark`, the drawing on
Apple's icon shape. It writes each style's 1024-pixel image to
`assets/icons/app/<style>/openscp-1024.png`, in sRGB.

The sizes derived from those images, from 16 to 512 pixels, are committed with
them, as are the macOS bundle icon `assets/macos/OpenSCP.icns`, made from
`macos-light`, the Windows icon `assets/windows/OpenSCP.ico`, made from
`standard`, and the snap icon `packaging/snap/icon.png`. Ordinary builds
therefore need no image tools. After changing the artwork, regenerate them on
macOS with Swift, Python 3, and the system `sips` and `iconutil` tools:

```bash
swift scripts/icons/generate-icon.swift
python3 scripts/icons/generate-icon-sizes.py
```

The application embeds every style and shows the one chosen under
**Settings → General → Application icon**: `macos-light` by default on macOS
and `standard` elsewhere. Installed icons cannot change while the application
runs, so Finder, the Dock while OpenSCP is closed, Linux launchers, and Windows
Explorer keep the platform default; Linux packages install the `standard`
sizes in the hicolor theme.

## Tests and quality checks

The standard local CI command configures all available test targets, builds
them, and runs CTest:

```bash
./scripts/check_ci_local.sh --clean --full --werror
```

On Windows, `.\scripts\windows.ps1 test` configures the tests, builds them,
and runs CTest; with `OPENSCP_WERROR=ON` it treats warnings as errors, as the
Windows CI job does. That job also runs
`.\scripts\checks\powershell-quality.ps1`, which parses the PowerShell
scripts, tries the help of the user-facing ones, and reports PSScriptAnalyzer's
errors when PSScriptAnalyzer is installed;
`.\scripts\ci\windows_sftp_integration.ps1`, which runs the SFTP integration
test against a local paramiko server, directly and through a jump host; and
`scripts/ci/windows_clang_tidy.sh`, which runs `cpp-quality.sh --tidy` from
Git Bash on the files that have Windows code, with the build's MSVC
compilation database. The SFTP script adds the jump host to `known_hosts`
only on a GitHub runner, or in the file given with `-KnownHosts`.

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
