# OpenSCP (Pre-alpha)

**[Leer en Español](README_ES.md)**

**OpenSCP** is a *two-panel commander*-style file explorer written in **C++/Qt**, with **SFTP** support (libssh2 + OpenSSL).
It aims to be a lightweight, cross-platform alternative to tools like WinSCP, focused on **simplicity**, **security**, and **extensibility**.

---

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

* Qt **6.x**
* libssh2 (**OpenSSL 3** recommended)
* CMake **3.22+**
* **C++20** compiler

**Optional**

* macOS: **Keychain** (native).
* Linux: **Libsecret/Secret Service** for credential storage.

---

## Build

```bash
git clone https://github.com/tuusuario/openscp.git
cd openscp
cmake -S . -B build
cmake --build build
./build/openscp_hello
```

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

* Third-party attribution started (see **docs/**).
* libssh2, OpenSSL, and Qt are owned by their respective authors.

---

## Project Status

OpenSCP is currently usable in most cases.
Issues and pull requests are welcome—especially around macOS/Linux stability, i18n, and the drag-and-drop flow.
