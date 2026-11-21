#!/usr/bin/env bash
# Fetch the verified PalmTLS ARM Modern release asset used by Map.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
version=v0.1.12
asset=PalmTLSArm-Modern.prc
sha256=98aecca5b4ef440e893bb5b66c882fd07c5d9c17a20a661cac9915a8f5dcbd82
mkdir -p "$root/build"
download=$(mktemp "$root/build/$asset.XXXXXX")
trap '[[ ! -e "$download" ]] || rm -- "$download"' EXIT
curl --fail --location --silent --show-error \
    "https://github.com/palmsnipe/palm-tls/releases/download/$version/$asset" -o "$download"
printf '%s  %s\n' "$sha256" "$download" | shasum -a 256 -c -
mv -- "$download" "$root/build/$asset"
