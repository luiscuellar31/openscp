#!/usr/bin/env python3
"""Create the bounded, Ed25519-signed AppImage update manifest.

The private key is a PEM secret supplied only to the release job. No production
keys are generated here. Keep minimum glibc versions aligned with the verified
release-build baselines.
"""

import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile


def build_manifest(assets: Path, version: str) -> bytes:
    if not re.fullmatch(r"(?:0|[1-9][0-9]{0,8})(?:\.(?:0|[1-9][0-9]{0,8})){2}", version):
        raise ValueError("Invalid stable release version")
    artifacts = []
    for architecture, baseline in (("x86_64", "2.34"), ("aarch64", "2.39")):
        name = f"OpenSCP-{version}-{architecture}.AppImage"
        matches = list(assets.rglob(name))
        if len(matches) != 1 or matches[0].is_symlink():
            raise ValueError(f"Expected exactly one regular artifact: {name}")
        artifact = matches[0]
        size = artifact.stat().st_size
        if not artifact.is_file() or not 0 < size <= 256 * 1024 * 1024:
            raise ValueError(f"Invalid artifact size: {name}")
        with artifact.open("rb") as stream:
            hasher = hashlib.sha256()
            for chunk in iter(lambda: stream.read(256 * 1024), b""):
                hasher.update(chunk)
            digest = hasher.hexdigest()
        artifacts.append(dict(name=name, size=size, sha256=digest, minimum_glibc=baseline))
    return (json.dumps(dict(schema=1, version=version, artifacts=artifacts),
                       sort_keys=True, separators=(",", ":")) + "\n").encode()


def sign_manifest(manifest: bytes, pem: str, public_key: str) -> bytes:
    openssl = os.environ.get("OPENSSL_BIN", "openssl")
    expected = base64.b64decode(public_key, validate=True)
    if len(expected) != 32:
        raise ValueError("Expected a raw 32-byte Ed25519 public key")
    with tempfile.TemporaryDirectory(prefix="openscp-update-sign-") as directory:
        root = Path(directory)
        private = root / "key.pem"
        private.write_text(pem)
        private.chmod(0o600)
        public = subprocess.check_output(
            [openssl, "pkey", "-in", str(private), "-pubout", "-outform", "DER"],
            stderr=subprocess.DEVNULL)
        # RFC 8410 Ed25519 SubjectPublicKeyInfo: algorithm OID plus 32 raw bytes.
        if public != bytes.fromhex("302a300506032b6570032100") + expected:
            raise ValueError("The signing key does not match the embedded public key")
        source = root / "manifest.json"
        source.write_bytes(manifest)
        signature = subprocess.check_output(
            [openssl, "pkeyutl", "-sign", "-rawin", "-inkey", str(private), "-in", str(source)],
            stderr=subprocess.DEVNULL)
        if len(signature) != 64:
            raise ValueError("Invalid Ed25519 signature length")
        return signature


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--assets", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    manifest = build_manifest(args.assets, args.version)
    signature = sign_manifest(manifest, os.environ["OPENSCP_UPDATE_PRIVATE_KEY"],
                              os.environ["OPENSCP_UPDATE_PUBLIC_KEY"])
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "update-manifest.json").write_bytes(manifest)
    (args.output / "update-manifest.sig").write_bytes(signature)


if __name__ == "__main__":
    main()
