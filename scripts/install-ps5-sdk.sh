#!/usr/bin/env bash
set -Eeuo pipefail

PROJECT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
DEPS_DIR="$PROJECT_DIR/.deps"
SDK_DIR="$DEPS_DIR/ps5-payload-sdk"
WORK_DIR="$DEPS_DIR/.sdk-install-$$"
API_URL="https://api.github.com/repos/ps5-payload-dev/sdk/releases/latest"

cleanup() {
    rm -rf -- "$WORK_DIR"
}
trap cleanup EXIT

if [[ "${EUID}" -eq 0 ]]; then
    SUDO=()
else
    command -v sudo >/dev/null 2>&1 || {
        echo "sudo is required to install Debian build packages." >&2
        exit 1
    }
    SUDO=(sudo)
fi

echo "Installing Debian build prerequisites (clang/lld/LLVM 18, socat, make, curl, unzip, Python)..."
"${SUDO[@]}" apt-get update
"${SUDO[@]}" apt-get install -y clang-18 lld-18 llvm-18 socat make curl unzip python3

if [[ -e "$SDK_DIR" ]]; then
    echo "SDK destination already exists: $SDK_DIR" >&2
    echo "Move it aside or remove it before rerunning this installer." >&2
    exit 1
fi

mkdir -p "$DEPS_DIR"
rm -rf -- "$WORK_DIR"
mkdir -p "$WORK_DIR/extract"

echo "Reading the latest release metadata from GitHub..."
curl --fail --location --silent --show-error \
    --proto '=https' --tlsv1.2 \
    "$API_URL" -o "$WORK_DIR/release.json"

readarray -t RELEASE_INFO < <(python3 - "$WORK_DIR/release.json" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as source:
    release = json.load(source)

asset = next((a for a in release.get("assets", [])
              if a.get("name") == "ps5-payload-sdk.zip"), None)
if not asset:
    raise SystemExit("latest SDK release has no ps5-payload-sdk.zip asset")

digest = asset.get("digest", "")
if not digest.startswith("sha256:") or len(digest) != len("sha256:") + 64:
    raise SystemExit("GitHub release metadata has no usable SHA-256 asset digest")

print(release.get("tag_name", "unknown"))
print(asset["browser_download_url"])
print(digest.removeprefix("sha256:"))
PY
)

if [[ "${#RELEASE_INFO[@]}" -ne 3 ]]; then
    echo "Could not read SDK release tag, asset URL, and SHA-256." >&2
    exit 1
fi
SDK_VERSION="${RELEASE_INFO[0]}"
SDK_URL="${RELEASE_INFO[1]}"
SDK_SHA256="${RELEASE_INFO[2]}"

echo "Downloading PS5 Payload SDK $SDK_VERSION..."
curl --fail --location --retry 3 --silent --show-error \
    --proto '=https' --tlsv1.2 \
    "$SDK_URL" -o "$WORK_DIR/ps5-payload-sdk.zip"
printf '%s  %s\n' "$SDK_SHA256" "$WORK_DIR/ps5-payload-sdk.zip" | sha256sum --check --status || {
    echo "SDK archive SHA-256 does not match the digest published by GitHub." >&2
    exit 1
}

python3 - "$WORK_DIR/ps5-payload-sdk.zip" <<'PY'
import stat
import sys
import zipfile

archive_path = sys.argv[1]
with zipfile.ZipFile(archive_path) as archive:
    for entry in archive.infolist():
        name = entry.filename.replace("\\", "/")
        parts = name.split("/")
        mode = entry.external_attr >> 16
        if name.startswith("/") or any(part == ".." for part in parts):
            raise SystemExit(f"unsafe SDK archive path: {entry.filename!r}")
        if stat.S_ISLNK(mode):
            raise SystemExit(f"SDK archive contains an unexpected symlink: {entry.filename!r}")
PY

unzip -q "$WORK_DIR/ps5-payload-sdk.zip" -d "$WORK_DIR/extract"
if [[ ! -f "$WORK_DIR/extract/ps5-payload-sdk/toolchain/prospero.mk" ||
      ! -x "$WORK_DIR/extract/ps5-payload-sdk/bin/prospero-clang" ]]; then
    echo "The downloaded archive does not have the expected PS5 SDK layout." >&2
    exit 1
fi

mv -- "$WORK_DIR/extract/ps5-payload-sdk" "$SDK_DIR"
cat > "$SDK_DIR/INSTALL-METADATA.txt" <<EOF
Release: $SDK_VERSION
SHA-256: $SDK_SHA256
Source: $SDK_URL
EOF

export PS5_PAYLOAD_SDK="$SDK_DIR"
export LLVM_CONFIG="/usr/bin/llvm-config-18"
export PATH="/usr/lib/llvm-18/bin:$PATH"

echo "SDK installed at $SDK_DIR"
echo "Running host tests..."
make -C "$PROJECT_DIR" host-test
echo "Building the PS5 ELF..."
make -C "$PROJECT_DIR"

echo
echo "Build complete: $PROJECT_DIR/rd-downloader.elf"
echo "For future builds, run:"
echo "  export PS5_PAYLOAD_SDK=\"$SDK_DIR\""
echo "  export LLVM_CONFIG=/usr/bin/llvm-config-18"
echo "  export PATH=/usr/lib/llvm-18/bin:\$PATH"
echo "  make -C \"$PROJECT_DIR\""
