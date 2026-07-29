# Scripts Index

Minimal map of repository helper scripts.

## CI and Release

| Script | Purpose | Details |
| --- | --- | --- |
| [`check_ci_local.sh`](./check_ci_local.sh) | Local pre-push CI check (configure, build every configured test binary, run CTest, optional full app build). | Usage in [README.md](../README.md#testing-locally) and [README_ES.md](../README_ES.md#probar-localmente). |
| [`ci/setup_protocol_services.sh`](./ci/setup_protocol_services.sh) | Starts isolated FTP, explicit/implicit FTPS, and HTTPS WebDAV services on Ubuntu. | Generates an ephemeral localhost CA/certificate and serves WebDAV at `/openscp-dav`. |
| [`ci/run_protocol_integration.sh`](./ci/run_protocol_integration.sh) | Runs the FTP, both FTPS modes, and WebDAV integration binaries directly. | A missing binary or unavailable service is a hard failure rather than a CTest skip. |
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

## Recommended macOS Flow

For day-to-day development, configure, build, and launch with one command:

```bash
./scripts/macos.sh dev
```

After producing the local unsigned packaged app, validate that its Qt
frameworks, plugins, and linkage are self-contained:

```bash
./scripts/macos.sh app
./scripts/verify_macos_bundle.sh build/OpenSCP.app
```

Signing and notarization are not required for this local development and
verification flow.

The advanced packager sources focused helpers from `scripts/macos/`:
`package_signing.sh` owns Developer ID/ad-hoc signing and
`package_artifacts.sh` owns ZIP/PKG/DMG creation plus notarization.
