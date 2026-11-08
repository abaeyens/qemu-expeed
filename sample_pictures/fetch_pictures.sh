#!/usr/bin/env bash
# Fetch the sample pictures used by the emulated camera's sensor model
# Required for live view and stills capture emulation to work
# Please do not redistribute these pictures
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
BASE_URL="https://files.arnebaeyens.com:2202/qemu-expeed"

# One line per picture: <file name> <sha256>
PICTURES="
canoe.NEF   90dbc687cfc611dc1991ea948a0b29e53fc77fe145ec24fbd8e4e2d0acac213d
"

while read -r name sha256; do
    [ -n "$name" ] || continue
    if [ ! -f "$HERE/$name" ]; then
        echo "fetching $name"
        curl -fS --progress-bar -o "$HERE/$name.tmp" "$BASE_URL/$name"
        mv "$HERE/$name.tmp" "$HERE/$name"
    fi
    echo "$sha256  $HERE/$name" | sha256sum -c -
done <<< "$PICTURES"
