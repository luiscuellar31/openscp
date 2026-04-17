# Scripts Index

Minimal map of repository helper scripts.

## CI and Release

| Script | Purpose | Details |
| --- | --- | --- |
| [`check_ci_local.sh`](./check_ci_local.sh) | Local pre-push CI check (configure, build, test, optional full app build). | Usage in [README.md](../README.md#testing-locally) and [README_ES.md](../README_ES.md#probar-localmente). |
| [`generate_release_notes.sh`](./generate_release_notes.sh) | Generates release notes from Conventional Commits. | Used by release workflow tooling. |

## Platform Scripts

| Script | Purpose | Platform docs |
| --- | --- | --- |
| [`macos.sh`](./macos.sh) | Daily macOS configure/build/run/packaging wrapper. | [assets/macos/README.md](../assets/macos/README.md) |
| [`package_mac.sh`](./package_mac.sh) | Advanced macOS packaging/sign/notarization flow. | [assets/macos/README.md](../assets/macos/README.md) |
| [`package_appimage.sh`](./package_appimage.sh) | Linux AppImage packaging helper. | [assets/linux/README.md](../assets/linux/README.md) |
| [`package_snap.sh`](./package_snap.sh) | Linux Snap packaging helper. | [assets/linux/README.md](../assets/linux/README.md) |
| [`package_flatpak.sh`](./package_flatpak.sh) | Linux Flatpak packaging helper. | [assets/linux/README.md](../assets/linux/README.md) |

## Validation Utilities

| Script | Purpose |
| --- | --- |
| [`verify_qt_svg_plugins.sh`](./verify_qt_svg_plugins.sh) | Validates required Qt SVG plugins in packaged trees. |
| [`verify_macos_bundle.sh`](./verify_macos_bundle.sh) | Validates macOS app bundle linkage/layout. |
