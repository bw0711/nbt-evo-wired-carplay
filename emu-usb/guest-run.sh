#!/bin/bash
# Run any command inside the IDEmu guest without hitting the console bridge's 30 s limit: it runs in the background
# in the guest with its output in /dev/shmem/run-<tag>.log, and this script reads that file with short commands until
# the command has finished (or MAX seconds have passed), then prints it.
#
#   wired-carplay/emu-usb/guest-run.sh TAG "COMMAND" [MAX=180]
#   wired-carplay/emu-usb/guest-run.sh cp-iap2 "NBT_ACP_CERT_FILE=/fs/shim/carplug/_tools/acp-cert.der \
#       /fs/shim/carplug/_tools/nbt_carplay --stop-after iap2 --fifo none --no-audio --no-mic" 240
set -u
TAG=${1:?tag}
CMD=${2:?command}
MAX=${3:-180}
HERE=$(cd "$(dirname "$0")" && pwd)
CFG=${IDEMU_SSH_CONFIG:-$HERE/../../emulators/IDEmu/environments/f30-business-24-03/ssh/ssh_config}
LOG=/dev/shmem/run-$TAG.log
g() { ssh -F "$CFG" idemu "$1" 2>&1; }
g "rm -f $LOG; ($CMD; echo RUN_RC=\$?) > $LOG 2>&1 &" >/dev/null
t=0
while [ $t -lt "$MAX" ]; do
    sleep 3; t=$((t + 3))
    if g "grep -c RUN_RC= $LOG" | grep -q '^1$'; then break; fi
done
g "cat $LOG" | grep -v '__IDEMU'
[ $t -lt "$MAX" ] || echo "guest-run: still running after $MAX s (log: $LOG in the guest)"
