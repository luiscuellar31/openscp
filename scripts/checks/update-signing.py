#!/usr/bin/env python3
"""Offline regression check for release-manifest signing with disposable keys."""

import base64
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main() -> None:
    root = Path(__file__).resolve().parents[2]
    script = root / "scripts/release/update-manifest.py"
    spec = importlib.util.spec_from_file_location("update_manifest", script)
    module = importlib.util.module_from_spec(spec)
    sys.dont_write_bytecode = True
    spec.loader.exec_module(module)
    openssl = os.environ.get("OPENSSL_BIN", "openssl")
    with tempfile.TemporaryDirectory(prefix="openscp-signing-check-") as directory:
        working = Path(directory)
        key = working / "test-key.pem"
        subprocess.run([openssl, "genpkey", "-algorithm", "ED25519", "-out", str(key)],
                       check=True, capture_output=True)
        public_der = subprocess.check_output(
            [openssl, "pkey", "-in", str(key), "-pubout", "-outform", "DER"])
        public = base64.b64encode(public_der[-32:]).decode()
        assets = working / "assets"
        assets.mkdir()
        for arch in ("x86_64", "aarch64"):
            (assets / f"OpenSCP-1.2.0-{arch}.AppImage").write_bytes(b"test artifact " + arch.encode())
        output = working / "signed"
        env = dict(os.environ, OPENSCP_UPDATE_PRIVATE_KEY=key.read_text(),
                   OPENSCP_UPDATE_PUBLIC_KEY=public)
        subprocess.run(["python3", str(script), "--assets", str(assets),
                        "--version", "1.2.0", "--output", str(output)], check=True, env=env)
        manifest = output / "update-manifest.json"
        signature = output / "update-manifest.sig"
        contents = json.loads(manifest.read_bytes())
        assert contents["version"] == "1.2.0"
        assert len(contents["artifacts"]) == 2
        assert signature.stat().st_size == 64
        pubfile = working / "public.der"
        pubfile.write_bytes(public_der)
        verify = [openssl, "pkeyutl", "-verify", "-rawin", "-pubin", "-keyform", "DER",
                  "-inkey", str(pubfile), "-in", str(manifest), "-sigfile", str(signature)]
        subprocess.run(verify, check=True, capture_output=True)
        manifest.write_bytes(manifest.read_bytes() + b" ")
        assert subprocess.run(verify, capture_output=True).returncode != 0
        try:
            module.sign_manifest(b"test", key.read_text(), base64.b64encode(bytes(32)).decode())
        except ValueError:
            pass
        else:
            raise AssertionError("mismatched signing key must fail")
        (assets / "OpenSCP-1.2.0-aarch64.AppImage").unlink()
        try:
            module.build_manifest(assets, "1.2.0")
        except ValueError:
            pass
        else:
            raise AssertionError("missing architecture must fail")
    print("Update manifest signing checks passed")


if __name__ == "__main__":
    main()
