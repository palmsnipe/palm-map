#!/usr/bin/env bash
# Fetch the verified PalmTLS ARM Modern release asset used by Map.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
source "$root/tools/palmtls-release.env"
mkdir -p "$root/build"
download=$(mktemp "$root/build/$asset.XXXXXX")
trap '[[ ! -e "$download" ]] || rm -- "$download"' EXIT
curl --fail --location --silent --show-error --retry 3 \
    "https://github.com/palmsnipe/palm-tls/releases/download/$version/$asset" -o "$download"
if command -v sha256sum >/dev/null; then
    printf '%s  %s\n' "$sha256" "$download" | sha256sum -c -
else
    printf '%s  %s\n' "$sha256" "$download" | shasum -a 256 -c -
fi
mv -- "$download" "$root/build/$asset"
