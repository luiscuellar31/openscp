#!/usr/bin/env python3
"""Update the dependency versions that release builds pin by hand.

The macOS release dependencies (scripts/ci/build_macos_release_deps.sh) pin
OpenSSL, libssh2 and tinyxml2, and the Windows vcpkg overlay port
(vcpkg-ports/openssl) pins OpenSSL. This script moves OpenSSL to the newest
release of its 3.5 long-term support series (OPENSSL_SERIES) in both places,
and libssh2 and tinyxml2 to their newest release with the same major version.
A newer series or major version is reported, never taken: that stays a
maintainer's choice.

Every new archive is checked before a pin changes:
- OpenSSL: the release tarball must match its published .sha256 file and
  GitHub's digest of it, and the tag archive that the vcpkg port downloads
  must hold the same files as the tarball.
- libssh2: the tarball from libssh2.org must be identical to the copy attached
  to its GitHub release.
- tinyxml2: only GitHub's tag archive exists, so its hash is taken as
  downloaded, which the summary points out.

Without arguments it updates the files; --check only lists the updates. In
GitHub Actions, --github-output and --summary describe the change for the
pull request that proposes it.
"""

import argparse
from dataclasses import dataclass, field
import hashlib
import json
import os
from pathlib import Path
import re
import sys
import tarfile
import tempfile
import urllib.request

OPENSSL_SERIES = (3, 5)
MAX_DOWNLOAD_BYTES = 256 * 1024 * 1024

MACOS_SCRIPT = Path("scripts/ci/build_macos_release_deps.sh")
OVERLAY_MANIFEST = Path("vcpkg-ports/openssl/vcpkg.json")
OVERLAY_PORTFILE = Path("vcpkg-ports/openssl/portfile.cmake")


@dataclass
class Update:
    name: str
    old: str
    new: str
    platforms: list
    checks: list = field(default_factory=list)


def open_url(url: str, accept: str = ""):
    headers = {"User-Agent": "openscp-pinned-dependency-updates"}
    if accept:
        headers["Accept"] = accept
    # The token only ever goes to the GitHub API, which limits anonymous calls.
    token = os.environ.get("GITHUB_TOKEN", "")
    if token and url.startswith("https://api.github.com/"):
        headers["Authorization"] = f"Bearer {token}"
    return urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=120)


def stable_releases(repository: str) -> list:
    with open_url(f"https://api.github.com/repos/{repository}/releases?per_page=100",
                  "application/vnd.github+json") as response:
        releases = json.load(response)
    return [release for release in releases
            if not release["draft"] and not release["prerelease"]]


def versions(releases: list, tag_pattern: str) -> list:
    """(version tuple, release) pairs for the tags that match, newest first."""
    found = []
    for release in releases:
        match = re.fullmatch(tag_pattern, release["tag_name"])
        if match:
            found.append((tuple(int(part) for part in match.groups()), release))
    return sorted(found, key=lambda item: item[0], reverse=True)


def version_text(version: tuple) -> str:
    return ".".join(str(part) for part in version)


def parse_version(text: str) -> tuple:
    if not re.fullmatch(r"[0-9]+(\.[0-9]+){2}", text):
        raise ValueError(f"Unexpected version: {text}")
    return tuple(int(part) for part in text.split("."))


def download(url: str, destination: Path) -> dict:
    """Saves `url` and returns its SHA-256 and SHA-512."""
    sha256 = hashlib.sha256()
    sha512 = hashlib.sha512()
    size = 0
    with open_url(url) as response, destination.open("wb") as output:
        while chunk := response.read(1024 * 1024):
            size += len(chunk)
            if size > MAX_DOWNLOAD_BYTES:
                raise ValueError(f"{url} is larger than {MAX_DOWNLOAD_BYTES} bytes")
            sha256.update(chunk)
            sha512.update(chunk)
            output.write(chunk)
    return {"sha256": sha256.hexdigest(), "sha512": sha512.hexdigest()}


def release_asset(release: dict, name: str) -> dict:
    for asset in release["assets"]:
        if asset["name"] == name:
            return asset
    raise ValueError(f"Release {release['tag_name']} has no asset named {name}")


def files_in(archive: Path) -> dict:
    """The regular files of a .tar.gz by their path below its top folder,
    with their SHA-256."""
    files = {}
    with tarfile.open(archive, "r:gz") as tar:
        for member in tar:
            parts = member.name.split("/", 1)
            if member.isfile() and len(parts) == 2:
                files[parts[1]] = hashlib.sha256(tar.extractfile(member).read()).hexdigest()
    return files


def check_digest(asset: dict, sha256: str, checks: list, what: str) -> None:
    digest = asset.get("digest")
    if digest:
        if digest != f"sha256:{sha256}":
            raise ValueError(f"{what} does not match GitHub's digest {digest}")
        checks.append(f"{what} matches GitHub's digest of it.")


