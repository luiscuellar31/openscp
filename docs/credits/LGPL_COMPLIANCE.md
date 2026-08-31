# Qt and the LGPLv3

OpenSCP uses the open-source edition of Qt under the GNU LGPL version 3. This
page explains what ships with OpenSCP and how to obtain, replace, or rebuild the
Qt libraries. The license text remains the authoritative source.

## What OpenSCP provides

Official self-contained macOS and AppImage packages use Qt 6.8.3 and link to Qt
dynamically. Snap and Flatpak use the compatible Qt 6 libraries supplied by
their external runtimes.

OpenSCP packages provide:

- a notice that identifies the Qt modules in use;
- the complete LGPLv3 license text;
- access to the corresponding Qt source; and
- a way to rebuild OpenSCP or replace compatible Qt libraries.

OpenSCP currently uses Qt Core, Gui, Widgets, and SVG. We do not ship modified
Qt libraries. If that changes, the corresponding source or patches will be
provided with the affected release.

See [CREDITS.md](CREDITS.md) for the complete third-party component list and
[Qt-LGPL-3.0.txt](LICENSES/Qt-LGPL-3.0.txt) for the license text.

## Qt source

For official packages that bundle Qt 6.8.3, the matching upstream source is:

- [Qt 6.8.3 source archive](https://download.qt.io/official_releases/qt/6.8/6.8.3/single/qt-everywhere-src-6.8.3.tar.xz)

You may also request a copy of the corresponding source for at least three
years after the OpenSCP release that included it:

- Email: luiscuellar31@proton.me

The source is provided at no charge other than reasonable media or shipping
costs, if any. For Snap and Flatpak, consult the package runtime metadata for
the exact Qt build and its corresponding source.

## Rebuilding with another Qt

The simplest way to use a compatible or modified Qt build is to rebuild
OpenSCP against it:

```bash
git clone https://github.com/luiscuellar31/openscp.git
cd openscp
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/Qt
cmake --build build --parallel
```

On Linux, run the rebuilt application with the Qt library path you selected if
it is not installed system-wide:

```bash
LD_LIBRARY_PATH=/path/to/Qt/lib:$LD_LIBRARY_PATH ./build/openscp
```

An AppImage can also be extracted with `--appimage-extract`. Its Qt libraries
and plugins can then be replaced with ABI-compatible versions before running
the extracted `AppRun` or repacking the image.

On macOS, point CMake to the selected Qt installation:

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/Qt/6.x/macos
cmake --build build --parallel
```

A packaged application keeps its Qt frameworks in
`OpenSCP.app/Contents/Frameworks`. Compatible replacements may invalidate the
existing code signature; an ad-hoc local signature can be applied afterward:

```bash
codesign --force --deep --sign - OpenSCP.app
```

OpenSCP does not use technical measures to prevent these replacements. For the
general LGPL requirements, see
[Qt's open-source licensing guidance](https://www.qt.io/development/open-source-lgpl-obligations).

## Contact

For Qt source requests or questions about the files shipped with OpenSCP,
contact luiscuellar31@proton.me.
