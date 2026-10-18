#!/usr/bin/env python3
"""Write the winget manifests of a release.

winget installs packages from the manifests in microsoft/winget-pkgs. This
writes the three manifests of one OpenSCP release, which offer the per-user
setup and the per-machine MSI, into OUTPUT/<version>. Submit that folder to
winget-pkgs as manifests/l/luiscuellar31/OpenSCP/<version>, for instance
with "wingetcreate submit"; the first submission creates the package.
"""

import argparse
import hashlib
from pathlib import Path
import re

PACKAGE = "luiscuellar31.OpenSCP"
REPOSITORY = "https://github.com/luiscuellar31/openscp"
MANIFEST_VERSION = "1.10.0"
# packaging/windows/OpenSCP.iss: Windows lists the setup's AppId with "_is1".
SETUP_PRODUCT_CODE = "{2C03B5C3-87DD-4DAB-B609-F89E831909E6}_is1"
# packaging/windows/OpenSCP.wxs: the UpgradeCode that every version shares.
MSI_UPGRADE_CODE = "{FF30140B-7449-4FBC-A92C-EA5E5C79F69A}"


def sha256_of(assets: Path, name: str) -> str:
    matches = [path for path in assets.rglob(name)
               if path.is_file() and not path.is_symlink()]
    if len(matches) != 1:
        raise ValueError(f"Expected exactly one {name} under {assets}")
    hasher = hashlib.sha256()
    with matches[0].open("rb") as stream:
        for chunk in iter(lambda: stream.read(256 * 1024), b""):
            hasher.update(chunk)
    return hasher.hexdigest().upper()


def manifest(kind: str, version: str, body: str) -> str:
    return (f"# yaml-language-server: $schema=https://aka.ms/winget-manifest."
            f"{kind}.{MANIFEST_VERSION}.schema.json\n\n"
            f"PackageIdentifier: {PACKAGE}\n"
            f"PackageVersion: {version}\n"
            f"{body}"
            f"ManifestType: {kind}\n"
            f"ManifestVersion: {MANIFEST_VERSION}\n")


def build_manifests(assets: Path, version: str) -> dict:
    if not re.fullmatch(r"(?:0|[1-9][0-9]{0,8})(?:\.(?:0|[1-9][0-9]{0,8})){2}", version):
        raise ValueError("Invalid stable release version")
    setup = f"OpenSCP-{version}-x64-setup.exe"
    msi = f"OpenSCP-{version}-x64.msi"
    download = f"{REPOSITORY}/releases/download/v{version}"
    installers = (
        # Windows 10 1809, the oldest Windows that Qt 6 supports.
        "MinimumOSVersion: 10.0.17763.0\n"
        "UpgradeBehavior: install\n"
        "Installers:\n"
        "- Architecture: x64\n"
        "  InstallerType: inno\n"
        "  Scope: user\n"
        f"  InstallerUrl: {download}/{setup}\n"
        f"  InstallerSha256: {sha256_of(assets, setup)}\n"
        "  InstallerSwitches:\n"
        "    Custom: /CURRENTUSER\n"
        f"  ProductCode: '{SETUP_PRODUCT_CODE}'\n"
        "- Architecture: x64\n"
        "  InstallerType: wix\n"
        "  Scope: machine\n"
        f"  InstallerUrl: {download}/{msi}\n"
        f"  InstallerSha256: {sha256_of(assets, msi)}\n"
        "  AppsAndFeaturesEntries:\n"
        f"  - UpgradeCode: '{MSI_UPGRADE_CODE}'\n")
    locale = (
        "PackageLocale: en-US\n"
        "Publisher: OpenSCP contributors\n"
        f"PublisherUrl: {REPOSITORY}\n"
        f"PublisherSupportUrl: {REPOSITORY}/issues\n"
        "PackageName: OpenSCP\n"
        f"PackageUrl: {REPOSITORY}\n"
        "License: GPL-3.0-only\n"
        f"LicenseUrl: {REPOSITORY}/blob/main/LICENSE\n"
        "ShortDescription: A dual-panel file transfer client for SFTP, SCP, "
        "FTP, FTPS, and WebDAV.\n"
        "Tags:\n"
        "- ftp\n"
        "- ftps\n"
        "- scp\n"
        "- sftp\n"
        "- webdav\n"
        f"ReleaseNotesUrl: {REPOSITORY}/releases/tag/v{version}\n")
    return {
        f"{PACKAGE}.yaml": manifest("version", version, "DefaultLocale: en-US\n"),
        f"{PACKAGE}.installer.yaml": manifest("installer", version, installers),
        f"{PACKAGE}.locale.en-US.yaml": manifest("defaultLocale", version, locale),
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--assets", required=True, type=Path,
                        help="folder that holds the release's Windows packages")
    parser.add_argument("--version", required=True)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    manifests = build_manifests(args.assets, args.version)
    folder = args.output / args.version
    folder.mkdir(parents=True, exist_ok=True)
    for name, content in manifests.items():
        (folder / name).write_bytes(content.encode("utf-8"))
        print(folder / name)


if __name__ == "__main__":
    main()
