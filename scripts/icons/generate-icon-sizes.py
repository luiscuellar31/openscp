#!/usr/bin/env python3
"""Derive the icon sizes, the macOS bundle icon and the snap icon from the
1024-pixel icons, on macOS, with the system sips and iconutil tools."""

from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
ICONS = ROOT / "assets/icons/app"
STYLES = ("standard", "macos-light", "macos-dark")
# The sizes desktops and the application use; CMakeLists.txt lists them too.
SIZES = (16, 24, 32, 48, 64, 128, 256, 512)
# Finder, and the Dock while OpenSCP is closed, show macOS's default style.
BUNDLE_STYLE = "macos-light"
# The sizes an iconset holds, each of them also at twice the size.
ICONSET_SIZES = (16, 32, 128, 256, 512)


def resample(source, size, output):
    subprocess.run(
        [
            "sips",
            "--resampleHeightWidth", str(size), str(size),
            str(source),
            "--out", str(output),
        ],
        check=True,
        stdout=subprocess.DEVNULL,
    )


def main():
    for style in STYLES:
        master = ICONS / style / "openscp-1024.png"
        for size in SIZES:
            resample(master, size, ICONS / style / f"openscp-{size}.png")

    master = ICONS / BUNDLE_STYLE / "openscp-1024.png"
    with tempfile.TemporaryDirectory() as directory:
        iconset = Path(directory) / "OpenSCP.iconset"
        iconset.mkdir()
        for size in ICONSET_SIZES:
            resample(master, size, iconset / f"icon_{size}x{size}.png")
            resample(master, 2 * size, iconset / f"icon_{size}x{size}@2x.png")
        subprocess.run(
            ["iconutil", "-c", "icns", str(iconset),
             "-o", str(ROOT / "assets/macos/OpenSCP.icns")],
            check=True,
        )

    # snapcraft reads the icon from the snap project's folder.
    shutil.copyfile(ICONS / "standard/openscp-256.png", ROOT / "packaging/snap/icon.png")


if __name__ == "__main__":
    main()
