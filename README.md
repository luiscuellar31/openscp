<div align="center">
	<img src="assets/program/icon-openscp-2048.png" alt="OpenSCP icon" width="128">
	<h1 align="center">OpenSCP</h1>

<p>
	<strong>Two‑panel SFTP client focused on simplicity and security</strong>
</p>

<p>
	<a href="README_ES.md"><strong>Leer en Español</strong></a>
</p>

<p>
	<strong>OpenSCP</strong> is a <em>two-panel commander</em>-style file explorer written in <strong>C++/Qt</strong>, with <strong>SFTP</strong> support (libssh2 + OpenSSL). It aims to be a lightweight, cross-platform alternative to tools like WinSCP, focused on <strong>simplicity</strong>, <strong>security</strong>, and <strong>extensibility</strong>.
</p>

  <br>

  <img src="assets/screenshots/screenshot-main-window.png" alt="OpenSCP main window showing dual panels and transfer queue" width="900">

</div>

## Releases

Looking for a fixed/stable build? Download tagged releases:
https://github.com/luiscuellar31/openscp/releases

- `main`: tested, stable code (moves between releases)
- `dev`: active development (PRs should target dev)

## Current Features (v0.6.0)

### Dual panels (local ↔ remote)

* Left and right panels navigate independently.
* Drag-and-drop between panels to copy/move.
* Remote context menu: Download, Upload, Rename, Delete, New Folder, Change Permissions (recursive).
* Sortable columns with automatic width adjustment.

### Transfers

* **Visible queue** with pause/resume/cancel/retry.
* Per-file resume; speed limits (global and per task).
* Automatic reconnection with backoff.
* Global and per-file progress.
* Timestamps: preserves remote **mtime** on download (UI shows **local time**, operations use **UTC**).
* Unknown sizes: end-to-end flag; the UI shows “—” with tooltip “Size: unknown”.

### SFTP (libssh2)

* Authentication: user/password, private key (with passphrase), **keyboard-interactive** (OTP/2FA), **ssh-agent**.
* Host-key verification policies:

  * **Strict**
  * **Accept new (TOFU)**
  * **Disabled**
* **Non-modal TOFU** (macOS/Linux/Windows): no `exec()`, no blocking; the “Connecting…” window never steals focus.
* Robust `known_hosts`:

  * **Atomic writes** (POSIX: `mkstemp → fsync → rename`; Windows: `FlushFileBuffers + MoveFileEx`).
  * Strict permissions (`~/.ssh` **0700**, file **0600**).
  * **Hashed hostnames** by default (`OpenSSH/TYPE_SHA1`); option for plain text.
  * Default fingerprints `SHA256:<base64>` (OpenSSH style); alternative **HEX with colons** view.
* Modern crypto:

  * Excludes `ssh-dss` and `ssh-rsa` (SHA-1).
  * Uses ED25519/ECDSA/RSA-SHA2 host keys and modern KEX/ciphers (curve25519, chacha20-poly1305, AES-GCM/CTR, HMAC-SHA-2).
* TOFU security:

  * If the fingerprint cannot be persisted, explicit confirmation is required to **“connect this time only.”**
  * **Audit log** at `~/.openscp/openscp.auth`: host, algorithm, fingerprint, and status (`saved|skipped|save_failed|rejected`).
* Cleanup: when deleting a site with “remove saved credentials,” its `known_hosts` entry is purged as well.

### Remote → system drag-and-drop (asynchronous)

* **Truly asynchronous**: the drag starts only after URLs are prepared (no UI blocking).
* **Folders (recursive)** with structure preserved; confirmation if > **500** items or > **1 GiB** estimated.
* **Per-batch staging**:

  * Default root `~/Downloads/OpenSCP-Dragged`.
  * Optional auto-clean on completion; deferred cleanup of batches older than **7 days** at startup.
  * If something fails or you cancel: drag **does not** start; a clickable link to the batch is shown (not deleted).
* Collisions & names: “`name (1).ext`” style, safe for multi-extension files and Unicode **NFC**.
* MIME: `text/uri-list` + `application/x-openscp-staging-batch` (batch ID).

### Site Manager

* Server list with stored credentials.
* Independent preferences:

  * **Open at startup** (optional).
  * **Open on disconnect** (optional).
    Both **ON by default** and non-modal; if a modal is active, opening is **deferred**.
* Comfortable editing: ellipsis only while painting; the editor shows the **full name**.
* Keychain (macOS) and Libsecret (Linux) for credentials; **insecure** fallback only with confirmation (persistent **red** banner when active).
  - macOS: the insecure fallback option is not shown (Keychain is always used).
  - Linux/Windows: the fallback is available only when the build does not link Libsecret and is not compiled with `OPEN_SCP_BUILD_SECURE_ONLY`.

