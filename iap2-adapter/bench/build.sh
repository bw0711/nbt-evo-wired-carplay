#!/bin/sh
# IDEmu bench for the wired iAP2 chain: testnext.so, fakeplugin, fakeplugin2, mmipod_node, fakeacp, iap2dev_run (QNX 6.5 ARMv7), into out/.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
mkdir -p "$HERE/out"
docker run --rm --platform=linux/amd64 -v "$ROOT":/w "${QNX_IMAGE:-qnx65-armv7-toolchain:4.9}" sh -eu -c '
G=arm-unknown-nto-qnx6.5.0eabi
CF="-O2 -std=gnu99 -D_QNX_SOURCE -mno-unaligned-access -Wall -Wextra"
cd /w/iap2-adapter/bench
$G-gcc $CF -fPIC -shared -o out/testnext.so testnext.c
$G-gcc $CF -o out/fakeplugin fakeplugin.c
$G-gcc $CF -Wl,-E -o out/fakeplugin2 fakeplugin2.c
$G-gcc $CF -o out/mmipod_node mmipod_node.c
$G-gcc $CF -o out/fakeacp fakeacp.c
$G-gcc $CF -Wno-unused-function -I../../nbt-carplay-evo/src -o out/iap2dev_run iap2dev_run.c -lsocket
ls -l out/
'
