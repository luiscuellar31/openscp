<div align="center">
  <img src="assets/program/icon-openscp-2048.png" alt="OpenSCP icon" width="128">
  <h1 align="center">OpenSCP</h1>

<p>
  <strong>Two-panel SFTP client focused on simplicity and security</strong>
</p>

<p>
  <a href="README_ES.md"><strong>Leer en Español</strong></a>
</p>

<p>
  <strong>OpenSCP</strong> is a <em>two-panel commander</em>-style file explorer written in <strong>C++/Qt</strong>, with <strong>SFTP</strong> support (libssh2 + OpenSSL). It aims to be a lightweight alternative to tools like WinSCP, focused on <strong>security</strong>, <strong>clarity</strong>, and <strong>extensibility</strong>.
</p>

<br>

<img src="assets/screenshots/screenshot-main-window.png" alt="OpenSCP main window showing dual panels and transfer queue" width="900">

</div>

## Releases

Looking for a fixed/stable build? Download tagged releases:
https://github.com/luiscuellar31/openscp/releases

- `main`: tested, stable code (moves between releases)
- `dev`: active development (PRs should target `dev`)

## Current Features (v0.7.0)

### Dual panels (local <-> remote)

- Independent navigation on both sides.
- Drag-and-drop between panels for copy/move workflows.
- Remote context actions: Download, Upload, Rename, Delete, New Folder, Change Permissions (recursive).
- Sortable columns and responsive resizing.

### Transfer engine and queue

- True concurrent transfers with isolated worker connections (parallel workers, no single global transfer lock).
- Resume support, pause/resume/cancel/retry, per-task and global speed limits.
- Queue UI upgraded with model-based updates (no full table rebuild on every refresh).
- Rich per-task columns: Type, Name, Source, Destination, Status, Progress, Transferred, Speed, ETA, Attempts, Error.
- Per-row progress bars and quick filters: `All / Active / Errors / Completed / Canceled`.
- Context actions: pause/resume/limit/cancel/retry selected, open destination, copy source/destination path, clear finished.
- Summary badges for total/active/running/paused/errors/completed/canceled + global limit.
- Auto-clear policies for finished tasks (completed, failed/canceled, or all finished after N minutes).
- Persists queue window geometry, column layout/order, and active filter.

### Remote -> system drag-and-drop (asynchronous)

- Fully asynchronous drag preparation (no UI freeze while preparing URLs/files).
- Recursive folder staging with preserved structure.
- Safety thresholds for large batches (item-count and size confirmation).
- Staging root and cleanup behavior configurable from Settings.
- Robust collision naming (`name (1).ext`) and NFC-safe filename handling.

### SFTP and security hardening

- Auth methods: password, private key (passphrase), keyboard-interactive (OTP/2FA), ssh-agent.
- Host-key policies:
  - `Strict`
  - `Accept new (TOFU)`
  - `No verification` (with hardening)
- `No verification` hardening:
  - double confirmation
  - temporary per-host exception with TTL
  - persistent in-session risk banner
- Non-modal TOFU confirmation flow for better responsiveness.
- Atomic `known_hosts` persistence:
  - POSIX: `mkstemp -> write -> fsync -> rename -> fsync(parent)`
  - Windows path validated with `FlushFileBuffers` + `MoveFileEx`
- If fingerprint persistence fails, one-time connect requires explicit user confirmation.
- `known_hosts` defaults to hashed hostnames; optional plain mode.
- Strict file/dir permissions on POSIX (`~/.ssh` 0700, file 0600).
- Safer keyboard-interactive behavior: explicit cancel prevents password fallback.
- Defensive cleanup on failed `connect()` paths.
- Transfer integrity checks for resume/finalize (`Off/Optional/Required`), with `.part` temporary files and atomic final rename.
- Sensitive log data is redacted by default; debug-sensitive output is opt-in.
- Host-key audit log at `~/.openscp/openscp.auth`.

### Site Manager and credential handling

- Saved sites use stable UUID-backed identities (instead of name-only keys).
- Duplicate site names are blocked.
- Rename/remove flows clean legacy/orphan secret entries.
- Deleting a site can also remove stored credentials and related `known_hosts` entry.
- Secure storage backends:
  - macOS: Keychain
  - Linux: libsecret (when available)
- Secure-only builds report explicit persistence-unavailable states.
- Quick Connect can save the site automatically (and optionally credentials) without creating duplicates.

### UX / UI improvements

- Connection dialog improvements:
  - no misleading prefilled `host/user`
  - cleaner placeholders and focus on host
  - `host+port` row composition
  - show/hide toggles for password and key passphrase
  - inline browse controls for private key and `known_hosts`
