#!/usr/bin/env python3
"""Copy a built PS5 ELF into the repository and render its Payload Manager catalog."""

import argparse
import hashlib
import json
import re
import shutil
from pathlib import Path


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--repository", required=True, help="owner/name")
parser.add_argument("--version", required=True)
parser.add_argument("--artifact", type=Path, help="built ELF to publish under distribution/")
parser.add_argument("--sha256", help="expected SHA-256; checked when --artifact is supplied")
parser.add_argument("--output", default="payloads.json")
args = parser.parse_args()

if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", args.repository):
    parser.error("--repository must have the form owner/name")
if not re.fullmatch(r"v?[0-9A-Za-z.+-]+", args.version):
    parser.error("--version contains invalid characters")
if args.artifact is None and not args.sha256:
    parser.error("provide --artifact or --sha256")

if args.artifact is not None:
    artifact = args.artifact.resolve(strict=True)
    with artifact.open("rb") as source:
        if source.read(4) != b"\x7fELF":
            parser.error(f"not an ELF file: {artifact}")
    destination = Path("distribution/rd-downloader.elf")
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(artifact, destination)
    digest = hashlib.sha256(destination.read_bytes()).hexdigest()
    if args.sha256 and digest.lower() != args.sha256.lower():
        parser.error("artifact SHA-256 does not match --sha256")
else:
    digest = args.sha256
if not re.fullmatch(r"[0-9a-fA-F]{64}", digest):
    parser.error("SHA-256 must be a 64-digit hexadecimal digest")

catalog = {
    "name": "RD Downloader",
    "payloads": [
        {
            "name": "RD Downloader",
            "filename": "rd-downloader.elf",
            "url": f"https://raw.githubusercontent.com/{args.repository}/main/distribution/rd-downloader.elf",
            "description": "PS5 HTTP/HTTPS download queue with optional archive extraction.",
            "version": args.version,
            "category": "Downloads",
            "checksum": digest.lower(),
        }
    ],
}
Path(args.output).write_text(json.dumps(catalog, indent=2) + "\n", encoding="utf-8")