class Pins:
    """The files that hold the pins, edited in memory until saved."""

    def __init__(self, root: Path):
        self.root = root
        self.text = {path: (root / path).read_bytes().decode("utf-8")
                     for path in (MACOS_SCRIPT, OVERLAY_MANIFEST, OVERLAY_PORTFILE)}
        self.changed = set()

    def shell(self, name: str) -> str:
        match = re.search(rf'^{name}="([^"\r\n]*)"(?=\r?$)', self.text[MACOS_SCRIPT],
                          re.MULTILINE)
        if not match:
            raise ValueError(f"{MACOS_SCRIPT} has no {name}")
        return match.group(1)

    def set_shell(self, name: str, value: str) -> None:
        self._replace(MACOS_SCRIPT, rf'^{name}="[^"\r\n]*"(?=\r?$)', f'{name}="{value}"',
                      re.MULTILINE)

    def overlay_version(self) -> str:
        match = re.search(r'^  "version": "([^"]+)",(?=\r?$)', self.text[OVERLAY_MANIFEST],
                          re.MULTILINE)
        if not match:
            raise ValueError(f"{OVERLAY_MANIFEST} has no version")
        return match.group(1)

    def set_overlay(self, version: str, sha512: str) -> None:
        self._replace(OVERLAY_MANIFEST, r'^  "version": "[^"]+",(?=\r?$)',
                      f'  "version": "{version}",', re.MULTILINE)
        # A new version starts again at port version 0, which vcpkg leaves out.
        self.text[OVERLAY_MANIFEST] = re.sub(r'\r?\n  "port-version": [0-9]+,', "",
                                             self.text[OVERLAY_MANIFEST])
        if 'REF "openssl-${VERSION}"' not in self.text[OVERLAY_PORTFILE]:
            raise ValueError(f"{OVERLAY_PORTFILE} no longer downloads the openssl-<version> tag")
        self._replace(OVERLAY_PORTFILE, r"SHA512 [0-9a-f]{128}", f"SHA512 {sha512}")

    def _replace(self, path: Path, pattern: str, replacement: str, flags: int = 0) -> None:
        updated, count = re.subn(pattern, replacement, self.text[path], flags=flags)
        if count != 1:
            raise ValueError(f"Expected one match of {pattern!r} in {path}, found {count}")
        if updated != self.text[path]:
            self.text[path] = updated
            self.changed.add(path)

    def save(self) -> None:
        for path in self.changed:
            (self.root / path).write_bytes(self.text[path].encode("utf-8"))


def update_openssl(pins: Pins, work: Path, notes: list):
    candidates = versions(stable_releases("openssl/openssl"), r"openssl-([0-9]+)\.([0-9]+)\.([0-9]+)")
    in_series = [item for item in candidates if item[0][:2] == OPENSSL_SERIES]
    if not in_series:
        raise ValueError(f"No OpenSSL {version_text(OPENSSL_SERIES)} release found")
    newest, release = in_series[0]
    if candidates[0][0][:2] != OPENSSL_SERIES:
        notes.append(f"OpenSSL {version_text(candidates[0][0])} exists; this workflow stays on "
                     f"the {version_text(OPENSSL_SERIES)} long-term support series.")
    def behind(pin: tuple) -> bool:
        # A pin outside the series, as after copying vcpkg's port again,
        # goes back to it.
        return pin[:2] != OPENSSL_SERIES or pin < newest

    macos = parse_version(pins.shell("OPENSSL_VERSION"))
    overlay = parse_version(pins.overlay_version())
    if not behind(macos) and not behind(overlay):
        return None

    version = version_text(newest)
    tarball_name = f"openssl-{version}.tar.gz"
    tarball_asset = release_asset(release, tarball_name)
    checks = []
    tarball = download(tarball_asset["browser_download_url"], work / tarball_name)
    with open_url(release_asset(release, f"{tarball_name}.sha256")["browser_download_url"]) as response:
        published = response.read().decode("ascii").split()[0].lower()
    if published != tarball["sha256"]:
        raise ValueError(f"{tarball_name} does not match its published .sha256 file")
    checks.append(f"`{tarball_name}` matches the `.sha256` file published with the release.")
    check_digest(tarball_asset, tarball["sha256"], checks, f"`{tarball_name}`")
    tag_archive = download(f"https://github.com/openssl/openssl/archive/openssl-{version}.tar.gz",
                           work / "openssl-tag.tar.gz")
    if files_in(work / tarball_name) != files_in(work / "openssl-tag.tar.gz"):
        raise ValueError(f"The openssl-{version} tag archive differs from {tarball_name}")
    checks.append("The tag archive that the vcpkg port downloads holds the same files as the tarball.")

    platforms = []
    replaced = []
    if behind(macos):
        pins.set_shell("OPENSSL_VERSION", version)
        pins.set_shell("OPENSSL_SHA256", tarball["sha256"])
        platforms.append("macOS")
        replaced.append(macos)
    if behind(overlay):
        pins.set_overlay(version, tag_archive["sha512"])
        platforms.append("Windows")
        replaced.append(overlay)
    return Update("OpenSSL", version_text(min(replaced)), version, platforms, checks)


def same_major_newest(candidates: list, current: tuple, name: str, notes: list):
    same_major = [item for item in candidates if item[0][0] == current[0]]
    if candidates and candidates[0][0][0] != current[0]:
        notes.append(f"{name} {version_text(candidates[0][0])} exists; this workflow stays on "
                     f"major version {current[0]}.")
    return same_major[0] if same_major else None


