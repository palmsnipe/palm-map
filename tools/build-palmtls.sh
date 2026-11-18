#!/usr/bin/env bash
# Build the negotiated HTTPS engine omitted by the public v0.1.9 Modern PRC.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
tls=$(cd "${PALM_TLS_ROOT:-$root/../palm-tls}" && pwd)
toolchain=$(cd "${PALM_TOOLCHAIN_ROOT:-$root/../palm-toolchain}" && pwd)
prefix=${PALM_TOOLCHAIN_PREFIX:-"$toolchain/.toolchain/prefix"}
deps=${PALM_TLS_DEPS_PREFIX:-"$tls/.palm-tls/prefix"}
revision=1f122605fd32c21e3fb32fc494e3859c5e1233df
if [[ $(git -C "$tls" rev-parse 'v0.1.9^{commit}') != "$revision" ]]; then
    echo "PalmTLS checkout must contain the upstream v0.1.9 tag at $revision" >&2
    exit 1
fi
if [[ ! -f "$deps/lib/libwolfssl-browser-armlet.a" ]]; then
    echo "Bootstrap wolfSSL with 'make bootstrap' in a PalmTLS v0.1.9 checkout first." >&2
    exit 1
fi
dest="$root/build/deps/palm-tls-v0.1.9"
mkdir -p "$dest"
git -C "$tls" archive "$revision" | tar -x -C "$dest"
make -C "$dest/library" TLS_PROFILE=browser PALM_TLS_TEST_RANDOM=0 \
    PALM_TOOLCHAIN_PREFIX="$prefix" PALM_TLS_DEPS_PREFIX="$deps" test-prc
cp "$dest/library/build/palm/PalmTLSArm-Browser.prc" "$root/build/PalmTLSArm-Browser.prc"
