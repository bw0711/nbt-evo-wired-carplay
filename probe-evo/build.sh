#!/bin/sh
# Build nbt_carplay_probe_evo for the NBT Evo (QNX 6.5 ARMv7) in the qnx65-armv7-toolchain:4.9 image.
#
# Mbed TLS 3.6.7 comes from its GitHub release (SHA-256 checked, cached in .cache/) with the same two x509 patches
# and disabled options as upstream's scripts/build_all.py (nbt-android-auto, ../upstream/nbt-android-auto).
# Output: out/nbt_carplay_probe_evo, plus an import check against the QNX 6.5 target libraries.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
UP=$HERE/../upstream/nbt-android-auto
V=3.6.7
SHA=a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6
CACHE=$HERE/.cache
IMAGE=${QNX_IMAGE:-qnx65-armv7-toolchain:4.9}
mkdir -p "$CACHE" "$HERE/out"

TB=$CACHE/mbedtls-$V.tar.bz2
[ -f "$TB" ] || curl -fsSL -o "$TB" "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$V/mbedtls-$V.tar.bz2"
echo "$SHA  $TB" | shasum -a 256 -c - >/dev/null || { echo "Mbed TLS archive: SHA-256 mismatch" >&2; exit 1; }

M=$CACHE/mbedtls-$V
if [ ! -f "$M/.patched" ]; then
    rm -rf "$M"
    tar -xjf "$TB" -C "$CACHE"
    python3 -I - "$UP/scripts" "$M" <<'PY'
import re, sys
sys.path.insert(0, sys.argv[1])
from mbedtls_legacy_time import patch_x509
from mbedtls_empty_name import patch_x509_empty_name
m = sys.argv[2]
p = m + "/library/x509.c"
src = open(p).read()
src = patch_x509_empty_name(patch_x509(src))
open(p, "w").write(src)
p = m + "/include/mbedtls/mbedtls_config.h"
c = open(p).read()
for option in ("MBEDTLS_AESNI_C", "MBEDTLS_HAVE_ASM", "MBEDTLS_PADLOCK_C"):
    c, n = re.subn(r"^#define " + option + r"\s*$", "/* QNX GCC 4.x: disabled " + option + " */", c, flags=re.M)
    if n != 1:
        raise SystemExit("Mbed TLS config: expected exactly one " + option)
open(p, "w").write(c)
PY
    touch "$M/.patched"
fi

INNER=$(cat <<'EOF'
G=arm-unknown-nto-qnx6.5.0eabi
CF="-O2 -std=gnu99 -D_QNX_SOURCE -mno-unaligned-access"
cd /m/library
if [ ! -f libmbedcrypto.a ]; then
    make -s CC=$G-gcc AR=$G-ar CFLAGS="$CF -include stddef.h -I../include -w" libmbedtls.a libmbedx509.a libmbedcrypto.a >/tmp/mbed.log 2>&1 \
        || { tail -30 /tmp/mbed.log; exit 1; }
fi
cd /w
$G-gcc $CF -Wall -Wextra -I/m/include nbt_carplay_probe_evo.c \
    /m/library/libmbedtls.a /m/library/libmbedx509.a /m/library/libmbedcrypto.a -lusbdi -lsocket \
    -o out/nbt_carplay_probe_evo
# import check: every undefined symbol must come from libc.so.3, libusbdi.so.2 or libsocket.so.3 of QNX 6.5
T=$QNX_TARGET/armle-v7/lib
: > /tmp/defined
for lib in libc.so.3 libusbdi.so.2 libsocket.so.3; do
    $G-nm -D --defined-only $T/$lib | awk 'NF>=3 {print $NF}' >> /tmp/defined
done
sort -u /tmp/defined -o /tmp/defined
missing=$($G-nm -D -u out/nbt_carplay_probe_evo | awk '$1=="U" {print $2}' | sed 's/@.*//' | sort -u | comm -23 - /tmp/defined)
[ -z "$missing" ] || { echo "no provider for:"; echo "$missing"; exit 1; }
$G-readelf -d out/nbt_carplay_probe_evo | grep NEEDED
$G-strip -o out/nbt_carplay_probe_evo.stripped out/nbt_carplay_probe_evo
ls -l out/
EOF
)
docker run --rm --platform=linux/amd64 -v "$HERE":/w -v "$M":/m "$IMAGE" sh -eu -c "$INNER"
echo "Built out/nbt_carplay_probe_evo (import check passed)."
