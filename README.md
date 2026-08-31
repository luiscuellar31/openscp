<div align="center">
  <img src="assets/program/icon-openscp-2048.png" alt="OpenSCP icon" width="128">
  <h1>OpenSCP</h1>

  <p><strong>A lightweight, cross-platform file transfer client inspired by WinSCP.</strong></p>
  <p><a href="README_ES.md">Leer en español</a></p>

  <img src="assets/screenshots/screenshot-main-window.png" alt="OpenSCP main window" width="900">
</div>

OpenSCP is a C++20 and Qt 6 desktop application for moving and managing files
between local and remote systems. It focuses on predictable behavior, secure
defaults, and a familiar commander-style workflow.

## Quick start

OpenSCP currently supports Linux and macOS. A compatible Qt 6 installation,
CMake 3.22+, libssh2, and OpenSSL are required. libcurl enables optional FTP
and FTPS support; WebDAV also requires tinyxml2.

```bash
git clone https://github.com/luiscuellar31/openscp.git
cd openscp

# Linux
./scripts/linux.sh dev

# macOS
./scripts/macos.sh dev
```

For distribution-specific dependencies, manual build steps, packaging, and
troubleshooting, see [Building OpenSCP](docs/BUILDING.md).

## Highlights

- Local/remote dual-panel navigation with breadcrumbs, history, favorites,
  search, and drag-and-drop.
- SFTP and SCP through libssh2; optional FTP, FTPS, and WebDAV through libcurl.
- A persistent transfer queue with parallel workers, pause, resume, retry,
  conflict policies, bandwidth limits, and resumable `.part` files.
- Saved sites with Keychain on macOS and Secret Service/libsecret on Linux.
- Strict, accept-new, or explicitly disabled SSH host-key verification.
- SOCKS5, HTTP CONNECT, and SSH jump-host support where the protocol allows it.
- One-way synchronization with preview, filters, and optional checksum checks.
- Spanish, French, and Portuguese interfaces in addition to English.

Protocol availability depends on how the application was packaged. Consult the
[protocol matrix](docs/PLATFORM_COMPATIBILITY.md#protocol-availability-by-build)
before selecting an artifact.

## Documentation

- [Building and packaging](docs/BUILDING.md)
- [Contributing and translations](CONTRIBUTING.md)
- [Architecture](docs/ARCHITECTURE.md)
- [Platform and protocol compatibility](docs/PLATFORM_COMPATIBILITY.md)
- [Security policy](SECURITY.md)
- [Licensing](docs/LICENSING.md)

## Runtime diagnostics

These optional environment variables are useful when diagnosing a problem:

- `OPENSCP_LOG_LEVEL=off|error|warn|info|debug`
- `OPENSCP_TRANSFER_INTEGRITY=off|optional|required`
- `OPENSCP_KNOWNHOSTS_PLAIN=1|0`
- `OPENSCP_FP_HEX_ONLY=1`
- `OPENSCP_ENV=dev|prod` selects the runtime environment
- `OPENSCP_LOG_SENSITIVE=1` permits sensitive diagnostic details only together
  with `OPENSCP_ENV=dev`
- `OPENSCP_ENABLE_INSECURE_FALLBACK=1` only when the build permits it

Sensitive logging is disabled by default and should only be used temporarily in
a controlled development environment.

## More screenshots

<p align="center">
  <img src="assets/screenshots/screenshot-site-manager.png" alt="Saved sites" width="32%">
  <img src="assets/screenshots/screenshot-connect.png" alt="Connection dialog" width="32%">
  <img src="assets/screenshots/screenshot-transfer-queue.png" alt="Transfer queue" width="32%">
</p>

<p align="center">
  <img src="assets/screenshots/screenshot-history.png" alt="Navigation history" width="40%">
  <img src="assets/screenshots/screenshot-settings.png" alt="Application settings" width="40%">
</p>

## Roadmap

- Finish and validate Windows support; the current Windows code is still
  experimental.
- Test WebDAV with a wider range of servers.
- Support interactive authentication and more enterprise proxy and SSH
  jump-host setups.
- Add a command palette and selectable themes.

## Releases and contributions

Tagged releases are published on the
[GitHub Releases page](https://github.com/luiscuellar31/openscp/releases).
`main` contains stable work and `dev` is the pull-request target.

Contributions are welcome. Read [CONTRIBUTING.md](CONTRIBUTING.md) before
opening a pull request. Report vulnerabilities privately as described in
[SECURITY.md](SECURITY.md).

OpenSCP is available under GPLv3-only or a commercial license. Third-party
components retain their own licenses; see [Licensing](docs/LICENSING.md) and
[third-party credits](docs/credits/CREDITS.md).
