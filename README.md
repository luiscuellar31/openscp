<div align="center">
    <img src="assets/program/icon-openscp-2048.png" alt="OpenSCP icon" width="128">
    <h1 align="center">OpenSCP</h1>

<p>
    <strong>Two-panel SFTP/SCP/FTP/FTPS client focused on simplicity and security</strong>
</p>

<p>
    <a href="README_ES.md"><strong>Leer en Español</strong></a>
</p>

<p>
    <strong>OpenSCP</strong> is a two-panel commander-style file explorer written in <strong>C++/Qt</strong>, with <strong>SFTP</strong>, initial <strong>SCP</strong>, and initial <strong>FTP/FTPS</strong> support. It aims to be a lightweight alternative to tools like WinSCP, focused on <strong>security</strong>, <strong>clarity</strong>, and <strong>extensibility</strong>.
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

## What OpenSCP Offers (v0.9.0)

### 1. Dual-panel workflow

- Independent local/remote navigation.
- Quick `Home` navigation in panel toolbars (left local panel always; right panel uses local `HOME` in local mode and `/` fallback in remote mode).
- Right panel includes `Open in terminal` in remote mode to start an SSH terminal directly in the currently viewed remote path using the active transport settings (direct, proxy tunnel, or jump host); if the SSH shell fails with a session error (for example PTY denied), it automatically falls back to `sftp` CLI in the same terminal. If transport requirements cannot be reproduced safely, the app shows an explicit error instead of downgrading to a basic direct SSH fallback. In `Settings > Security`, you can force interactive login (password/keyboard-interactive) and toggle automatic `sftp` CLI fallback for these commands.
- Drag-and-drop copy/move between panels.
- Remote context operations: download, upload, rename, delete, new folder/file, permissions.
- Clickable breadcrumbs and per-panel search (toolbar button or `Ctrl/Cmd+F`) with wildcard/regex patterns and optional recursive mode.
- Remote panel icons use MIME-based detection (plus native provider on macOS) for closer parity with local icons.

### 2. Transfer engine and queue

- Real parallel transfers with isolated worker connections.
- Expensive queue prechecks run off the UI thread; scheduling fairness and queue metrics reduce starvation under high concurrency.
- Pause/resume/cancel/retry, per-task/global limits, and resume support.
- Status-aware queue actions: controls are enabled only when the selected task state allows that action (for example, retry for `Error`/`Canceled`, resume for `Paused`).
- Queue UI with per-row progress percentages, filters, and detailed columns (`Speed`, `ETA`, `Transferred`, `Error`, etc.).
- Context actions like retry selected, open destination, copy paths, and cleanup policies.
- Queue window/layout/filter persistence.
- Main status bar emits transfer completion notices (for successful uploads/downloads).
- Transfers use interruptible worker sessions and bounded socket read/write waits to avoid indefinite hangs during stalled network conditions.
- Upload completion path is hardened and remote views refresh reliably after finished uploads.
- Critical remote operations now attempt one automatic stale-session recovery (reconnect + retry) before failing.
- Main remote session health checks run periodically and after wake/resume; if transport is no longer valid, OpenSCP disconnects safely with a clear warning.

### 3. SSH transport security hardening

- Auth: password, private key (+passphrase), keyboard-interactive (OTP/2FA), ssh-agent.
- Protocol selector per site/session (`SFTP`, `SCP`, `FTP`, `FTPS`).
- FTP/FTPS currently run in transfer-only mode (no remote listing panel).
- SCP mode policy per site/session: `Automatic (SCP + SFTP fallback)` or
  `SCP only` (disable fallback), plus a global default for new connections.
