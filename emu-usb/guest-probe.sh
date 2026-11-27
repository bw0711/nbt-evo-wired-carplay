#!/bin/bash
# Run one nbt_carplay_probe_evo step inside the IDEmu guest without hitting the console bridge's 30 s limit:
# the step runs in the background in the guest with its output in /dev/shmem/probe-<step>.log, and this script
# reads that file with short commands until the step has finished (or MAX seconds have passed).
#
#   wired-carplay/emu-usb/guest-probe.sh lockdown [MAX=180]
#   wired-carplay/emu-usb/guest-probe.sh "usb carplay"
#   NBT_ENV="NBT_ACP_NO_SIGN=1" wired-carplay/emu-usb/guest-probe.sh acp
#
# Needs the unit running (emulators/emu-usb-server.sh for USB) and the probe staged as
# IDEmu/idemu/plugins-car/_tools/nbt_carplay_probe_evo (on the shim disk: /fs/shim/carplug/_tools/).
set -u
STEP=${1:?step: acp | usb | "usb carplay" | lockdown | carkit}
MAX=${2:-180}
HERE=$(cd "$(dirname "$0")" && pwd)
CFG=${IDEMU_SSH_CONFIG:-$HERE/../../emulators/IDEmu/environments/f30-business-24-03/ssh/ssh_config}
P=/fs/shim/carplug/_tools/nbt_carplay_probe_evo
TAG=$(echo "$STEP" | tr ' ' '-')
LOG=/dev/shmem/probe-$TAG.log
g() { ssh -F "$CFG" idemu "$1" 2>&1; }
g "rm -f $LOG; (${NBT_ENV:+$NBT_ENV }$P $STEP; echo PROBE_RC=\$?) > $LOG 2>&1 &" >/dev/null
t=0
while [ $t -lt "$MAX" ]; do
    sleep 3; t=$((t + 3))
    if g "grep -c PROBE_RC= $LOG" | grep -q '^1$'; then break; fi
done
g "cat $LOG" | grep -v '^__IDEMU' | grep -v '^echo __IDEMU'
[ $t -lt "$MAX" ] || echo "guest-probe: still running after $MAX s (log: $LOG in the guest)"
