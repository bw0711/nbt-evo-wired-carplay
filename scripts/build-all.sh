#!/bin/sh
# Build everything that goes onto the head unit, in the order the builds depend on each other:
#   1. probe-evo        (also builds Mbed TLS 3.6.7 for ARM, which nbt-carplay-evo links)
#   2. nbt-carplay-evo  -> out/nbt_carplay.stripped   (the bridge / service)
#   3. iap2-adapter     -> out/nbtcp_iap2_adapter.so
#   4. conn-patch       -> out/conn_patch             (needs copies of YOUR head unit's Connectivity and
#                          libconn_carplay.so: car/car-chip-test.sh conn-pull first; skipped with a note if missing)
# Needs Docker with the image qnx65-armv7-toolchain:4.9 (README.md, "Dependencies") and scripts/fetch-deps.sh done.
# Prints the SHA-256 of each result: compare with the table in README.md.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
docker image inspect "${QNX_IMAGE:-qnx65-armv7-toolchain:4.9}" > /dev/null 2>&1 || {
    echo "Docker image ${QNX_IMAGE:-qnx65-armv7-toolchain:4.9} not found: build it first (README.md, Dependencies)" >&2
    exit 1
}
"$ROOT/probe-evo/build.sh"
"$ROOT/nbt-carplay-evo/build.sh"
"$ROOT/iap2-adapter/build.sh"
if ! "$ROOT/conn-patch/build.sh"; then
    echo
    echo "conn-patch NOT built (see above). Without it the service runs CarPlay but no track names on the cluster;"
    echo "car-chip-test.sh auto-install refuses until it exists."
fi
echo
echo "== results"
cd "$ROOT"
for f in nbt-carplay-evo/out/nbt_carplay.stripped iap2-adapter/out/nbtcp_iap2_adapter.so conn-patch/out/conn_patch car/autostart/cpd.sh; do
    [ -f "$f" ] && shasum -a 256 "$f" || echo "missing  $f"
done