- FTPS certificate verification (peer+host) is enabled by default, with optional custom CA bundle per site/session.
- Host-key policies: `Strict`, `Accept new (TOFU)`, `No verification` (hardened).
- Per-site transport can use direct TCP, `SOCKS5`, or `HTTP CONNECT` proxy tunneling.
- Per-site SSH jump host (`ProxyJump`/bastion) tunneling is supported.
- Current implementation treats proxy tunneling and jump host tunneling as mutually exclusive per session.
- Hardened no-verification flow: double confirmation, TTL-based temporary exception, risk banner.
- Atomic `known_hosts` persistence and strict POSIX permissions (`~/.ssh` 0700, file 0600).
- One-time-connect confirmation when fingerprint persistence fails.
- Safer keyboard-interactive cancel path (no accidental password fallback).
- Transfer integrity policy (`off/optional/required`) per site/session (and env override) using `.part` + atomic finalize.
- Sensitive data redacted from production logs by default.

### 4. Sites and credential storage

- Saved sites use stable UUID identities.
- Saved sites persist proxy type/endpoint/username per site.
- Saved sites persist SSH jump host settings (host/port/user/key path) per site.
- Saved sites persist SCP mode policy per site.
- Saved sites persist FTPS certificate settings (verify toggle and optional CA bundle path) per site.
- Duplicate site names blocked; rename/delete cleans legacy or orphan secrets.
- Optional cleanup of stored credentials and related `known_hosts` entries when deleting sites.
- Secure backends:
    - macOS: Keychain
    - Linux: libsecret (when available)
- Proxy passwords are stored in the secure backend (never in plaintext site settings).
- Clear persistence feedback in secure-only builds.
- Quick Connect can save/update site data without creating duplicates.

### 5. UX/UI quality

- Connection dialog improved (clearer inputs, inline key/known_hosts selectors, show/hide password fields).
- Connection dialog includes per-site proxy configuration (`Direct`, `SOCKS5`, `HTTP CONNECT`) with optional auth.
- Connection dialog includes optional per-site SSH jump host (bastion) configuration.
- Connection dialog includes FTPS certificate controls (verify toggle + optional CA bundle selector).
- UI language selection includes `English`, `Spanish`, and `Portuguese`.
- Settings redesigned into focused sections: `General`, `Transfers`, `Sites`, `Security`, `Network`, and `Staging and drag-out`.
- Settings keeps controls visible while resizing (minimum size + scrollable pages).
- One-click reset for default main-window layout/sizes in Settings.
- Permissions dialog includes octal preview + common presets.
- About dialog includes diagnostics copy support and friendlier fallback messaging.
- Transfer queue dialog opens centered relative to the main window.
- Status bar shows connection type and per-session elapsed connection time.
- Disconnect flow stays responsive: UI returns to local mode immediately while transfer cleanup can continue in background, with watchdog/status feedback.
- Reconnect is blocked while previous transfer cleanup is still running, preventing session overlap races.

### 6. Quality baseline (CI and tests)

- CI split by intent:
    - push to `dev`: fast Linux build + non-integration tests
    - PR to `main`: Linux and macOS integration gate
- Integration workflow spins up a temporary SFTP server for end-to-end checks.
- PR integration coverage validates transport variants in CI: direct, `SOCKS5` proxy tunnel, `HTTP CONNECT` proxy tunnel (with auth), and SSH jump host tunnel.
- Tag release workflow auto-generates draft release notes from Conventional Commits (`feat`, `fix`, `BREAKING CHANGE`, etc.).
- Nightly quality job includes `ASan`, `UBSan`, `TSan`, and `cppcheck`.

## Requirements

- Qt `6.x` (tested with `6.8.3`)
- libssh2 (OpenSSL 3 recommended)
- libcurl (optional; required only for FTP/FTPS backends)
- CMake `3.22+`
- C++20 compiler

Optional:

