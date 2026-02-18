<div align="center">
  <img src="assets/program/icon-openscp-2048.png" alt="OpenSCP icon" width="128">
  <h1 align="center">OpenSCP</h1>

<p>
  <strong>Two-panel SFTP client focused on simplicity and security</strong>
</p>

<p>
  <a href="README_ES.md"><strong>Leer en Espanol</strong></a>
</p>

<p>
  <strong>OpenSCP</strong> is a two-panel commander-style file explorer written in <strong>C++/Qt</strong>, with <strong>SFTP</strong> support (libssh2 + OpenSSL). It aims to be a lightweight alternative to tools like WinSCP, focused on <strong>security</strong>, <strong>clarity</strong>, and <strong>extensibility</strong>.
</p>

<br>

<img src="assets/screenshots/screenshot-main-window.png" alt="OpenSCP main window showing dual panels and transfer queue" width="900">

</div>

## Releases and Branches

Stable tagged releases:
https://github.com/luiscuellar31/openscp/releases

- `main`: tested and stable branch
- `dev`: active development branch (PR target)

## Quick Start

```bash
git clone https://github.com/luiscuellar31/openscp.git
cd openscp
rm -rf build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# Linux
./build/openscp_hello

# macOS
open build/OpenSCP.app
```

## What OpenSCP Offers (v0.7.0)

### 1. Dual-panel workflow

- Independent local/remote navigation.
- Drag-and-drop copy/move between panels.
- Remote context operations: download, upload, rename, delete, new folder/file, permissions.
- Clickable breadcrumbs and per-panel incremental search.

### 2. Transfer engine and queue

- Real parallel transfers with isolated worker connections.
- Pause/resume/cancel/retry, per-task/global limits, and resume support.
- Status-aware queue actions: controls are enabled only when the selected task state allows that action (for example, retry for `Error`/`Canceled`, resume for `Paused`).
- Queue UI with per-row progress bars, filters, and detailed columns (`Speed`, `ETA`, `Transferred`, `Error`, etc.).
- Context actions like retry selected, open destination, copy paths, and cleanup policies.
- Queue window/layout/filter persistence.
- Transfers use interruptible worker sessions and bounded socket read/write waits to avoid indefinite hangs during stalled network conditions.
- Upload completion path is hardened and remote views refresh reliably after finished uploads.

### 3. SFTP security hardening

- Auth: password, private key (+passphrase), keyboard-interactive (OTP/2FA), ssh-agent.
- Host-key policies: `Strict`, `Accept new (TOFU)`, `No verification` (hardened).
- Hardened no-verification flow: double confirmation, TTL-based temporary exception, risk banner.
- Atomic `known_hosts` persistence and strict POSIX permissions (`~/.ssh` 0700, file 0600).
- One-time-connect confirmation when fingerprint persistence fails.
- Safer keyboard-interactive cancel path (no accidental password fallback).
- Transfer integrity policy (`off/optional/required`) using `.part` + atomic finalize.
- Sensitive data redacted from production logs by default.

### 4. Sites and credential storage

- Saved sites use stable UUID identities.
- Duplicate site names blocked; rename/delete cleans legacy or orphan secrets.
- Optional cleanup of stored credentials and related `known_hosts` entries when deleting sites.
- Secure backends:
  - macOS: Keychain
  - Linux: libsecret (when available)
- Clear persistence feedback in secure-only builds.
- Quick Connect can save/update site data without creating duplicates.

### 5. UX/UI quality

- Connection dialog improved (clearer inputs, inline key/known_hosts selectors, show/hide password fields).
- Settings redesigned into `General` and `Advanced` sections.
- One-click reset for default main-window layout/sizes in Settings.
- Permissions dialog includes octal preview + common presets.
- About dialog includes diagnostics copy support and friendlier fallback messaging.
- Disconnect flow stays responsive: UI returns to local mode immediately while transfer cleanup can continue in background, with watchdog/status feedback.
- Reconnect is blocked while previous transfer cleanup is still running, preventing session overlap races.

