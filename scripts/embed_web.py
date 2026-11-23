#!/usr/bin/env python3
"""Embed the dashboard HTML in a C translation unit as a string literal."""
import json
import pathlib
import sys

source, destination = map(pathlib.Path, sys.argv[1:3])
html = source.read_text(encoding="utf-8")
with destination.open("w", encoding="utf-8") as output:
    output.write("/* Generated from web/index.html by scripts/embed_web.py. */\n")
    output.write("static const char index_body[] =\n")
    for line in html.splitlines(keepends=True):
        output.write("    " + json.dumps(line, ensure_ascii=False) + "\n")
    output.write(";\n")
