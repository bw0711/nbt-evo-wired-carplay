#!/bin/sh
# Build conn_patch for the NBT Evo (QNX 6.5 ARMv7) in the qnx65-armv7-toolchain:4.9 image, plus a selftest pair for an
# IDEmu guest: selftest/patch_target (a stand-in process) and out/conn_patch_selftest (the same code with a table that
# patches patch_target instead of Connectivity).
# Output: out/conn_patch, out/patch_target, out/conn_patch_selftest (stripped), checked against libc.so.3 of QNX 6.5.
#
# The patch table (out/conn_patch_tab.h) is made by gen_tab.py from copies of YOUR head unit's Connectivity and
# libconn_carplay.so (head-unit code, never in the repository): pull them first with the read-only stage
#   ../car/car-chip-test.sh conn-pull
# CONN_FILES=<folder> names the folder; the default is the newest car/runs/*/files-conn that has both files.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
IMAGE=${QNX_IMAGE:-qnx65-armv7-toolchain:4.9}
mkdir -p "$HERE/out"
CX=opt_conn_bin_Connectivity
CL=opt_conn_lib_carplay_devctrl_libconn_carplay.so
F=${CONN_FILES:-}
if [ -z "$F" ]; then
    for d in $(ls -1d "$HERE"/../car/runs/*/files-conn 2>/dev/null | sort -r); do
        if [ -f "$d/$CX" ] && [ -f "$d/$CL" ]; then F=$d; break; fi
    done
fi
[ -n "$F" ] && [ -f "$F/$CX" ] && [ -f "$F/$CL" ] || {
    echo "No copies of the head unit's $CX and $CL: run ../car/car-chip-test.sh conn-pull (read-only), or set CONN_FILES" >&2
    exit 1
}
echo "patch table from $F"
python3 -I "$HERE/gen_tab.py" "$F/$CX" "$F/$CL" > "$HERE/out/conn_patch_tab.h"
[ ! -e "$HERE/conn_patch_tab.h" ] || { echo "remove $HERE/conn_patch_tab.h: the table now lives in out/" >&2; exit 1; }
docker run --rm --platform=linux/amd64 -v "$HERE":/w "$IMAGE" sh -eu -c '
G=arm-unknown-nto-qnx6.5.0eabi
CF="-O2 -std=gnu99 -D_QNX_SOURCE -mno-unaligned-access -Wall -Wextra -Werror"
cd /w
$G-gcc $CF -marm -o out/patch_target selftest/patch_target.c
$G-gcc $CF -marm -fPIC -shared -o out/libpt.so selftest/libpt.c
$G-nm -D out/libpt.so | awk "\$3==\"answer2\" {print \$1}" > out/answer2.addr
$G-gcc $CF -Iout -o out/conn_patch conn_patch.c
$G-nm out/patch_target | awk "\$3==\"answer\" {print \$1}" > out/answer.addr
T=$QNX_TARGET/armle-v7/lib
$G-nm -D --defined-only $T/libc.so.3 | awk "NF>=3 {print \$NF}" | sort -u > /tmp/defined
missing=$($G-nm -D -u out/conn_patch | awk "\$1==\"U\" {print \$2}" | sed "s/@.*//" | sort -u | comm -23 - /tmp/defined)
[ -z "$missing" ] || { echo "no provider for: $missing"; exit 1; }
'
python3 -I "$HERE/gen_tab.py" --selftest "$HERE/out/patch_target" "$(cat "$HERE/out/answer.addr")" \
    "$HERE/out/libpt.so" "$(cat "$HERE/out/answer2.addr")" > "$HERE/out/selftest_tab.h"
docker run --rm --platform=linux/amd64 -v "$HERE":/w "$IMAGE" sh -eu -c '
G=arm-unknown-nto-qnx6.5.0eabi
cd /w
$G-gcc -O2 -std=gnu99 -D_QNX_SOURCE -mno-unaligned-access -Wall -Wextra -Werror -Iout -DCONN_PATCH_SELFTEST \
    -o out/conn_patch_selftest conn_patch.c
for f in conn_patch conn_patch_selftest patch_target libpt.so; do $G-strip out/$f; done
ls -l out/
'
shasum -a 256 "$HERE/out/conn_patch"
