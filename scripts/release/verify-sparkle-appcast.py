#!/usr/bin/env python3
"""Verify Sparkle's archive and feed against the key embedded in the app.

generate_appcast only warns when its signing key does not match SUPublicEDKey.
Do not publish an unusable update: independently verify with the bundle's key.
"""

import argparse
import base64
import os
from pathlib import Path
import plistlib
import re
import subprocess
import tempfile
import xml.etree.ElementTree as ET
import zipfile


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--appcast", type=Path, required=True)
    parser.add_argument("--version", required=True)
    args = parser.parse_args()
    with zipfile.ZipFile(args.archive) as archive:
        plist = plistlib.loads(archive.read("OpenSCP.app/Contents/Info.plist"))
    if plist.get("CFBundleVersion") != args.version or not plist.get("SURequireSignedFeed"):
        raise ValueError("Archive version or signed-feed configuration does not match")
    embedded = plist["SUPublicEDKey"]
    configured = os.environ.get("OPENSCP_SPARKLE_PUBLIC_KEY", embedded)
    if configured != embedded:
        raise ValueError("Archive key does not match the configured public key")
    key = base64.b64decode(embedded, validate=True)
    if len(key) != 32:
        raise ValueError("Invalid embedded Sparkle public key")
    feed = args.appcast.read_bytes()
    footer = re.search(rb"<!-- sparkle-signatures:\s*edSignature: ([A-Za-z0-9+/=]+)\s*length: ([0-9]+)\s*-->\s*$", feed)
    if not footer or int(footer[2]) != footer.start():
        raise ValueError("Missing or ambiguous Sparkle feed signature")
    signed_feed = feed[:footer.start()]
    enclosure = ET.fromstring(signed_feed).find("./channel/item/enclosure")
    if enclosure is None or enclosure.get("length") != str(args.archive.stat().st_size):
        raise ValueError("Appcast does not match the archive")
    expected_url = f"https://github.com/luiscuellar31/openscp/releases/download/v{args.version}/{args.archive.name}"
    if enclosure.get("url") != expected_url:
        raise ValueError("Unexpected appcast download URL")
    openssl = os.environ.get("OPENSSL_BIN", "openssl")
    with tempfile.TemporaryDirectory(prefix="openscp-sparkle-verify-") as directory:
        root = Path(directory)
        public = root / "public.der"
        public.write_bytes(bytes.fromhex("302a300506032b6570032100") + key)
        source = root / "appcast.xml"
        source.write_bytes(signed_feed)
        for data, signature in (
            (source, footer[1]),
            (args.archive, enclosure.attrib["{http://www.andymatuschak.org/xml-namespaces/sparkle}edSignature"].encode()),
        ):
            decoded = base64.b64decode(signature, validate=True)
            if len(decoded) != 64:
                raise ValueError("Invalid Sparkle signature size")
            sigfile = root / "signature"
            sigfile.write_bytes(decoded)
            subprocess.run([openssl, "pkeyutl", "-verify", "-rawin", "-pubin",
                            "-keyform", "DER", "-inkey", str(public),
                            "-in", str(data), "-sigfile", str(sigfile)],
                           check=True, capture_output=True)


if __name__ == "__main__":
    main()
