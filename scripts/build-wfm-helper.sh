#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
HELPER_DIR="$ROOT/.deps/wfm-7zip-helper"
HELPER_URL="https://github.com/owendswang/wfm-7zip-helper.git"
HELPER_REV="7a8392ab4a743bcc941fc531c2f8c65f8cf57800"
SDK=${PS5_PAYLOAD_SDK:-"$ROOT/.deps/ps5-payload-sdk"}

if [[ ! -d "$HELPER_DIR/.git" ]]; then
    mkdir -p "$ROOT/.deps"
    git clone --filter=blob:none --no-checkout "$HELPER_URL" "$HELPER_DIR"
fi

actual=$(git -C "$HELPER_DIR" rev-parse HEAD)
if [[ "$actual" != "$HELPER_REV" ]]; then
    printf 'WFM helper source must be checked out at %s (found %s).\n' \
        "$HELPER_REV" "$actual" >&2
    printf 'Review local changes before switching the checkout; this script will not discard them.\n' >&2
    exit 1
fi
if [[ -n "$(git -C "$HELPER_DIR" status --porcelain)" ]]; then
    printf 'WFM helper checkout has local modifications; review them before building.\n' >&2
    exit 1
fi

if [[ ! -f "$SDK/toolchain/prospero.mk" ]]; then
    printf 'PS5 Payload SDK not found at %s. Set PS5_PAYLOAD_SDK to its directory.\n' "$SDK" >&2
    exit 1
fi

make -C "$HELPER_DIR/CPP/7zip/Bundles/WfmHelper" \
    -f makefile.ps5 PS5_PAYLOAD_SDK="$SDK"
printf '\nBuilt helper: %s\n' \
    "$HELPER_DIR/CPP/7zip/Bundles/WfmHelper/wfm-7zip-helper.elf"
