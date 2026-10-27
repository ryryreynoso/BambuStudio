#!/bin/bash
# Regenerates the "3D Print" app icon from make_icon.swift (macOS only):
#   resources/Icon.icns                    Dock / Finder icon of the app bundle
#   resources/images/BambuStudio_128px.png window icon
#   resources/images/BambuStudio_192px.png About dialog logo
set -e
set -o pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

swift "$ROOT/scripts/icon/make_icon.swift" "$WORK/icon_1024.png" 1024

ICONSET="$WORK/Icon.iconset"
mkdir -p "$ICONSET"
for px in 16 32 128 256 512; do
    sips -z $px $px "$WORK/icon_1024.png" --out "$ICONSET/icon_${px}x${px}.png" >/dev/null
    sips -z $((px * 2)) $((px * 2)) "$WORK/icon_1024.png" --out "$ICONSET/icon_${px}x${px}@2x.png" >/dev/null
done
iconutil -c icns "$ICONSET" -o "$ROOT/resources/Icon.icns"

sips -z 128 128 "$WORK/icon_1024.png" --out "$ROOT/resources/images/BambuStudio_128px.png" >/dev/null
sips -z 192 192 "$WORK/icon_1024.png" --out "$ROOT/resources/images/BambuStudio_192px.png" >/dev/null
echo "Icon written to resources/Icon.icns"
