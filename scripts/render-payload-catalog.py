#!/usr/bin/env python3
"""Render a Payload Manager repository entry for a verified GitHub release."""

import argparse
import json
import re
from pathlib import Path


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--repository", required=True, help="owner/name")
parser.add_argument("--version", required=True)
parser.add_argument("--sha256", required=True)
parser.add_argument("--output", default="payloads.json")
args = parser.parse_args()

if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", args.repository):
    parser.error("--repository must have the form owner/name")
if not re.fullmatch(r"[0-9a-fA-F]{64}", args.sha256):
    parser.error("--sha256 must be a 64-digit hexadecimal digest")

catalog = {
    "name": "RD Downloader",
    "payloads": [
        {
            "name": "RD Downloader",
            "filename": "rd-downloader.elf",
            "url": f"https://github.com/{args.repository}/releases/download/{args.version}/rd-downloader.elf",
            "description": "PS5 HTTP/HTTPS download queue with optional archive extraction.",
            "version": args.version,
            "category": "Downloads",
            "checksum": args.sha256.lower(),
        }
    ],
}
Path(args.output).write_text(json.dumps(catalog, indent=2) + "\n", encoding="utf-8")