### 6. Quality baseline (CI and tests)

- CI split by intent:
  - push to `dev`: fast Linux build + non-integration tests
  - PR to `main`: Linux and macOS integration gate
- Integration workflow spins up a temporary SFTP server for end-to-end checks.
- Nightly quality job includes `ASan`, `UBSan`, `TSan`, and `cppcheck`.

## Requirements

- Qt `6.x` (tested with `6.8.3`)
- libssh2 (OpenSSL 3 recommended)
- CMake `3.22+`
- C++20 compiler

Optional:

- macOS: Keychain (native)
- Linux: libsecret / Secret Service

## Testing Locally

```bash
cmake -S . -B build -DOPEN_SCP_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

`openscp_sftp_integration_tests` is skipped unless integration variables are set:

- `OPEN_SCP_IT_SFTP_HOST`
- `OPEN_SCP_IT_SFTP_PORT`
- `OPEN_SCP_IT_SFTP_USER`
- `OPEN_SCP_IT_SFTP_PASS` or `OPEN_SCP_IT_SFTP_KEY`
- `OPEN_SCP_IT_SFTP_KEY_PASSPHRASE` (if needed)
- `OPEN_SCP_IT_REMOTE_BASE`

## Platform Workflows

### macOS

Recommended daily loop:

```bash
./scripts/macos.sh dev
```

Step-by-step:

```bash
./scripts/macos.sh configure
./scripts/macos.sh build
./scripts/macos.sh run
```

Local unsigned packaging:

```bash
./scripts/macos.sh app
./scripts/macos.sh pkg
./scripts/macos.sh dmg
./scripts/macos.sh dist
```

If Qt is outside the default path (`$HOME/Qt/<version>/macos`):

```bash
export QT_PREFIX="/path/to/Qt/<version>/macos"
# or
export Qt6_DIR="/path/to/Qt/<version>/macos/lib/cmake/Qt6"
```

Full packaging details: `assets/macos/README.md`

### Linux

AppImage packaging details: `assets/linux/README.md`

## Runtime Environment Variables

- `OPEN_SCP_KNOWNHOSTS_PLAIN=1|0` - force plain vs hashed hostnames in `known_hosts`.
- `OPEN_SCP_FP_HEX_ONLY=1` - show fingerprints in HEX with `:`.
- `OPEN_SCP_TRANSFER_INTEGRITY=off|optional|required` - override transfer integrity policy.
- `OPEN_SCP_LOG_LEVEL=error|warn|info|debug` - set log verbosity.
- `OPEN_SCP_LOG_SENSITIVE=1` - enable sensitive debug details (disabled by default).
- `OPEN_SCP_ENABLE_INSECURE_FALLBACK=1` - enable insecure secret fallback only when supported by the build/platform.

## Screenshots

<p align="center">
  <img src="assets/screenshots/screenshot-site-manager.png" alt="Site Manager with saved servers" width="32%">
  <img src="assets/screenshots/screenshot-connect.png" alt="Connect dialog with authentication options" width="32%">
  <img src="assets/screenshots/screenshot-transfer-queue.png" alt="Transfer queue with progress, filters, and actions" width="32%">
</p>

## Roadmap

- Windows support is planned for future releases.
- Protocols: `SCP`, then `FTP/FTPS/WebDAV`.
- Proxy and jump support: `SOCKS5`, `HTTP CONNECT`, `ProxyJump`.
- Sync workflows: compare/sync and keep-up-to-date with filters/ignores.
- Queue persistence across restarts.
- More UX features: bookmarks, history, command palette, themes.

## Credits and Licenses

- libssh2, OpenSSL, zlib, and Qt are owned by their respective authors.
- License texts: [docs/credits/LICENSES/](docs/credits/LICENSES/)
- Qt (LGPL) materials: [docs/credits](docs/credits)

## Contributing

- Contributions are welcome. See `CONTRIBUTING.md` for workflow and standards.
- Issues and pull requests are welcome, especially around macOS/Linux stability, i18n, and SFTP robustness.
