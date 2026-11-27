#!/bin/sh
# Build nbtcp_iap2_adapter.so for the NBT Evo (QNX 6.5 ARMv7) in the qnx65-armv7-toolchain:4.9 image, and check that it
# exports exactly iAP2WiFiAdapterGet / iAP2WiFiAdapterDispose and imports nothing libc.so.3 / libsocket.so.3 lack.
# Host test first: cc ... test_host.c (see its header).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$HERE/out"
docker run --rm --platform=linux/amd64 -v "$HERE":/w "${QNX_IMAGE:-qnx65-armv7-toolchain:4.9}" sh -eu -c '
G=arm-unknown-nto-qnx6.5.0eabi
cd /w
$G-gcc -O2 -std=gnu99 -D_QNX_SOURCE -mno-unaligned-access -fPIC -shared -Wall -Wextra -Werror \
    -fvisibility=hidden -Wl,-soname,nbtcp_iap2_adapter.so -o out/nbtcp_iap2_adapter.so nbtcp_iap2_adapter.c -lsocket
$G-nm -D --defined-only out/nbtcp_iap2_adapter.so | awk "\$2==\"T\" {print \$3}" | sort > /tmp/exports
echo "exports: $(tr "\n" " " < /tmp/exports)"
T=$QNX_TARGET/armle-v7/lib
for lib in libc.so.3 libsocket.so.3; do $G-nm -D --defined-only $T/$lib | awk "NF>=3 {print \$NF}"; done | sort -u > /tmp/defined
missing=$($G-nm -D -u out/nbtcp_iap2_adapter.so | awk "\$1==\"U\" {print \$2}" | sed "s/@.*//" | sort -u | comm -23 - /tmp/defined)
[ -z "$missing" ] || { echo "no provider for: $missing"; exit 1; }
$G-readelf -d out/nbtcp_iap2_adapter.so | grep -E "NEEDED|SONAME"
$G-strip out/nbtcp_iap2_adapter.so
ls -l out/
'
shasum -a 256 "$HERE/out/nbtcp_iap2_adapter.so"
