#!/usr/bin/env bash
# Build the PalmTLS 0.1.9 ARM bundle with SHA-384 and 4096-bit RSA certificate support in TLS 1.2.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
tls=$(cd "${PALM_TLS_ROOT:-$root/../palm-tls}" && pwd)
toolchain=$(cd "${PALM_TOOLCHAIN_ROOT:-$root/../palm-toolchain}" && pwd)
prefix=${PALM_TOOLCHAIN_PREFIX:-"$toolchain/.toolchain/prefix"}
state=${PALM_TLS_STATE:-"$tls/.palm-tls"}
deps=${PALM_TLS_DEPS_PREFIX:-"$state/prefix"}
wolf="$state/src/wolfssl"
revision=1f122605fd32c21e3fb32fc494e3859c5e1233df
wolf_revision=3c5eead44904df64e6a5a1f4ebdce377d35a849a
if [[ $(git -C "$tls" rev-parse 'v0.1.9^{commit}') != "$revision" ]]; then
    echo "PalmTLS checkout must contain the upstream v0.1.9 tag at $revision" >&2
    exit 1
fi
if [[ ! -x "$wolf/configure" || ! -f "$deps/lib/libwolfssl-tls13-armlet.a" ||
      ! -f "$deps/lib/libwolfssl-tls13.a" ||
      $(git -C "$wolf" rev-parse HEAD) != "$wolf_revision" ]]; then
    echo "Run 'make bootstrap' from a PalmTLS v0.1.9 checkout first." >&2
    exit 1
fi
dest="$root/build/deps/palm-tls-compat-v0.1.9"
mkdir -p "$dest/deps/lib" "$dest/wolfssl-build"
git -C "$tls" archive "$revision" | tar -x -C "$dest"
ln -sfn "$deps/include" "$dest/deps/include"
for archive in libwolfssl-tls13-armlet.a libwolfssl-tls13.a; do
    ln -sfn "$deps/lib/$archive" "$dest/deps/lib/$archive"
done
python3 - "$dest" "$wolf" <<'PY'
import pathlib,sys
source=pathlib.Path(sys.argv[1])
path=source / "wolfssl/user_settings.h"
text=path.read_text()
old='#if !defined(WOLFSSL_NO_TLS12) && !defined(PALM_WOLFSSL_BROWSER)\n#define NO_SHA512'
new='''#ifdef PALM_WOLFSSL_TLS12_ONLY
#define WOLFSSL_SHA384
#define WOLFSSL_PALM_16BIT_INT64
#define USE_SLOW_SHA512
#define FP_MAX_BITS 8192
#elif !defined(WOLFSSL_NO_TLS12) && !defined(PALM_WOLFSSL_BROWSER)
#define NO_SHA512'''
assert text.count(old)==1
path.write_text(text.replace(old,new))

# ecc_point embeds the bignum type. Compile the TLS 1.2 bridge with the same
# FP_MAX_BITS as the matching engine, while leaving the TLS 1.3 ABI alone.
path=source / "library/Makefile"
text=path.read_text()
old="OBJECTS_12_ARM := $(COMMON_OBJECTS_ARM) $(LEGACY_RANDOM) $(PALM_BUILD)/tls_engine_12.o"
assert text.count(old)==1
new="OBJECTS_12_ARM := $(COMMON_OBJECTS) $(PALM_BUILD)/tls_armlet_12.o $(LEGACY_RANDOM) $(PALM_BUILD)/tls_engine_12.o"
text=text.replace(old,new)
start=text.index("$(PALM_BUILD)/tls_armlet.o:")
end=text.index("$(PALM_BUILD)/tls_armlet_browser.o:",start)
rule=text[start:end].replace("tls_armlet.o:", "tls_armlet_12.o:").replace(
    "$(COMMON_FLAGS)) -c", "$(COMMON_FLAGS)) -DPALM_WOLFSSL_TLS12_ONLY -c")
path.write_text(text[:end]+rule+text[end:])

# Use the upstream Browser profile's public-RSA ARM fast path in TLS 1.2
# as well. Only these two callers and the matching TFM reduction change.
bridge=source / "library/src/tls_armlet.c"
guard="defined(PALM_WOLFSSL_BROWSER) && defined(WOLFSSL_RSA_VERIFY_ONLY)"
text=bridge.read_text()
assert text.count(guard)==2
bridge.write_text(text.replace(guard,
    "(defined(PALM_WOLFSSL_BROWSER) || defined(PALM_WOLFSSL_TLS12_ONLY)) && defined(WOLFSSL_RSA_VERIFY_ONLY)"))

wolf_src=pathlib.Path(sys.argv[2]) / "wolfcrypt/src"
target=source / "wolfssl-build/wolfcrypt/src"
target.mkdir(parents=True,exist_ok=True)
text=(wolf_src / "tfm.c").read_text()
guard="defined(PALM_WOLFSSL_BROWSER) && defined(PALM_WOLFSSL_ARMLET_MATH)"
assert text.count(guard)==1
(target / "tfm.c").write_text(text.replace(guard,
    "(defined(PALM_WOLFSSL_BROWSER) || defined(PALM_WOLFSSL_TLS12_ONLY)) && defined(PALM_WOLFSSL_ARMLET_MATH)"))
(target / "fp_mont_small.i").write_bytes((wolf_src / "fp_mont_small.i").read_bytes())
PY
(
    cd "$dest/wolfssl-build"
    CC="$prefix/bin/m68k-none-elf-gcc" AR="$prefix/bin/m68k-none-elf-ar" \
    RANLIB="$prefix/bin/m68k-none-elf-ranlib" \
    CFLAGS='-std=gnu17 -Os -fno-strict-aliasing -fwrapv -m68000 -mno-align-int -fshort-enums -mshort -ffunction-sections -fdata-sections' \
    CPPFLAGS="-DWOLFSSL_USER_SETTINGS -DPALM_WOLFSSL_TLS12_ONLY -DPALM_WOLFSSL_ENABLE_ARMLET_MATH -I$dest/wolfssl" \
    "$wolf/configure" --host=m68k-none-elf --enable-usersettings \
        --disable-shared --enable-static --disable-examples --disable-crypttests \
        --enable-16bit --enable-singlethreaded --enable-smallstack \
        --disable-filesystem --disable-tls13 --disable-oldtls \
        --disable-sp-math --disable-sp-math-all --enable-fastmath \
        --disable-rsa --disable-rsapub --enable-ecc --disable-dh --disable-mlkem \
        --disable-sha3 --disable-sha512 --enable-sha384 --disable-sha224 \
        --disable-shake128 --disable-shake256 --disable-chacha \
        --disable-poly1305 --disable-pkcs8 --disable-pkcs12
    make -j"${JOBS:-4}" src/libwolfssl.la
    cp src/.libs/libwolfssl.a "$dest/deps/lib/libwolfssl-armlet.a"
)
prc="$dest/library/build/palm/PalmTLSArm-Modern.prc"
make -C "$dest/library" -j"${JOBS:-4}" TLS_PROFILE=modern PALM_TLS_TEST_RANDOM=0 \
    PALM_TOOLCHAIN_PREFIX="$prefix" PALM_TLS_DEPS_PREFIX="$dest/deps" \
    BASE_ELF_68K="$dest/library/build/palm/68k/library-tls13.elf" "$prc"
node "$dest/library/tools/validate_library.mjs" "$prc" arm modern 0.1.9 1 no-test-seed
cp "$prc" "$root/build/PalmTLSArm-TLS12-Compat.prc"