- macOS: Keychain (native)
- Linux: libsecret / Secret Service
- OpenSSH client (`ssh`) for SSH jump host tunneling.
- FTP/FTPS backend can be disabled explicitly with
  `-DOPEN_SCP_ENABLE_FTP_BACKEND=OFF`.

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
- `OPEN_SCP_IT_PROXY_TYPE` (`socks5` or `http`, optional)
- `OPEN_SCP_IT_PROXY_HOST` (required when `OPEN_SCP_IT_PROXY_TYPE` is set)
- `OPEN_SCP_IT_PROXY_PORT` (optional; defaults: `1080` for `socks5`, `8080` for `http`)
- `OPEN_SCP_IT_PROXY_USER` (optional)
- `OPEN_SCP_IT_PROXY_PASS` (optional)
- `OPEN_SCP_IT_JUMP_HOST` (optional)
- `OPEN_SCP_IT_JUMP_PORT` (optional; default `22`)
- `OPEN_SCP_IT_JUMP_USER` (optional)
- `OPEN_SCP_IT_JUMP_KEY` (optional)

`openscp_ftp_integration_tests` is skipped unless integration variables are set:

- `OPEN_SCP_IT_FTP_HOST`
- `OPEN_SCP_IT_FTP_PORT` (optional; default `21`)
- `OPEN_SCP_IT_FTP_USER` (optional)
- `OPEN_SCP_IT_FTP_PASS` (optional)
- `OPEN_SCP_IT_FTP_REMOTE_BASE`

`openscp_ftps_integration_tests` is skipped unless integration variables are set:

- `OPEN_SCP_IT_FTPS_HOST`
- `OPEN_SCP_IT_FTPS_PORT` (optional; default `990`)
- `OPEN_SCP_IT_FTPS_USER` (optional)
- `OPEN_SCP_IT_FTPS_PASS` (optional)
- `OPEN_SCP_IT_FTPS_REMOTE_BASE`
- `OPEN_SCP_IT_FTPS_VERIFY_PEER` (`1`/`0`, optional; default `1`)
- `OPEN_SCP_IT_FTPS_CA_CERT` (optional)

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

Full packaging details: [assets/macos/README.md](assets/macos/README.md)

### Linux

Linux build and packaging details (AppImage, Snap, Flatpak): [assets/linux/README.md](assets/linux/README.md)

## Runtime Environment Variables

- `OPEN_SCP_KNOWNHOSTS_PLAIN=1|0` - force plain vs hashed hostnames in `known_hosts`.
- `OPEN_SCP_FP_HEX_ONLY=1` - show fingerprints in HEX with `:`.
- `OPEN_SCP_TRANSFER_INTEGRITY=off|optional|required` - override transfer integrity policy.
- `OPEN_SCP_LOG_LEVEL=off|error|warn|info|debug` - set log verbosity.
- `OPEN_SCP_ENV=dev|prod` - runtime environment selector (`dev` enables development-only diagnostics).
- `OPEN_SCP_LOG_SENSITIVE=1` - enable sensitive debug details only when `OPEN_SCP_ENV=dev` (disabled by default).
- `OPEN_SCP_ENABLE_INSECURE_FALLBACK=1` - enable insecure secret fallback only when supported by the build/platform.

## Screenshots

<p align="center">
    <img src="assets/screenshots/screenshot-site-manager.png" alt="Site Manager with saved servers" width="32%">
    <img src="assets/screenshots/screenshot-connect.png" alt="Connect dialog with authentication options" width="32%">
    <img src="assets/screenshots/screenshot-transfer-queue.png" alt="Transfer queue with progress, filters, and actions" width="32%">
</p>

## Roadmap

- Windows support is planned for future releases.
- Protocols: `WebDAV`.
- Broader enterprise proxy/jump auth flows (for example, non-batch/interactive jump auth).
- Sync workflows: compare/sync and keep-up-to-date with filters/ignores.
- Queue persistence across restarts.
- More UX features: bookmarks, history, command palette, themes.

## Credits and Licenses

- libssh2, libcurl, OpenSSL, zlib, and Qt are owned by their respective authors.
- License texts: [docs/credits/LICENSES/](docs/credits/LICENSES/)
- Qt (LGPL) materials: [docs/credits](docs/credits)

## Contributing

- Contributions are welcome. See [CONTRIBUTING.md](CONTRIBUTING.md) for workflow and standards.
- Issues and pull requests are welcome, especially around macOS/Linux stability, i18n, and SFTP/SCP/FTP/FTPS robustness.
