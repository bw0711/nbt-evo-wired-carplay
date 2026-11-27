#!/bin/sh
# Build nbt_carplay for the NBT Evo (QNX 6.5 ARMv7) in the qnx65-armv7-toolchain:4.9 image.
# Same sources and lists as upstream's scripts/build-qnx.sh (nbt-android-auto, x86), with:
#   lwIP 2.2.1 and Monocypher 4.0.2 from .cache/ (cloned at the commits upstream pins in scripts/carplay_deps.py),
#   Mbed TLS 3.6.7 as built for the probe (../probe-evo/build.sh, same patches and options as upstream),
#   -mno-unaligned-access (the head unit does not fix up unaligned accesses).
# Output: out/nbt_carplay (+ .stripped), checked against libc.so.3 / libusbdi.so.2 / libsocket.so.3 of QNX 6.5.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
IMAGE=${QNX_IMAGE:-qnx65-armv7-toolchain:4.9}
LW=$HERE/.cache/lwip-STABLE-2_2_1_RELEASE
MO=$HERE/.cache/monocypher-4.0.2
MB=$HERE/../probe-evo/.cache/mbedtls-3.6.7
[ "$(git -C "$LW" rev-parse HEAD)" = 77dcd25a72509eb83f72b033d219b1d40cd8eb95 ] || { echo "lwIP: wrong or missing checkout $LW" >&2; exit 1; }
[ "$(git -C "$MO" rev-parse HEAD)" = 0d85f98c9d9b0227e42cf795cb527dff372b40a4 ] || { echo "Monocypher: wrong or missing checkout $MO" >&2; exit 1; }
[ -f "$MB/library/libmbedcrypto.a" ] || { echo "Mbed TLS for ARM not built: run ../probe-evo/build.sh first" >&2; exit 1; }
mkdir -p "$HERE/out"

INNER=$(cat <<'EOF'
G=arm-unknown-nto-qnx6.5.0eabi
CF="-O2 -std=gnu99 -D_QNX_SOURCE -mno-unaligned-access"
LW=/lwip/src
INC="-I/w/src -I/w/src/lwip_port -I$LW/include -I/mono/src -I/mono/src/optional -I/mbed/include"
O=/tmp/obj; mkdir -p $O
# third-party code without warnings
for c in $LW/core/init.c $LW/core/def.c $LW/core/inet_chksum.c $LW/core/ip.c $LW/core/mem.c $LW/core/memp.c \
         $LW/core/netif.c $LW/core/pbuf.c $LW/core/sys.c $LW/core/tcp.c $LW/core/tcp_in.c $LW/core/tcp_out.c \
         $LW/core/timeouts.c $LW/core/udp.c $LW/core/ipv6/ethip6.c $LW/core/ipv6/icmp6.c $LW/core/ipv6/inet6.c \
         $LW/core/ipv6/ip6.c $LW/core/ipv6/ip6_addr.c $LW/core/ipv6/ip6_frag.c $LW/core/ipv6/mld6.c \
         $LW/core/ipv6/nd6.c $LW/api/api_lib.c $LW/api/api_msg.c $LW/api/err.c $LW/api/netbuf.c $LW/api/sockets.c \
         $LW/api/tcpip.c $LW/netif/ethernet.c /mono/src/monocypher.c /mono/src/optional/monocypher-ed25519.c; do
    $G-gcc $CF -w $INC -c $c -o $O/$(basename $c .c).o
done
cd /w/src
for c in nbt_carplay cp_util cp_acp cp_usb cp_lockdown cp_iap2 cp_iap2dev cp_auto cp_ncm cp_ctrl cp_net_lwip cp_bplist cp_crypto cp_airplay \
         cp_screen cp_audio cp_mic cp_input lwip_port/sys_arch; do
    $G-gcc $CF -Wall -Wextra $INC -c $c.c -o $O/cp-$(basename $c).o
done
$G-gcc $O/*.o /mbed/library/libmbedtls.a /mbed/library/libmbedx509.a /mbed/library/libmbedcrypto.a \
    -lusbdi -lsocket -o /w/out/nbt_carplay
T=$QNX_TARGET/armle-v7/lib
: > /tmp/defined
for lib in libc.so.3 libusbdi.so.2 libsocket.so.3; do
    $G-nm -D --defined-only $T/$lib | awk 'NF>=3 {print $NF}' >> /tmp/defined
done
sort -u /tmp/defined -o /tmp/defined
missing=$($G-nm -D -u /w/out/nbt_carplay | awk '$1=="U" {print $2}' | sed 's/@.*//' | sort -u | comm -23 - /tmp/defined)
[ -z "$missing" ] || { echo "no provider for:"; echo "$missing"; exit 1; }
$G-readelf -d /w/out/nbt_carplay | grep NEEDED
$G-strip -o /w/out/nbt_carplay.stripped /w/out/nbt_carplay
ls -l /w/out/
EOF
)
docker run --rm --platform=linux/amd64 -v "$HERE":/w -v "$LW":/lwip:ro -v "$MO":/mono:ro -v "$MB":/mbed:ro "$IMAGE" sh -eu -c "$INNER"
echo "Built out/nbt_carplay (import check passed)."
