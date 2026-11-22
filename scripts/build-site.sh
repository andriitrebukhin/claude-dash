#!/usr/bin/env bash
# Assemble the browser installer (GitHub Pages) into _site/ from dist/.
# Run scripts/build-firmware.sh first. Usage: VERSION=v1.2.3 scripts/build-site.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="${VERSION:-$(git -C "$ROOT" describe --tags --always --dirty 2>/dev/null || echo dev)}"
SITE="$ROOT/_site"
FULL="$ROOT/dist/claude-dash-$VERSION-full.bin"
[ -f "$FULL" ] || { echo "missing $FULL - run scripts/build-firmware.sh first" >&2; exit 1; }

rm -rf "$SITE" && mkdir -p "$SITE/firmware"
cp "$ROOT/site/index.html" "$SITE/"
cp "$ROOT/docs/dashboard.png" "$SITE/"
cp "$ROOT/site/stand.html" "$SITE/"
cp "$ROOT/case/renders/ribbon_hero.png" "$ROOT/case/renders/halo_hero.png" "$SITE/"
cp "$FULL" "$SITE/firmware/claude-dash-full.bin"
cat > "$SITE/manifest.json" <<JSON
{
  "name": "Claude Dash",
  "version": "$VERSION",
  "new_install_prompt_erase": false,
  "new_install_improv_wait_time": 20,
  "builds": [
    {
      "chipFamily": "ESP32-S3",
      "parts": [{ "path": "firmware/claude-dash-full.bin", "offset": 0 }]
    }
  ]
}
JSON
echo "Site for $VERSION in $SITE"