### UX / UI (Qt)

* Several buttons replaced with consistent **icons**; cleaner menus and fixed shortcuts.
* **Non-modal** dialogs with macOS stability (native dialogs **are not moved/centered** on `QEvent::Show`).
* Stable window sizing: never shrinks below `sizeHint()`, honors `resize()/minimumSize()`.
* i18n: ES/EN updated (unified terminology and punctuation).

### Settings / Configuration

* Collapsible **Advanced** panel (▸/▾).
* New options:

  * `known_hosts`: hashed by default; switch to plain text if needed.
  * Fingerprint format: `SHA256:<base64>` or **HEX with “:”**.
  * Staging: root, auto-cleanup, depth limits, and preparation timeout (via QSettings `Advanced/stagingPrepTimeoutMs`, in milliseconds).
  * “When deleting a site, also remove saved credentials.” (listed first).

### Environment variables

* `OPEN_SCP_KNOWNHOSTS_PLAIN=1|0` — Force plain vs. hashed hostnames in `known_hosts` (default: **hashed**).
* `OPEN_SCP_FP_HEX_ONLY=1` — Show fingerprints only in HEX with “:”.
* `OPEN_SCP_ENABLE_INSECURE_FALLBACK=1` — Enable insecure secrets fallback when supported by the build/platform (non‑Apple, without Libsecret, and not `OPEN_SCP_BUILD_SECURE_ONLY`); shows a **red banner** when active.

---

## Requirements

* Qt **6.8.3**
* libssh2 (**OpenSSL 3** recommended)
* CMake **3.22+**
* **C++20** compiler

**Optional**

* macOS: **Keychain** (native).
* Linux: **Libsecret/Secret Service** for credential storage.

---

## Build

```bash
git clone https://github.com/luiscuellar31/openscp.git
cd openscp
rm -rf build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
# Linux/Windows:
./build/openscp_hello
```

For macOS, use the workflow below (`./scripts/macos.sh ...`) instead of opening `build/OpenSCP.app` directly.

### macOS quick workflow (recommended)

```bash
# Daily development
./scripts/macos.sh dev      # configure + build + run
# or explicitly:
./scripts/macos.sh configure
./scripts/macos.sh build
./scripts/macos.sh run

# Local packaging outputs (unsigned)
./scripts/macos.sh app      # dist/*.zip (OpenSCP.app zipped)
./scripts/macos.sh pkg      # dist/*.pkg
./scripts/macos.sh dmg      # dist/*.dmg
# all of them:
./scripts/macos.sh dist
```

- `scripts/macos.sh` is the fixed entrypoint for macOS development and packaging.
- See `assets/macos/README.md` for detailed notes about unsigned distribution and notarization.

### Linux (build + AppImage)

- See `assets/linux/README.md` for building on Linux and producing an unsigned `.AppImage` via `scripts/package_appimage.sh`.

---

## Screenshots

<p align="center">
  <img src="assets/screenshots/screenshot-site-manager.png" alt="Site Manager with saved servers" width="32%">
  <img src="assets/screenshots/screenshot-connect.png" alt="Connect dialog with authentication options" width="32%">
  <img src="assets/screenshots/screenshot-transfer-queue.png" alt="Transfer queue with pause/resume and progress" width="32%">
</p>

---

## Roadmap (short / mid-term)

* Protocols: **SCP**; plan for **FTP/FTPS/WebDAV**.
* Real queue concurrency (multiple parallel connections without global locking).
* Proxy / Jump host: **SOCKS5**, **HTTP CONNECT**, **ProxyJump**.
* Sync: compare/sync and “keep up to date” with filters/ignores.
* Queue persistence: resume after restart; optional checksums.
* More UX: bookmarks, history, command palette, themes.

---

## Credits & Licenses

* libssh2, OpenSSL, zlib, and Qt are owned by their respective authors.
* License texts: [docs/credits/LICENSES/](docs/credits/LICENSES/) — license files for third‑party components (Qt, libssh2, OpenSSL, zlib, etc.).
* Qt (LGPL) materials: [docs/credits](docs/credits) — see Qt LGPLv3 compliance and licensing notes.

---

## Contributing

- We welcome contributions from the community. Please read `CONTRIBUTING.md` for the workflow, branch strategy, and standards.
- Issues and pull requests are welcome, especially around macOS/Linux stability, i18n, and the drag-and-drop flow.
