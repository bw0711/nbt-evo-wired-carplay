#!/bin/sh
# Mac (or Linux) host tests under AddressSanitizer + UBSan: no head unit, no Docker. Run before every change goes
# to a car. Needs clang (Xcode command line tools) and scripts/fetch-deps.sh done (lwIP headers).
#   nbt-carplay-evo: auto_test (--auto gate), iap2dev_test (device role towards mm-ipod),
#                    iap2_factory_test (--bridge-start --iap2-factory end to end), ctrl_server (controller server)
#   iap2-adapter:    test_host (chaining, bridge hand-over, boot-loop guard, service start)
# Not here: test-host/bridge_replay (needs your own car recordings: see its header) and the IDEmu runs.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d "${TMPDIR:-/tmp}/nbt-cp-test.XXXXXX")
trap 'rm -rf "$T"' EXIT
SAN="-fsanitize=address,undefined"
export UBSAN_OPTIONS=halt_on_error=1
pass=0

ok() { pass=$((pass + 1)); echo "PASS $1"; }

cd "$ROOT/nbt-carplay-evo"
clang -std=gnu99 -g $SAN -Wall -Wextra -Isrc test-host/auto_test.c -o "$T/auto_test" -lpthread
"$T/auto_test" > "$T/auto.log" 2>&1 || { cat "$T/auto.log"; exit 1; }; ok auto_test

clang -std=gnu99 -g $SAN -Wall -Wextra -Wno-unused-function -DIAP2DEV_TEST -Isrc test-host/iap2dev_test.c \
    -o "$T/iap2dev_test" -lpthread
"$T/iap2dev_test" > "$T/iap2dev.log" 2>&1 || { cat "$T/iap2dev.log"; exit 1; }; ok iap2dev_test

D=$T/iap2ft
mkdir -p "$D/nbt-aa-native"
clang -std=gnu99 -g $SAN -Wall -Wextra -Wno-unused-function -Wno-unused-parameter -DIAP2DEV_TEST -DCP_RAM="\"$D\"" \
    -Isrc test-host/iap2_factory_test.c -o "$T/iap2_factory_test" -lpthread
"$T/iap2_factory_test" > "$T/iap2ft.log" 2>&1 || { cat "$T/iap2ft.log"; exit 1; }; ok iap2_factory_test

clang -std=gnu99 -g $SAN -w -Isrc test-host/ctrl_server.c -o "$T/ctrl_server"
ASAN_OPTIONS=detect_leaks=0 "$T/ctrl_server" > "$T/ctrl.log" 2>&1 || { cat "$T/ctrl.log"; exit 1; }; ok ctrl_server

cd "$ROOT/iap2-adapter"
A=$T/adapter
mkdir -p "$A"
cc -std=gnu99 -g -Wall -Wextra -Werror $SAN -DADAPTER_TEST -DBRIDGE_PORT=47611 -DSTATE_DIR="\"$A\"" -DTEST_STATE="\"$A\"" -DHEALTHY_S=1 \
    -DLOG_PATH="\"$A/log\"" -DSERVICE_PID="\"$A/cpd.pid\"" -o "$T/test_host" test_host.c nbtcp_iap2_adapter.c -lpthread
"$T/test_host" > "$T/adapter.log" 2>&1 || { cat "$T/adapter.log"; exit 1; }; ok adapter_test_host

echo "all $pass host tests passed"