- Settings dialog redesigned with left-side sections (`General` and `Advanced`) and grouped advanced categories.
- Permissions dialog includes octal preview and common presets (644/755/600/700/664/775).
- About dialog improvements:
  - friendlier fallback copy
  - diagnostics copy button (app version, Qt, OS, build type, git commit, repo)
  - dynamic docs/licenses path discovery
  - license button enabled/disabled based on availability

### CI, tests, and release quality baseline

- CI workflow split by intent:
  - push to `dev`: fast Linux checks (build + non-integration tests)
  - PR to `main`: integration gate on Linux and macOS
- Integration CI boots a temporary local SFTP server for end-to-end tests.
- Nightly quality workflow includes:
  - ASan + UBSan
  - TSan
  - `cppcheck` static analysis
- Test suites included in-tree:
  - core mock/unit tests
  - libssh2 integration tests (skips when integration env is unavailable)

### Environment variables

- `OPEN_SCP_KNOWNHOSTS_PLAIN=1|0` - force plain vs hashed hostnames in `known_hosts`.
- `OPEN_SCP_FP_HEX_ONLY=1` - show fingerprints in HEX with `:`.
- `OPEN_SCP_TRANSFER_INTEGRITY=off|optional|required` - override transfer integrity policy.
- `OPEN_SCP_LOG_LEVEL=error|warn|info|debug` - adjust core log verbosity.
- `OPEN_SCP_LOG_SENSITIVE=1` - enable sensitive debug details (disabled by default).
- `OPEN_SCP_ENABLE_INSECURE_FALLBACK=1` - enable insecure secret fallback only when supported by the build/platform.

---

## Requirements

- Qt `6.x` (tested with `6.8.3`)
- libssh2 (OpenSSL 3 recommended)
- CMake `3.22+`
- C++20 compiler

Optional:

- macOS: Keychain (native)
- Linux: libsecret / Secret Service

---

## Build

```bash
git clone https://github.com/luiscuellar31/openscp.git
cd openscp
rm -rf build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# Linux binary
./build/openscp_hello
```

### Run tests locally

```bash
cmake -S . -B build -DOPEN_SCP_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Note: `openscp_sftp_integration_tests` may be skipped locally if the integration SFTP environment variables are not set.

### macOS quick workflow (recommended)

```bash
# daily development
./scripts/macos.sh dev

# or step-by-step
./scripts/macos.sh configure
./scripts/macos.sh build
./scripts/macos.sh run

# local unsigned packaging
./scripts/macos.sh app   # dist/*.zip (OpenSCP.app zipped)
./scripts/macos.sh pkg   # dist/*.pkg
./scripts/macos.sh dmg   # dist/*.dmg
./scripts/macos.sh dist  # all of the above
```

If Qt is not in the default path (`$HOME/Qt/<version>/macos`), set one:

```bash
export QT_PREFIX="/path/to/Qt/<version>/macos"
# or
export Qt6_DIR="/path/to/Qt/<version>/macos/lib/cmake/Qt6"
```

See `assets/macos/README.md` for detailed packaging and notarization notes.

### Linux (build + AppImage)

See `assets/linux/README.md` for AppImage packaging via `scripts/package_appimage.sh`.

---

## Screenshots

<p align="center">
  <img src="assets/screenshots/screenshot-site-manager.png" alt="Site Manager with saved servers" width="32%">
  <img src="assets/screenshots/screenshot-connect.png" alt="Connect dialog with authentication options" width="32%">
  <img src="assets/screenshots/screenshot-transfer-queue.png" alt="Transfer queue with progress, filters, and actions" width="32%">
</p>

---

## Roadmap (short / mid-term)

- Windows support is planned for future releases.
- Protocols: `SCP`, then `FTP/FTPS/WebDAV`.
- Proxy/jump support: `SOCKS5`, `HTTP CONNECT`, `ProxyJump`.
- Sync workflows: compare/sync and keep-up-to-date with filters/ignores.
- Queue persistence across restarts.
- More UX: bookmarks, history, command palette, themes.

---

## Credits & Licenses

- libssh2, OpenSSL, zlib, and Qt are owned by their respective authors.
- License texts: [docs/credits/LICENSES/](docs/credits/LICENSES/)
- Qt (LGPL) materials: [docs/credits](docs/credits)

---

## Contributing

- Contributions are welcome. Read `CONTRIBUTING.md` for workflow, branch strategy, and standards.
- Issues and pull requests are welcome, especially around macOS/Linux stability, i18n, and SFTP robustness.
