#!/bin/sh
# wired-carplay/car/autostart/cpd.sh -> /var/opt/nbt-cp/cpd.sh on the head unit (ksh: no awk, head, touch, wc there).
# Wired CarPlay as a service. nbtcp_iap2_adapter.so starts it once per boot, inside Connectivity's startup, when
# /var/opt/nbt-cp/autostart exists. It runs nbt_carplay --auto again and again: one run waits for an iPhone and for the
# head unit to look for it with its Wi-Fi off, bridges one session, and ends when the iPhone is unplugged or the head
# unit ends the session (wired-carplay/nbt-carplay-evo/src/cp_auto.c).
# Stops for this boot after 5 crashes of the bridge in a row; runs shorter than 10 s ten times in a row slow it down
# to one a minute. Removing /var/opt/nbt-cp/autostart, or /var/opt/nbt-cp/disable, stops it before the next run.
# Log (the bridge's too): /dev/shmem/nbt-carplay.log, RAM, moved to .1 when over 1 MB.
D=/var/opt/nbt-cp
LOG=/dev/shmem/nbt-carplay.log
PATH=/proc/boot:/bin:/usr/bin:/sbin:/usr/sbin:$PATH
export PATH
echo $$ > /dev/shmem/nbt-cpd.pid
log() { echo "cpd: $*" >> $LOG; }
log "started (pid $$)"
crashes=0
quick=0
while [ -f $D/autostart ] && [ ! -f $D/disable ]; do
    if [ -f $LOG ]; then
        set -- $(ls -l $LOG)
        if [ "${5:-0}" -gt 1048576 ]; then
            mv -f $LOG $LOG.1
        fi
    fi
    start=$SECONDS
    $D/nbt_carplay --auto >> $LOG 2>&1 < /dev/null
    rc=$?
    dur=$((SECONDS - start))
    log "nbt_carplay exit $rc after $dur s"
    if [ $rc -ge 128 ]; then
        crashes=$((crashes + 1))
    else
        crashes=0
    fi
    if [ $crashes -ge 5 ]; then
        log "5 crashes in a row: stopped until the next boot"
        break
    fi
    if [ $dur -lt 10 ]; then
        quick=$((quick + 1))
    else
        quick=0
    fi
    if [ $quick -ge 10 ]; then
        log "10 short runs in a row: one a minute from now"
        sleep 60
    else
        sleep 3
    fi
done
log "ended"
rm -f /dev/shmem/nbt-cpd.pid
