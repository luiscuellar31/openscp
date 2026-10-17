#!/usr/bin/env python3
"""Derive the icon sizes, the macOS bundle icon, the Windows icon and the snap
icon from the 1024-pixel icons, on macOS, with the system sips and iconutil
tools."""

from pathlib import Path
import shutil
import struct
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
# Explorer and the taskbar show the standard style; an .ico holds at most
# 256 pixels.
WINDOWS_ICON_SIZES = (16, 24, 32, 48, 64, 128, 256)


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


def write_windows_icon(output):
    """Pack the standard PNG sizes into an .ico. Windows Vista and later read
    PNG-compressed icon images, so the files go in unchanged."""
    images = [
        (ICONS / f"standard/openscp-{size}.png").read_bytes()
        for size in WINDOWS_ICON_SIZES
    ]
    header = struct.pack("<HHH", 0, 1, len(images))
    entries = b""
    offset = len(header) + 16 * len(images)
    for size, image in zip(WINDOWS_ICON_SIZES, images):
        # A width or height of 0 means 256 pixels.
        dimension = size % 256
        entries += struct.pack("<BBBBHHII", dimension, dimension, 0, 0, 1, 32,
                               len(image), offset)
        offset += len(image)
    output.write_bytes(header + entries + b"".join(images))


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

    write_windows_icon(ROOT / "assets/windows/OpenSCP.ico")

    # snapcraft reads the icon from the snap project's folder.
    shutil.copyfile(ICONS / "standard/openscp-256.png", ROOT / "packaging/snap/icon.png")


if __name__ == "__main__":
    main()
