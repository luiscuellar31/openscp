# Qt (LGPLv3) Compliance — OpenSCP

OpenSCP uses Qt 6.8.3 under the GNU LGPL v3. This document explains how we comply with the license and how you can exercise your rights (to obtain source code, replace the Qt libraries, and relink).

---

## 1) Summary of obligations and compliance

- License text: We include the full LGPLv3 text (`docs/credits/LICENSES/Qt-LGPL-3.0.txt`).
- Dynamic linking: OpenSCP is built to dynamically link against Qt (LGPL‑compatible).
- No tivoization: We do not use technical measures that prevent you from replacing Qt.
- Source offer: We provide the corresponding source for the LGPL‑covered Qt libraries we use.
- Relinking: You can rebuild OpenSCP against your own Qt build (instructions below).
- Modifications to Qt: We do not distribute a modified Qt. If we ever do, we will publish the corresponding source/patches as required by the LGPL.

---

## 2) Qt version and modules

- Qt version used in releases: 6.8.3 (LTS). We pin to this exact version in distributed binaries; if we ship a patch release, we will update this document accordingly.
- Typical modules in this project: `Qt6Core`, `Qt6Gui`, `Qt6Widgets` (plus translation tools for build). No local modifications.
- Verify which Qt libraries your binary uses:
  - Linux: `ldd ./build/openscp_hello | grep Qt6`
  - macOS: `otool -L OpenSCP.app/Contents/MacOS/OpenSCP | grep Qt` (if using a bundled `.app`)

---

## 3) Obtaining the corresponding Qt source code

We link to the exact Qt source matching the binaries we distribute. Example for Qt 6.8.3:

- Official source tarball: https://download.qt.io/official_releases/qt/6.8/6.8.3/single/qt-everywhere-src-6.8.3.tar.xz

If the URL ever changes, contact us and we will provide a copy. You may also request the corresponding source by email within 3 years of the release:

- Email: luiscuellar31@proton.me

No charge applies other than reasonable media/shipping costs, if any.

Tip: If you prefer to build from distro packages, use the distro’s Qt source matching the Qt binaries you use to run OpenSCP.

---

## 4) Relinking OpenSCP with your own Qt build

You are entitled to replace the Qt libraries and relink OpenSCP against them.

### 4.1 Linux

Build or install your Qt (Qt 6.x) and rebuild OpenSCP against it:

```bash
# Example: building Qt from source
tar xf qt-everywhere-src-<version>.tar.xz
cd qt-everywhere-src-<version>
./configure -prefix "$HOME/qt-<version>" -release
cmake --build . --parallel
cmake --install .

# Rebuild OpenSCP against your Qt
git clone https://github.com/tuusuario/openscp.git
cd openscp
cmake -S . -B build -DCMAKE_PREFIX_PATH="$HOME/qt-<version>"
cmake --build build --parallel

# Run with your Qt (if installed in a non-standard location)
LD_LIBRARY_PATH="$HOME/qt-<version>/lib:$LD_LIBRARY_PATH" ./build/openscp_hello
```

If you use AppImage, you can extract, replace the `libQt6*.so.*` inside `squashfs-root/usr/lib/`, and repack with `appimagetool`. Alternatively, run from the extracted tree with an updated `LD_LIBRARY_PATH`.

We do not encrypt or lock the shipped libraries; replacement is not technically restricted.

### 4.2 macOS (.app bundle)

Install Qt (Qt 6.x, e.g., 6.8.3, via Homebrew, the official installer, or build from source) and rebuild OpenSCP:

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH="/path/to/Qt/<version>/macos"
cmake --build build --parallel
```

If using a prebuilt `.app`, it is dynamically linked and may embed Qt Frameworks (via `macdeployqt`). You can replace frameworks in `OpenSCP.app/Contents/Frameworks/` with your own Qt frameworks. After replacement, the system code signature may be invalidated; re‑sign locally:

```bash
codesign --force --deep --sign - OpenSCP.app
```

To inspect/tweak library paths:

```bash
otool -L OpenSCP.app/Contents/MacOS/OpenSCP | grep Qt
install_name_tool -add_rpath "@executable_path/../Frameworks" OpenSCP.app/Contents/MacOS/OpenSCP
```

We do not impose any technical measures to prevent relinking. On standard macOS systems, re‑signing your modified app is sufficient to run it.

---

## 5) Modifications to Qt

We currently do not ship a modified Qt. If we ever distribute OpenSCP with modified Qt libraries, we will provide the corresponding modified Qt source code and/or patches alongside the binaries or in this repository under `docs/credits/qt-patches/`.

---

## 6) License texts shipped

- `docs/credits/LICENSES/Qt-LGPL-3.0.txt` — Qt’s LGPLv3
- Project’s main license: `LICENSE` (GPLv3‑only) and `docs/LICENSING.md`

---

## 7) FAQ (quick)

- Why pin to an exact 6.x instead of “6.x”?  
  To ensure the “corresponding source” matches the exact binaries you ship.

- Do you statically link Qt?  
  No. We dynamically link Qt. If we ever shipped static builds, we would provide the object files (or an equivalent means) to allow relinking, per LGPLv3 §6.

- Can I use a newer/different Qt to run OpenSCP?  
  Yes, as long as ABI is compatible and you can satisfy runtime dependencies. The relinking section above should help.

---

## 8) Contact

Questions or source requests (Qt corresponding source):

Email: luiscuellar31@proton.me