def update_libssh2(pins: Pins, work: Path, notes: list):
    current = parse_version(pins.shell("LIBSSH2_VERSION"))
    candidates = versions(stable_releases("libssh2/libssh2"), r"libssh2-([0-9]+)\.([0-9]+)\.([0-9]+)")
    newest = same_major_newest(candidates, current, "libssh2", notes)
    if not newest or newest[0] <= current:
        return None
    version = version_text(newest[0])
    name = f"libssh2-{version}.tar.gz"
    site = download(f"https://libssh2.org/download/{name}", work / f"site-{name}")
    asset = release_asset(newest[1], name)
    github = download(asset["browser_download_url"], work / f"github-{name}")
    if site["sha256"] != github["sha256"]:
        raise ValueError(f"{name} from libssh2.org differs from the copy on its GitHub release")
    checks = [f"`{name}` from libssh2.org is identical to the copy attached to its GitHub release."]
    check_digest(asset, github["sha256"], checks, f"`{name}`")
    pins.set_shell("LIBSSH2_VERSION", version)
    pins.set_shell("LIBSSH2_SHA256", site["sha256"])
    return Update("libssh2", version_text(current), version, ["macOS"], checks)


def update_tinyxml2(pins: Pins, work: Path, notes: list):
    current = parse_version(pins.shell("TINYXML2_VERSION"))
    candidates = versions(stable_releases("leethomason/tinyxml2"), r"([0-9]+)\.([0-9]+)\.([0-9]+)")
    newest = same_major_newest(candidates, current, "tinyxml2", notes)
    if not newest or newest[0] <= current:
        return None
    version = version_text(newest[0])
    archive = download(f"https://github.com/leethomason/tinyxml2/archive/refs/tags/{version}.tar.gz",
                       work / "tinyxml2.tar.gz")
    pins.set_shell("TINYXML2_VERSION", version)
    pins.set_shell("TINYXML2_SHA256", archive["sha256"])
    checks = ["tinyxml2 publishes only GitHub's tag archive, so its SHA-256 is taken as "
              "downloaded; no second source confirms it."]
    return Update("tinyxml2", version_text(current), version, ["macOS"], checks)


def describe(updates: list, notes: list) -> tuple:
    """The commit title, branch name and pull request body for the updates."""
    platforms = sorted({platform for update in updates for platform in update.platforms})
    scope = "(macos)" if platforms == ["macOS"] else "(windows)" if platforms == ["Windows"] else ""
    names = [f"{update.name} {update.new}" for update in updates]
    if len(names) < 3:
        listed = " and ".join(names)
    else:
        listed = f"{', '.join(names[:-1])}, and {names[-1]}"
    title = f"build{scope}: ship {listed} in release builds"
    branch = "deps/" + "-".join(f"{update.name.lower()}-{update.new}" for update in updates)
    lines = ["Updates the dependency versions that release builds pin by hand.", ""]
    for update in updates:
        lines.append(f"**{update.name} {update.old} → {update.new}** "
                     f"({' and '.join(update.platforms)})")
        lines.extend(f"- {check}" for check in update.checks)
        lines.append("")
    if notes:
        lines.append("Not taken, as a newer series or major version is a maintainer's choice:")
        lines.extend(f"- {note}" for note in notes)
        lines.append("")
    lines.append("Proposed by `scripts/release/update-pinned-deps.py`.")
    return title, branch, "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true", help="list the updates without applying them")
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2],
                        help="repository to update (default: this one)")
    parser.add_argument("--summary", type=Path, help="write the pull request body to this file")
    parser.add_argument("--github-output", action="store_true",
                        help="describe the change in $GITHUB_OUTPUT")
    args = parser.parse_args()

    pins = Pins(args.root)
    notes = []
    with tempfile.TemporaryDirectory(prefix="openscp-pinned-deps-") as directory:
        work = Path(directory)
        updates = [update for update in (update_openssl(pins, work, notes),
                                         update_libssh2(pins, work, notes),
                                         update_tinyxml2(pins, work, notes)) if update]

    for note in notes:
        print(f"note: {note}")
    if not updates:
        print("Every pinned dependency is on its newest allowed release.")
    for update in updates:
        print(f"{update.name}: {update.old} -> {update.new} ({', '.join(update.platforms)})")
        for check in update.checks:
            print(f"  {check}")
    title, branch, body = describe(updates, notes) if updates else ("", "", "")
    if not args.check:
        pins.save()
    if args.summary and updates:
        args.summary.write_text(body, encoding="utf-8")
    if args.github_output:
        platforms = {platform for update in updates for platform in update.platforms}
        with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as output:
            output.write(f"changed={'true' if updates else 'false'}\n")
            output.write(f"macos={'true' if 'macOS' in platforms else 'false'}\n")
            output.write(f"windows={'true' if 'Windows' in platforms else 'false'}\n")
            output.write(f"title={title}\n")
            output.write(f"branch={branch}\n")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
