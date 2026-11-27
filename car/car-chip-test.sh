#!/bin/bash
# One car session for the question the emulator cannot answer: does the iPhone take the NBT Evo's Apple chip for
# wired CarPlay? Runs probe-evo on the head unit. Nothing is written to the head unit's flash: the probe
# and every file it makes live in /dev/shmem (RAM, gone at the next restart); NBT_CP_FLAT=1 keeps the probe there.
#
# Before: ENET cable in the OBD port, ignition on, head unit booted. Open the SSH connection yourself (it asks for
# the password once; ~/.ssh/config keeps it open for the scripts, ControlPersist 300 s):
#     ssh root@169.254.199.119 true
#
#   ./car-chip-test.sh check      read-only: processes, chip node and lock, RAM folders, USB, October leftovers; pulls
#                                 medialaunch, Connectivity, the ACP driver and the USB scripts and compares them with
#                                 the 007_001_016 firmware on the Mac
#   ./car-chip-test.sh push       copies the probe to /dev/shmem and checks it by SHA-256
#   ./car-chip-test.sh acp-read   the chip, no signature: way of access, version, certificate.
#                                 The iPhone unplugged and its Bluetooth off (so wireless CarPlay does not use the chip).
#   ./car-chip-test.sh acp-sign   the chip: one test signature
#   ./car-chip-test.sh iphone     the iPhone plugged in and unlocked: usb carplay, lockdown (twice), carkit
#                                 (answer "Trust" on the iPhone); then the head unit's log
#   ./car-chip-test.sh iphone-free  the same with the iPhone's iPod handlers ended before each step (see do_free)
#   ./car-chip-test.sh session    phase 1 of the nbt_carplay port: a CarPlay session up to the picture, dumped to a file
#   ./car-chip-test.sh bridge     factory-like test: iPhone's USB network as tap0 for the factory CarPlay (BT on, Wi-Fi off)
#   ./car-chip-test.sh bridge-start  the same, plus a firewall pass rule for tap0 (RAM only: anchor filter_USB, emptied
#                                 again at the end) and CarPlayStartSession to the factory AirPlay receiver
#   ./car-chip-test.sh bridge-ctrl   bridge-start, plus a CarPlay controller announced on tap0 in the iPhone's name
#                                 (its Bluetooth address from the head unit's log, or PHONE_BT=XX:XX:XX:XX:XX:XX)
#   ./car-chip-test.sh nbt-aa-pull   read only: the nbt-aa CarPlay hook (/var/opt/nbt-aa), its logs, the HMI binary
#   ./car-chip-test.sh pf-check      read only: the firewall rules, and whether HIDE_WIFI can be used on this unit
#                                 HIDE_WIFI=mdns|all before bridge-start/bridge-ctrl: hide the iPhone's wireless CarPlay
#                                 (mdns: its Bonjour on uap0; all: all of uap0) while the cable runs; RAM only, undone after
#                                 WAIT_HU=1 before bridge-start/bridge-ctrl: start the bridge only once the head unit looks
#                                 for the iPhone ("not found in available device list" again), up to WAIT_HU_MAX=150 s
#   ./car-chip-test.sh trace         read only: the head unit's live log for TRACE_SECONDS (default 150) into trace-<time>.txt
#                                 (start it, then switch Wi-Fi on / press buttons: the ring buffer loses a session's start)
#   ./car-chip-test.sh pcm-check     read only: has the live carplay.ini the uncompressed PCM audio lines, is a backup there
#   ./car-chip-test.sh pcm-apply     FLASH: backup of /var/opt/conn/carplay/carplay.ini, then the PCM lines added
#                                 (make_pcm_ini.py); takes effect after the head unit restarts (ignition off/on)
#   ./car-chip-test.sh pcm-revert    FLASH: the backup back in place (then restart the head unit)
#   ./car-chip-test.sh iap2-check    read only: carplay.ini [iAP2] LibPath, the adapter's files and log
#   ./car-chip-test.sh iap2-install  FLASH: iap2-adapter in front of the current [iAP2] LibPath (it chains to
#                                 it); takes effect after a head unit restart. IAP2_FACTORY=1 before bridge-start then
#                                 runs the bridge with --iap2-factory (with conn-apply w1 first: RAM)
#   ./car-chip-test.sh iap2-uninstall  FLASH: the LibPath line back as it was
#   ./car-chip-test.sh auto-check    read only: wired CarPlay without the laptop (autostart/README.md): files, service, log
#   ./car-chip-test.sh auto-install  FLASH: nbt_carplay, conn_patch, cpd.sh (and the adapter build) to /var/opt/nbt-cp
#   ./car-chip-test.sh auto-try      one nbt_carplay --auto by hand (RAM log), as the service would run it
#   ./car-chip-test.sh auto-on|auto-off  FLASH: the autostart flag (off also stops the service and puts w1 back)
#   ./car-chip-test.sh auto-uninstall    FLASH: auto-off and the files (the pair record stays unless PURGE=1)
#   ./car-chip-test.sh conn-pull     read only: copies of the running Connectivity and its libconn_carplay.so into
#                                 runs/<date>/files-conn/ (head-unit code: they stay on this Mac), which
#                                 conn-patch/build.sh needs to make its patch table
#   ./car-chip-test.sh conn-check    read only: are patches c1/c2/w1 in the running Connectivity (conn-patch)
#   ./car-chip-test.sh conn-apply c1|c2|w1 ...   patch the RUNNING Connectivity (w1: its libconn_carplay.so) in RAM
#                                 (flash untouched; a reboot undoes it)
#   ./car-chip-test.sh conn-revert c1|c2|w1 ...  put the factory bytes back without a reboot
#   ./car-chip-test.sh cleanup    removes the probe and its files from /dev/shmem
#   ./car-chip-test.sh all        check, push, acp-read, acp-sign, then asks before the iPhone part
#
# Logs: runs/<date-time>/ beside this script, one file per step plus summary.txt. They hold the iPhone's name and
# version and the chip's certificate: keep them private. DRY=1 prints the head-unit commands instead of running them.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
HU=${HU:-root@169.254.199.119}
PROBE=$HERE/../probe-evo/out/nbt_carplay_probe_evo.stripped
FW=$HERE/../../emulators/fw-extract/carplay/007_001_016/fs/sda0/repository/istep
P=/dev/shmem/nbt_carplay_probe_evo
PENV="NBT_CP_FLAT=1"

run() {   # one command on the head unit through the open SSH connection
    if [ -n "${DRY:-}" ]; then echo "[dry] hu: $*" >&2; return 0; fi
    ssh -T -o BatchMode=yes -o ConnectTimeout=10 "$HU" "$@"
}

new_run() { R=$HERE/runs/$(date +%Y%m%d-%H%M%S); mkdir -p "$R"; chmod 700 "$HERE/runs" "$R"; ln -sfn "$R" "$HERE/runs/current"; }
cur_run() { if [ -d "$HERE/runs/current/" ]; then R=$(cd "$HERE/runs/current/" && pwd -P); else new_run; fi; }
note() { echo "$*" | tee -a "$R/summary.txt"; }
step() { note ""; note "=== $1 ($(date +%H:%M:%S))"; }
keylines() { grep -E "$2" "$R/$1" | cut -c1-200 | tee -a "$R/summary.txt"; }

need_ssh() {
    [ -n "${DRY:-}" ] && return 0
    run true 2>/dev/null || { echo "No open SSH connection to $HU. Open it first: ssh $HU true" >&2; exit 1; }
}

do_check() {
    step "check (read-only)"
    run 'echo "== uname"; uname -a
echo "== processes"; pidin -F "%a %N" | grep -i -E "acpd|mm-ipod|medialaunch|io-usb|Connectivity|io-fs-media|aoap|nbt"
echo "== medialaunch arguments"; pidin -p vdev-medialaunch arg
echo "== chip node"; ls -l /dev/i2c/i2c_1/
echo "== chip lock"; ls -l /dev/shm_acp_lock
echo "== RAM folders"; ls -ld /tmp /fs/ram3 /fs/ram5 /dev/shmem
echo "== USB stacks"; ls /dev/io-usb /dev/otg
echo "== USB devices"; usb
echo "== October leftovers"; ls -l /fs/sda0/bin/start_usb.sh /fs/sda0/bin/start_vdev_medialaunch.sh; ls -l /var/opt/sys; ls -l /var/opt/sys/patched; cat /var/opt/sys/core.counter
echo "== which medialaunch"; whence vdev-medialaunch evo-medialaunch' > "$R/check.txt" 2>&1
    keylines check.txt '^== |acpd|mm-ipod|medialaunch|acp|shm_acp|ram|io-usb|CARPLAY|patched|counter|No such'
    step "files compared with 007_001_016"
    mkdir -p "$R/files"
    for f in bin/vdev-medialaunch bin/start_usb.sh bin/start_vdev_medialaunch.sh etc/umass-enum.cfg \
             opt/conn/bin/Connectivity opt/conn/bin/Connectivity.cfg opt/conn/data/carplay/carplay.ini \
             opt/conn/lib/carplay/libipod-acp-har-i2c-iap2.so opt/conn/bin/carplay/acpd; do
        local out=$R/files/$(echo "$f" | tr / _)
        run "cat /fs/sda0/repository/istep/$f" > "$out" < /dev/null 2>/dev/null
        if [ ! -s "$out" ]; then note "  MISSING   $f"; continue; fi
        local a b; a=$(shasum -a 256 "$out" | cut -c1-16); b=$(shasum -a 256 "$FW/$f" 2>/dev/null | cut -c1-16)
        note "  $([ "$a" = "$b" ] && echo "SAME     " || echo "DIFFERENT") $f ($(wc -c < "$out" | tr -d ' ') bytes, car $a, 007_001_016 ${b:-none})"
    done
    # the running scripts (repository copies above may differ from /fs/sda0/bin after the October steps)
    for f in start_usb.sh start_vdev_medialaunch.sh; do
        run "cat /fs/sda0/bin/$f" > "$R/files/running_$f" < /dev/null 2>/dev/null
        cmp -s "$R/files/running_$f" "$FW/bin/$f" && note "  SAME      /fs/sda0/bin/$f" || note "  DIFFERENT /fs/sda0/bin/$f (see files/running_$f)"
    done
}

do_push() {
    step "push the probe to $P"
    [ -f "$PROBE" ] || { note "  no $PROBE: build it with probe-evo/build.sh"; return 1; }
    local want got
    want=$(shasum -a 256 "$PROBE" | cut -d' ' -f1)
    run "cat > $P.part && mv $P.part $P && chmod 755 $P" < "$PROBE"
    got=$(run "cat $P" < /dev/null | shasum -a 256 | cut -d' ' -f1)
    [ -n "${DRY:-}" ] && got=$want
    if [ "$want" = "$got" ]; then note "  OK, SHA-256 ${want:0:16}"; else note "  MISMATCH: local ${want:0:16}, head unit ${got:0:16}"; return 1; fi
}

do_acp() {   # $1 = read | sign
    if [ "$1" = read ]; then
        step "acp-read (no signature; iPhone unplugged, its Bluetooth off)"
        run "$PENV NBT_ACP_NO_SIGN=1 $P acp" > "$R/acp-read.txt" 2>&1 < /dev/null
        keylines acp-read.txt 'acp:|way|version|certificate|OK|failed|stopping|in use|No such'
    else
        step "acp-sign (one test signature)"
        run "$PENV $P acp" > "$R/acp-sign.txt" 2>&1 < /dev/null
        keylines acp-sign.txt 'acp:|status|signature|error|OK|failed|in use'
        run "cat /dev/shmem/acp-cert.der" > "$R/acp-cert.der" 2>/dev/null < /dev/null
        if [ -s "$R/acp-cert.der" ]; then
            { openssl pkcs7 -inform DER -in "$R/acp-cert.der" -print_certs -noout 2>/dev/null \
              || openssl x509 -inform DER -in "$R/acp-cert.der" -noout -subject -issuer -dates 2>/dev/null; } \
              > "$R/acp-cert.txt"
            keylines acp-cert.txt 'subject|issuer|notBefore|notAfter'
        fi
    fi
    run "sloginfo" > "$R/sloginfo-after-acp-$1.txt" 2>&1 < /dev/null
    grep -i -E "ACP-I2C|acpd|sh_mutex" "$R/sloginfo-after-acp-$1.txt" | tail -8 | cut -c1-200 | sed 's/^/  slog: /' | tee -a "$R/summary.txt"
}

do_iphone() {
    step "iphone (plugged in, unlocked)"
    run "usb" > "$R/usb-before.txt" 2>&1 < /dev/null
    keylines usb-before.txt 'Vendor|Product|USB [0-9]'
    step "usb carplay"
    run "$PENV $P usb carplay" > "$R/usb-carplay.txt" 2>&1 < /dev/null
    keylines usb-carplay.txt 'configurations|CarPlay|re-connect|held|failed|attach'
    for n in 1 2; do
        step "lockdown $n"
        run "$PENV $P lockdown" > "$R/lockdown-$n.txt" 2>&1 < /dev/null
        keylines lockdown-$n.txt 'configurations|select|mux:|lockdown: (connected|done|stopped)|ProductVersion|ProductType|not reported|held|failed'
        grep -q "lockdown: done" "$R/lockdown-$n.txt" && break
    done
    step "carkit (answer \"Trust\" on the iPhone, with the passcode)"
    run "$PENV $P carkit" > "$R/carkit.txt" 2>&1 < /dev/null
    keylines carkit.txt 'pair:|tls:|carkit:|acp:|iap2:|signature|status|identif|authent|CarPlay|failed|error'
    run "sloginfo" > "$R/sloginfo-after-iphone.txt" 2>&1 < /dev/null
    grep -i -E "ACP-I2C|medialaunch|iap2|ipod|carplay|dipo" "$R/sloginfo-after-iphone.txt" | tail -15 | cut -c1-200 | sed 's/^/  slog: /' | tee -a "$R/summary.txt"
}

# The factory iPod stack holds every interface of an iPhone plugged in (car 2026-11-26: "Resource busy" on all three):
# vdev-medialaunch starts, for that device only, "io-fs-media -dipod,..." and "io-audio ... -dipod busno=..,devno=..".
# Ends exactly those (SIGTERM, by the -dipod argument; never vdev-medialaunch, never the system's io-fs-media/io-audio).
# Replugging the iPhone brings them back.
FREE='for n in io-fs-media io-audio; do pidin -p $n arg | grep -- "-dipod" | while read pid rest; do kill $pid && echo "ended $n $pid"; done; done'

do_free() { run "$FREE" < /dev/null 2>&1 | tee -a "$R/freed.txt" | sed 's/^/  /' | tee -a "$R/summary.txt"; sleep 1; }

do_iphone_free() {   # the iPhone steps with the iPod handlers ended before each one
    step "iphone-free (iPod handlers ended before each step)"
    do_free
    step "usb carplay"
    run "$PENV $P usb carplay" > "$R/usb-carplay.txt" 2>&1 < /dev/null
    keylines usb-carplay.txt 'configurations|CarPlay|re-connect|held|failed|attach|busy'
    for n in 1 2 3; do
        do_free
        step "lockdown $n"
        run "$PENV $P lockdown" > "$R/lockdown-$n.txt" 2>&1 < /dev/null
        keylines lockdown-$n.txt 'configurations|select|mux:|lockdown: (connected|done|stopped)|ProductVersion|ProductType|not reported|held|failed|busy'
        grep -q "lockdown: done" "$R/lockdown-$n.txt" && break
    done
    do_free
    step "carkit (answer \"Trust\" on the iPhone, with the passcode)"
    run "$PENV $P carkit" > "$R/carkit.txt" 2>&1 < /dev/null
    keylines carkit.txt 'pair:|tls:|carkit:|acp:|iap2:|signature|status|identif|authent|CarPlay|failed|error|busy|held'
    run "sloginfo" > "$R/sloginfo-after-iphone-free.txt" 2>&1 < /dev/null
    grep -a -i -E "ACP-I2C|medialaunch|iap2|ipod|carplay|dipo" "$R/sloginfo-after-iphone-free.txt" | tail -15 | cut -c1-200 | sed 's/^/  slog: /' | tee -a "$R/summary.txt"
}

# Phase 1 of the nbt_carplay port (nbt-carplay-evo): a whole CarPlay session up to the picture, written
# to a file instead of the screen, no sound, no microphone. nbt_carplay ends the iPhone's iPod handlers itself.
# Everything stays in RAM: the program in /dev/shmem, its state (pairing, AirPlay identity) and dumps under /tmp
# (/fs/ram5). Afterwards the H.264 dump is pulled and one frame of it is saved as a PNG.
CP_BIN=$HERE/../nbt-carplay-evo/out/nbt_carplay.stripped
CP=/dev/shmem/nbt_carplay
CPLOG=/dev/shmem/cp-bridge.log

do_session() {
    step "session: push nbt_carplay to $CP"
    [ -f "$CP_BIN" ] || { note "  no $CP_BIN: build it with nbt-carplay-evo/build.sh"; return 1; }
    local want got
    want=$(shasum -a 256 "$CP_BIN" | cut -d' ' -f1)
    run "rm -f $CP $CP.part; cat > $CP && chmod 755 $CP" < "$CP_BIN"   # /dev/shmem: no rename over a file; SHA checked below
    got=$(run "cat $CP" < /dev/null | shasum -a 256 | cut -d' ' -f1)
    [ -n "${DRY:-}" ] && got=$want
    [ "$want" = "$got" ] && note "  OK, SHA-256 ${want:0:16}" || { note "  MISMATCH: local ${want:0:16}, head unit ${got:0:16}"; return 1; }
    step "session: nbt_carplay up to the picture (answer \"Trust\" on the iPhone)"
    run "rm -f /tmp/cp-session.h264 /tmp/cp-session.pcap; $CP --stop-after video --seconds 20 --fifo none --no-audio --no-mic --dump /tmp/cp-session.h264 --pcap /tmp/cp-session.pcap" \
        > "$R/session.txt" 2>&1 < /dev/null
    keylines session.txt 'nbt_carplay:|usb:|pair:|tls:|carkit|iap2:|acp:|net:|ncm|airplay:|screen:|video|frames|key frame|error|failed|stopped|done'
    run "cat /tmp/cp-session.h264" > "$R/cp-session.h264" 2>/dev/null < /dev/null
    run "cat /tmp/cp-session.pcap" > "$R/cp-session.pcap" 2>/dev/null < /dev/null
    note "  H.264 dump: $(wc -c < "$R/cp-session.h264" | tr -d ' ') bytes, pcap: $(wc -c < "$R/cp-session.pcap" | tr -d ' ') bytes"
    if [ -s "$R/cp-session.h264" ] && command -v ffmpeg >/dev/null; then
        ffmpeg -loglevel error -y -i "$R/cp-session.h264" -vf "select=gte(n\,30)" -frames:v 1 "$R/cp-session-frame.png" \
            && note "  frame: $R/cp-session-frame.png"
    fi
    run "sloginfo" > "$R/sloginfo-after-session.txt" 2>&1 < /dev/null
    grep -a -i -E "medialaunch|ipod|ACP-I2C" "$R/sloginfo-after-session.txt" | tail -8 | cut -c1-200 | sed 's/^/  slog: /' | tee -a "$R/summary.txt"
}

# Step 4 of CHECKLIST-factory-entry.md (the iPhone's Wi-Fi on): while the cable runs, hide the iPhone's wireless CarPlay
# from the head unit. HIDE_WIFI=mdns blocks Bonjour on uap0 (the head unit then sees the iPhone's controller only on the
# cable); HIDE_WIFI=all blocks all of uap0 (that also ends a wireless session already running). The uap0 pass rules
# are "quick" and come before the anchor filter_USB, so a block there would never be reached; the only anchor evaluated
# before them is filter_OBD. Fail closed: the rule goes there only if filter_OBD is empty on this head unit and comes
# before the first uap0 rule. The marker /dev/shmem/uap0.pf says the anchor holds our rule; only then is it emptied again.
UAP0_PF=/dev/shmem/uap0.pf
hide_wifi_rule() {
    case "${HIDE_WIFI:-}" in
        mdns) echo 'block quick on uap0 proto udp from any to any port 5353' ;;
        all) echo 'block quick on uap0 all' ;;
    esac
}
pf_hide_possible() {   # 0: filter_OBD is empty and evaluated before the first uap0 rule
    local rules obd a u
    [ -n "${DRY:-}" ] && return 0
    rules=$(run "pfctl -s rules" < /dev/null 2>/dev/null)
    obd=$(run "pfctl -a filter_OBD -s rules" < /dev/null 2>/dev/null | grep -c .)
    a=$(echo "$rules" | grep -n -E 'anchor "?filter_OBD"?' | sed -n 1p | cut -d: -f1)
    u=$(echo "$rules" | grep -n ' on uap0 ' | sed -n 1p | cut -d: -f1)
    [ "$obd" = 0 ] && [ -n "$a" ] && [ -n "$u" ] && [ "$a" -lt "$u" ]
}
hide_wifi_on() {
    local rule
    [ -z "${HIDE_WIFI:-}" ] && return 0
    rule=$(hide_wifi_rule)
    [ -n "$rule" ] || { note "  HIDE_WIFI must be mdns or all"; return 1; }
    pf_hide_possible || { note "  HIDE_WIFI: filter_OBD is not empty here or not before the uap0 rules; not touching it (see pf-check)"; return 1; }
    step "hide wireless CarPlay (HIDE_WIFI=$HIDE_WIFI): a block rule for uap0 in anchor filter_OBD (RAM only)"
    run "echo '$rule' > $UAP0_PF && pfctl -a filter_OBD -f $UAP0_PF; pfctl -a filter_OBD -s rules" < /dev/null 2>&1 \
        | sed 's/^/  /' | tee -a "$R/summary.txt"
}
hide_wifi_off() {   # only if the marker says the rule is ours
    run "if [ -f $UAP0_PF ]; then pfctl -a filter_OBD -F rules 2>/dev/null; rm -f $UAP0_PF; fi; pfctl -a filter_OBD -s rules | grep -c ." \
        < /dev/null 2>&1 | sed 's/^/  rules left in filter_OBD: /' | tee -a "$R/summary.txt"
}
redact_bt() { sed -E 's/[0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5}/<BT>/g'; }
hu_carplay_lines() {   # the CarPlay plug-in's last lines; "not found in available device list" again and again = the head
    # unit is trying to connect the phone it knows by Bluetooth (Connectivity's own state is not in this log)
    run "sloginfo" < /dev/null 2>/dev/null | grep -a 'CarPlay\]\[DevCtrl\]' | tail -"${2:-4}" | redact_bt | cut -c1-200 \
        | sed "s/^/  $1: /" | tee -a "$R/summary.txt"
}

# WAIT_HU=1: the session must reach the head unit while Connectivity is connecting the phone (it is then "active" and
# gets the iDrive entry); one that comes earlier stays inactive, as on 26.11. So before the bridge starts, wait until the
# plug-in logs "not found in available device list" (or "Connection failed with the device", when a stale Wi-Fi
# controller of the iPhone is still in its list) at least twice more than before: the head unit retries every second
# while it looks for the phone. At most WAIT_HU_MAX seconds; then it starts anyway and says so.
wait_hu() {
    local last cur changes=0 t=0 max=${WAIT_HU_MAX:-150}
    local pat='not found in available device list|Connection failed with the device'
    [ -n "${WAIT_HU:-}" ] || return 0
    [ -n "${DRY:-}" ] && { note "  [dry] would wait up to $max s for the head unit to look for the iPhone"; return 0; }
    # the log is a ring buffer, so a count of these lines can stand still while new ones come in: watch the newest
    # one instead (each carries its own timestamp to the microsecond)
    last=$(run "sloginfo" < /dev/null 2>/dev/null | grep -a -E "$pat" | tail -1)
    note "  waiting up to $max s until the head unit looks for the iPhone (switch the iPhone's Bluetooth on now if it is off)"
    while [ $t -lt "$max" ]; do
        sleep 3
        t=$((t + 3))
        cur=$(run "sloginfo" < /dev/null 2>/dev/null | grep -a -E "$pat" | tail -1)
        if [ -n "$cur" ] && [ "$cur" != "$last" ]; then
            changes=$((changes + 1))
            last=$cur
            if [ $changes -ge 2 ]; then
                note "  the head unit is looking for the iPhone (after $t s)"
                return 0
            fi
        fi
    done
    note "  the head unit did not look for the iPhone within $max s: starting the bridge anyway"
}

do_pf_check() {
    step "pf-check (read only)"
    run "pfctl -s rules; echo '== anchors'; pfctl -s Anchors; echo '== filter_OBD'; pfctl -a filter_OBD -s rules; echo '== filter_USB'; pfctl -a filter_USB -s rules; echo '== interfaces'; ifconfig -a" \
        < /dev/null > "$R/pf-check.txt" 2>&1
    note "  $(grep -c . "$R/pf-check.txt" | tr -d ' ') lines in pf-check.txt; uap0 rules: $(grep -c ' on uap0 ' "$R/pf-check.txt" | tr -d ' ')"
    if pf_hide_possible; then
        note "  HIDE_WIFI can be used: filter_OBD is empty and comes before the uap0 rules"
    else
        note "  HIDE_WIFI cannot be used here (filter_OBD not empty, or not before the uap0 rules): see pf-check.txt"
    fi
}

# The factory-like route (docs/research/factory-like-design-2026-11-26.md): nbt_carplay --bridge does USB, the CarPlay
# request and the iAP2 authentication with the chip, then makes the iPhone's NCM link the head unit's interface tap0
# (RAM only; removed when it ends) and sends no CarPlayStartSession: the factory Connectivity is to find the iPhone
# there as over Wi-Fi. The iPhone: Bluetooth ON (paired with the car, wireless CarPlay on), Wi-Fi OFF.
do_bridge() {
    local mode=${1:-} ctrl=0
    [ "$mode" = ctrl ] && { mode=start; ctrl=1; }
    step "bridge${mode:+-$mode}: push nbt_carplay to $CP"
    [ -f "$CP_BIN" ] || { note "  no $CP_BIN"; return 1; }
    local want got
    want=$(shasum -a 256 "$CP_BIN" | cut -d' ' -f1)
    run "rm -f $CP $CP.part; cat > $CP && chmod 755 $CP" < "$CP_BIN"   # /dev/shmem: no rename over a file; SHA checked below
    got=$(run "cat $CP" < /dev/null | shasum -a 256 | cut -d' ' -f1)
    [ -n "${DRY:-}" ] && got=$want
    [ "$want" = "$got" ] && note "  OK, SHA-256 ${want:0:16}" || { note "  MISMATCH"; return 1; }
    run "sloginfo > /tmp/slog-before-bridge.txt; ifconfig -a | grep -E '^[a-z]' > /tmp/if-before-bridge.txt" < /dev/null
    hu_carplay_lines "before" 4
    [ -n "${HIDE_WIFI:-}" ] && [ "$mode" != start ] && note "  HIDE_WIFI is only for bridge-start and bridge-ctrl: ignored"
    if [ "$mode" = start ]; then
        # pf: the main ruleset ends in "block all" and passes only named interfaces; filter_USB is an empty anchor it
        # evaluates. One pass rule for tap0 there, in RAM; "pfctl -a filter_USB -F rules" (below, or a reboot) undoes it
        step "bridge-start: firewall pass rule for tap0 (anchor filter_USB, RAM only)"
        run "echo 'pass quick on tap0 all' > /dev/shmem/tap0.pf && pfctl -a filter_USB -f /dev/shmem/tap0.pf; pfctl -a filter_USB -s rules" \
            < /dev/null 2>&1 | sed 's/^/  /' | tee -a "$R/summary.txt"
        if ! hide_wifi_on; then
            run "pfctl -a filter_USB -F rules; rm -f /dev/shmem/tap0.pf" < /dev/null
            return 1
        fi
        step "bridge-start: 180 s after the authentication (watch the head unit's screen; \"Trust\" on the iPhone if asked)"
        # the whole head unit log while it runs (the ring buffer loses a session's start within minutes)
        run "sloginfo -w" > "$R/slog-live-bridge.txt" 2>&1 < /dev/null &
        local slogpid=$!
        wait_hu
        # START_V6=1: CarPlayStartSession names tap0's IPv6 link-local instead of the receiver's IPv4 172.16.226.1
        local mode_opt="--bridge-start${IAP2_FACTORY:+ --iap2-factory}"
        if [ "$ctrl" = 1 ]; then
            local bt=${PHONE_BT:-}
            [ -n "$bt" ] || bt=$(run "sloginfo" < /dev/null 2>/dev/null | grep -a -o -E "BT Addr(ess=|: )[0-9A-F:]{17}" | tail -1 | grep -o -E "[0-9A-F:]{17}")
            [ -n "${DRY:-}" ] && bt=${bt:-00:00:00:00:00:00}
            [ -n "$bt" ] || { note "  no Bluetooth address of the iPhone in the head unit's log (connect it by BT, or PHONE_BT=)"; run "pfctl -a filter_USB -F rules; rm -f /dev/shmem/tap0.pf" < /dev/null; hide_wifi_off; pkill -P $slogpid 2>/dev/null; kill $slogpid 2>/dev/null; return 1; }
            note "  the iPhone's Bluetooth address: found (kept out of this summary)"
            mode_opt="--bridge-ctrl $bt${IAP2_FACTORY:+ --iap2-factory}"
        fi
        # the bridge's own log goes to a file on the head unit, pulled afterwards: on 27.11 (IAP2_FACTORY run) its output
        # over the SSH channel stopped after 5 KB and the bridge stalled with it (CarPlay lost after 10 s)
        run "rm -f $CPLOG; ${START_V6:+NBT_CP_START_V6=1 }$CP $mode_opt --stop-after iap2 --seconds ${SECONDS_AFTER:-180}${BRIDGE_PCAP:+ --pcap /tmp/cp-bridge.pcap} > $CPLOG 2>&1; echo \"nbt_carplay exit \$?\" >> $CPLOG" > "$R/bridge-ssh.txt" 2>&1 < /dev/null
        run "cat $CPLOG; rm -f $CPLOG" > "$R/bridge.txt" 2>&1 < /dev/null
        pkill -P $slogpid 2>/dev/null; kill $slogpid 2>/dev/null   # the ssh under the background run (it outlived a plain
        # kill on 27.11 and kept three remote "sloginfo -w" running); the remote one ends when its channel closes
        note "  live log: $(wc -l < "$R/slog-live-bridge.txt" | tr -d ' ') lines"
        step "bridge-start: firewall back as it was"
        run "ifconfig tap0 destroy 2>/dev/null; pfctl -a filter_USB -F rules; rm -f /dev/shmem/tap0.pf; pfctl -a filter_USB -s rules | grep -c . " \
            < /dev/null 2>&1 | sed 's/^/  rules left in filter_USB: /' | tee -a "$R/summary.txt"
        hide_wifi_off
    else
        step "bridge: 120 s after the authentication (watch the head unit's screen; \"Trust\" on the iPhone if asked)"
        run "rm -f $CPLOG; $CP --bridge --stop-after iap2 --seconds 120${BRIDGE_PCAP:+ --pcap /tmp/cp-bridge.pcap} > $CPLOG 2>&1; echo \"nbt_carplay exit \$?\" >> $CPLOG" > "$R/bridge-ssh.txt" 2>&1 < /dev/null
        run "cat $CPLOG; rm -f $CPLOG" > "$R/bridge.txt" 2>&1 < /dev/null
    fi
    keylines bridge.txt 'nbt_carplay:|usb: (CarPlay|select|ending)|bridge:|address|inet6|iap2: (identification|authentication|CarPlay)|pair:|tls:|carkit|error|failed'
    # the head unit's /tmp is a 14 MB RAM disk: a whole session's pcap (PCM audio, video) filled it on 27.11 and Siri's
    # microphone buffer then failed (ENOSPC, session lost). So the pcap is opt-in (BRIDGE_PCAP=1, short runs only)
    if [ -n "${BRIDGE_PCAP:-}" ]; then
        run "cat /tmp/cp-bridge.pcap; rm -f /tmp/cp-bridge.pcap" > "$R/cp-bridge.pcap" 2>/dev/null < /dev/null
        note "  pcap: $(wc -c < "$R/cp-bridge.pcap" | tr -d ' ') bytes"
    fi
    run "sloginfo" > "$R/sloginfo-after-bridge.txt" 2>&1 < /dev/null
    grep -a -i -E "carplay|dipo|airplay|bonjour|controller|mdns|tap0|SessionStatus|ConnectCPDevice|CCarPlayImpl" "$R/sloginfo-after-bridge.txt" \
        | tail -40 | redact_bt | cut -c1-200 | sed 's/^/  slog: /' | tee -a "$R/summary.txt"
    run "ifconfig -a | grep -c '^tap'" < /dev/null | sed 's/^/  tap interfaces left: /' | tee -a "$R/summary.txt"
    if [ -n "${IAP2_FACTORY:-}" ]; then
        run "cat /dev/shmem/nbtcp-iap2.log" < /dev/null > "$R/nbtcp-iap2.log" 2>&1
        run "ls -l /pps/services/multimedia/iap2_wifi; for f in device nowplaying communications; do echo \"== \$f\"; cat /pps/services/multimedia/iap2_wifi/\$f; done" \
            < /dev/null > "$R/pps-iap2-wifi-wired.txt" 2>&1
        grep -a -E "iap2dev:" "$R/bridge.txt" | cut -c1-200 | sed 's/^/  /' | tee -a "$R/summary.txt"
        tail -5 "$R/nbtcp-iap2.log" | sed 's/^/  adapter: /' | tee -a "$R/summary.txt"
        grep -a -E "Title|Artist|Authenticated" "$R/pps-iap2-wifi-wired.txt" | sed 's/^/  pps: /' | tee -a "$R/summary.txt"
        # nbt-aa's adapter sits behind ours and, armed, puts its own route-guidance packets into the iAP2 stream
        grep -a "nbt-cpadapter" "$R/sloginfo-after-bridge.txt" | tail -8 | cut -c1-200 | sed 's/^/  nbt-aa: /' | tee -a "$R/summary.txt"
    fi
}

# Read only: what the third-party nbt-aa installs into the factory CarPlay (Connectivity loads
# /var/opt/nbt-aa/libnbt-aa-cp.so and hands it CDipo; docs/research/factory-like-device-list-re-2026-11-26.md), its
# logs, and the car's HMI binary (to match the HMI side of the "Apple CarPlay" entry to the car's build).
do_nbt_aa_pull() {
    step "nbt-aa-pull (read only)"
    local d="$R/files-nbt-aa" f n
    mkdir -p "$d"
    run "ls -lR /var/opt/nbt-aa; ls -l /dev/shmem | grep nbt-aa; pidin -p HmiMain3D arg | sed -n 2p" < /dev/null > "$d/listing.txt" 2>&1
    for f in $(run "ls /var/opt/nbt-aa/*.so /var/opt/nbt-aa/*.cfg /var/opt/nbt-aa/*.sh /var/opt/nbt-aa/*.txt /dev/shmem/nbt-aa-cp.log /dev/shmem/nbt-aa-label.status /dev/shmem/nbt-cpadapter.status 2>/dev/null" < /dev/null); do
        n=$(echo "${f#/}" | tr / _)
        run "cat $f" < /dev/null > "$d/$n" 2>/dev/null
    done
    f=$(run "pidin -p HmiMain3D arg | sed -n 2p" < /dev/null | awk '{print $2}')
    [ -n "$f" ] && run "cat $f" < /dev/null > "$d/HmiMain3D" 2>/dev/null
    ls -l "$d" | sed 's/^/  /' | tee -a "$R/summary.txt"
    [ -f "$d/HmiMain3D" ] && note "  HmiMain3D SHA-256 $(shasum -a 256 "$d/HmiMain3D" | cut -c1-16)"
}

do_trace() {
    local secs=${TRACE_SECONDS:-150} f pid
    f="$R/trace-$(date +%H%M%S).txt"
    step "trace: the head unit's live log for $secs s (read only) -> $(basename "$f")"
    run "sloginfo -w" > "$f" 2>&1 < /dev/null &
    pid=$!
    note "  recording; do the thing now (Wi-Fi on, play, buttons, a call)"
    sleep "$secs"
    pkill -P $pid 2>/dev/null; kill $pid 2>/dev/null   # the ssh under the background run, then the run itself
    wait $pid 2>/dev/null
    note "  $(wc -l < "$f" | tr -d ' ') lines; iAP2 messages: $(grep -a -c -E '\[[0-9A-F]{4}\] [A-Za-z]' "$f" | tr -d ' ')"
}

# Uncompressed audio over the cable (docs/research/audio-pcm-re-2026-11-27.md): the receiver offers the iPhone only the
# formats in [Audio] of the live carplay.ini (/var, made by Connectivity from /opt/conn/data/carplay/carplay.ini and
# kept until the car's coding changes; nbt-aa edited its [iAP2] once). The car's file has only AAC, and over the cable
# the iPhone opened no audio stream. pcm-apply keeps a backup beside it (carplay.ini.before-pcm, written once, never
# overwritten) and a copy here, adds the PCM lines from make_pcm_ini.py, and checks every write by SHA-256. Fail closed:
# no write if the file has PCM lines already or the backup exists (apply) / is missing (revert).
CPINI=/var/opt/conn/carplay/carplay.ini
CPBAK=/var/opt/conn/carplay/carplay.ini.before-pcm
hu_sha() { run "cat $1" < /dev/null 2>/dev/null | shasum -a 256 | cut -c1-64; }
do_pcm() {
    local what=$1 have bak want got
    step "pcm-$what"
    run "cat $CPINI" < /dev/null > "$R/carplay.ini.now" 2>/dev/null
    [ -s "$R/carplay.ini.now" ] || [ -n "${DRY:-}" ] || { note "  cannot read $CPINI"; return 1; }
    have=$(grep -c "Format:PCM" "$R/carplay.ini.now")
    bak=$(run "[ -f $CPBAK ] && echo yes || echo no" < /dev/null 2>/dev/null)
    note "  live carplay.ini: $(grep -c "^FormatSupport=" "$R/carplay.ini.now") FormatSupport lines, $have of them PCM; backup on the head unit: $bak"
    case "$what" in
    check) return 0 ;;
    apply)
        [ "$have" = 0 ] || { note "  it has PCM lines already: nothing written"; return 1; }
        [ "$bak" = no ] || [ -n "${DRY:-}" ] || { note "  $CPBAK exists already: nothing written (revert first, or look at it)"; return 1; }
        python3 -I "$HERE/make_pcm_ini.py" "$R/carplay.ini.now" "$R/carplay.ini.pcm" | sed 's/^/  /' | tee -a "$R/summary.txt"
        [ -s "$R/carplay.ini.pcm" ] || [ -n "${DRY:-}" ] || { note "  make_pcm_ini.py made nothing"; return 1; }
        cp "$R/carplay.ini.now" "$R/carplay.ini.before-pcm"
        run "cp $CPINI $CPBAK" < /dev/null
        [ "$(hu_sha $CPBAK)" = "$(shasum -a 256 < "$R/carplay.ini.now" | cut -c1-64)" ] || [ -n "${DRY:-}" ] || { note "  backup does not match: nothing else written"; return 1; }
        note "  backup: $CPBAK (and $R/carplay.ini.before-pcm)"
        want=$(shasum -a 256 < "$R/carplay.ini.pcm" | cut -c1-64)
        run "rm -f $CPINI.new; cat > $CPINI.new" < "$R/carplay.ini.pcm"
        got=$(hu_sha $CPINI.new)
        [ "$want" = "$got" ] || [ -n "${DRY:-}" ] || { note "  new file did not arrive whole: $CPINI untouched"; run "rm -f $CPINI.new" < /dev/null; return 1; }
        run "mv -f $CPINI.new $CPINI" < /dev/null
        got=$(hu_sha $CPINI)
        [ "$want" = "$got" ] || [ -n "${DRY:-}" ] || { note "  MISMATCH after the rename: run $0 pcm-revert"; return 1; }
        note "  written, SHA-256 ${want:0:16}. Wait 30 s, then ignition off, wait for the screen to go dark, ignition on."
        ;;
    revert)
        [ "$bak" = yes ] || [ -n "${DRY:-}" ] || { note "  no $CPBAK on the head unit: nothing written (the Mac copy is in an earlier run folder)"; return 1; }
        if grep -a -q -E '^LibPath=.*nbtcp_iap2_adapter\.so' "$R/carplay.ini.now" && [ -z "${FORCE:-}" ]; then
            note "  the iAP2 adapter is in LibPath: run $0 iap2-uninstall first (FORCE=1 puts the whole pre-PCM file back, LibPath included)"
            return 1
        fi
        want=$(hu_sha $CPBAK)
        run "cp $CPBAK $CPINI" < /dev/null
        [ "$(hu_sha $CPINI)" = "$want" ] || [ -n "${DRY:-}" ] || { note "  MISMATCH after the copy: look at it by hand"; return 1; }
        note "  carplay.ini is the backup again (SHA-256 ${want:0:16}); restart the head unit. The backup file stays."
        ;;
    esac
}

# The iAP2 adapter (iap2-adapter, docs/research/iap2-factory-re-2026-11-27.md): the CarPlay plug-in loads the library in
# carplay.ini [iAP2] LibPath; ours stands in front of the one there now (the factory's, or nbt-aa's) and chains every
# call to it, so wireless CarPlay and nbt-aa keep working. Files: /var/opt/nbt-cp/nbtcp_iap2_adapter.so and
# /var/opt/nbt-cp/iap2-next (the LibPath it replaced). Only the LibPath line changes (make_iap2_ini.py); a whole-file
# backup carplay.ini.before-iap2 is kept once. Fail closed: no write if LibPath is ours already, the library it names
# is missing, or any copy does not match by SHA-256.
ADIR=/var/opt/nbt-cp
ASO=$ADIR/nbtcp_iap2_adapter.so
ANEXT=$ADIR/iap2-next
ABAK=/var/opt/conn/carplay/carplay.ini.before-iap2
ABIN=$HERE/../iap2-adapter/out/nbtcp_iap2_adapter.so
hu_put() {   # local file -> head unit path, through .new and a rename, checked by SHA-256
    local want
    want=$(shasum -a 256 < "$1" | cut -c1-64)
    run "rm -f $2.new; cat > $2.new" < "$1"
    [ "$(hu_sha $2.new)" = "$want" ] || [ -n "${DRY:-}" ] || { note "  $2.new did not arrive whole"; run "rm -f $2.new" < /dev/null; return 1; }
    run "mv -f $2.new $2" < /dev/null
    [ "$(hu_sha $2)" = "$want" ] || [ -n "${DRY:-}" ] || { note "  $2 does not match after the rename"; return 1; }
}
do_iap2() {
    local what=$1 old
    step "iap2-$what"
    run "cat $CPINI" < /dev/null > "$R/carplay.ini.now" 2>/dev/null
    [ -s "$R/carplay.ini.now" ] || [ -n "${DRY:-}" ] || { note "  cannot read $CPINI"; return 1; }
    note "  [iAP2] $(grep -a -E '^LibPath=' "$R/carplay.ini.now" | sed -n 1p)"
    case "$what" in
    check)
        run "ls -l $ADIR $ABAK 2>&1; echo '== next'; cat $ANEXT 2>&1; echo '== log'; tail -20 /dev/shmem/nbtcp-iap2.log 2>&1" < /dev/null 2>&1 \
            | sed 's/^/  /' | tee -a "$R/summary.txt"
        ;;
    install)
        [ -f "$ABIN" ] || { note "  no $ABIN: build it with iap2-adapter/build.sh"; return 1; }
        old=$(python3 -I "$HERE/make_iap2_ini.py" "$R/carplay.ini.now" "$R/carplay.ini.iap2" "$ASO" 2>&1) || { note "  $old: nothing written"; return 1; }
        [ -n "${DRY:-}" ] || run "[ -f $old ]" < /dev/null || { note "  the library it names ($old) is not there: nothing written"; return 1; }
        note "  chaining to $old"
        run "mkdir -p $ADIR" < /dev/null
        hu_put "$ABIN" "$ASO" || return 1
        printf '%s\n' "$old" > "$R/iap2-next"
        hu_put "$R/iap2-next" "$ANEXT" || return 1
        if [ "$(run "[ -f $ABAK ] && echo yes" < /dev/null 2>/dev/null)" != yes ]; then
            run "cp $CPINI $ABAK" < /dev/null
            [ "$(hu_sha $ABAK)" = "$(shasum -a 256 < "$R/carplay.ini.now" | cut -c1-64)" ] || [ -n "${DRY:-}" ] || { note "  backup does not match: carplay.ini untouched"; return 1; }
        fi
        cp "$R/carplay.ini.now" "$R/carplay.ini.before-iap2"
        hu_put "$R/carplay.ini.iap2" "$CPINI" || { note "  carplay.ini: run $0 iap2-uninstall or restore $ABAK"; return 1; }
        note "  installed. Restart the head unit (hold the volume knob ~30 s) for Connectivity to load it."
        ;;
    uninstall)
        old=$(run "cat $ANEXT" < /dev/null 2>/dev/null | sed -n 1p)
        [ -n "$old" ] || [ -n "${DRY:-}" ] || { note "  no $ANEXT: put LibPath back from $ABAK by hand"; return 1; }
        python3 -I "$HERE/make_iap2_ini.py" --restore "$R/carplay.ini.now" "$R/carplay.ini.noiap2" "$old" > /dev/null || { note "  LibPath is not ours: nothing written"; return 1; }
        hu_put "$R/carplay.ini.noiap2" "$CPINI" || return 1
        note "  LibPath is $old again. Restart the head unit. The adapter files in $ADIR stay (unused)."
        ;;
    esac
}

# Wired CarPlay without the laptop (car/autostart/README.md): nbt_carplay --auto as a service. Files in
# /var/opt/nbt-cp: nbt_carplay, conn_patch (w1 before each wired session), cpd.sh (the service loop), state/ (the pair
# record: "Trust" once per iPhone), autostart (the flag: nbtcp_iap2_adapter.so starts cpd.sh at Connectivity's start).
# auto-install copies the files (and the current adapter build; it takes effect at the next start of the head unit)
# but sets no flag; auto-try runs one --auto by hand; auto-on / auto-off set / remove the flag (auto-off also stops a
# running service and puts w1 back); auto-uninstall also removes the files (the pair record stays unless PURGE=1).
AUTO_BIN=$HERE/../nbt-carplay-evo/out/nbt_carplay.stripped
AUTO_SH=$HERE/autostart/cpd.sh
AUTO_LOG=/dev/shmem/nbt-carplay.log
do_auto() {
    local what=$1 f
    step "auto-$what"
    case "$what" in
    check)
        run "ls -l $ADIR $ADIR/state 2>&1 | sed 's/  */ /g'; echo '== service'; cat /dev/shmem/nbt-cpd.pid 2>&1; pidin arg | grep -E 'cpd[.]sh|nbt_carplay|sloginfo -w' | grep -v grep; echo '== log'; tail -25 $AUTO_LOG 2>&1; echo '== adapter'; tail -5 /dev/shmem/nbtcp-iap2.log 2>&1" < /dev/null 2>&1 \
            | redact_bt | sed 's/^/  /' | tee -a "$R/summary.txt"
        for f in nbt_carplay:$AUTO_BIN conn_patch:$CPATCH_BIN cpd.sh:$AUTO_SH nbtcp_iap2_adapter.so:$ABIN; do
            [ -f "${f#*:}" ] || continue
            if [ "$(hu_sha $ADIR/${f%%:*})" = "$(shasum -a 256 < "${f#*:}" | cut -c1-64)" ]; then
                note "  $ADIR/${f%%:*}: same as here"
            else
                note "  $ADIR/${f%%:*}: DIFFERENT from here (or missing)"
            fi
        done
        ;;
    install)
        grep -a -q -E '^LibPath=.*nbtcp_iap2_adapter\.so' "$(run "cat $CPINI" < /dev/null > "$R/carplay.ini.now" 2>/dev/null; echo "$R/carplay.ini.now")" \
            || [ -n "${DRY:-}" ] || { note "  the iAP2 adapter is not in LibPath: run $0 iap2-install first"; return 1; }
        for f in "$AUTO_BIN" "$CPATCH_BIN" "$AUTO_SH" "$ABIN"; do
            [ -f "$f" ] || { note "  no $f: build it first"; return 1; }
        done
        run "mkdir -p $ADIR/state && chmod 700 $ADIR/state" < /dev/null
        hu_put "$AUTO_BIN" "$ADIR/nbt_carplay" || return 1
        hu_put "$CPATCH_BIN" "$ADIR/conn_patch" || return 1
        hu_put "$AUTO_SH" "$ADIR/cpd.sh" || return 1
        hu_put "$ABIN" "$ASO" || return 1
        run "chmod 755 $ADIR/nbt_carplay $ADIR/conn_patch $ADIR/cpd.sh" < /dev/null
        note "  installed (no autostart flag yet). Next: $0 auto-try (by hand), then $0 auto-on."
        note "  The adapter build changed: it takes effect at the next start of the head unit."
        ;;
    try)
        # one --auto run by hand, its log on the head unit (pulled afterwards) and the head unit's live log here
        run "sloginfo -w" > "$R/slog-live-auto.txt" 2>&1 < /dev/null &
        local slogpid=$!
        note "  nbt_carplay --auto runs; it ends when the iPhone is unplugged or the head unit ends the session (Ctrl-C here stops it)"
        run "rm -f /dev/shmem/nbt-carplay-try.log; $ADIR/nbt_carplay --auto > /dev/shmem/nbt-carplay-try.log 2>&1; echo \"nbt_carplay exit \$?\" >> /dev/shmem/nbt-carplay-try.log" > "$R/auto-try-ssh.txt" 2>&1 < /dev/null
        pkill -P $slogpid 2>/dev/null; kill $slogpid 2>/dev/null
        run "cat /dev/shmem/nbt-carplay-try.log" > "$R/auto-try.txt" 2>&1 < /dev/null
        grep -a -E "auto:|iap2dev:|iap2: (identification|authentication)|pair:|exit|over" "$R/auto-try.txt" | cut -c1-200 | sed 's/^/  /' | tee -a "$R/summary.txt"
        ;;
    on)
        run "[ -x $ADIR/nbt_carplay ] && [ -x $ADIR/conn_patch ] && [ -f $ADIR/cpd.sh ] && echo on > $ADIR/autostart && cat $ADIR/autostart" < /dev/null 2>&1 | sed 's/^/  autostart: /' | tee -a "$R/summary.txt"
        note "  the service starts at the next start of the head unit"
        ;;
    off|uninstall)
        run "rm -f $ADIR/autostart; p=\$(cat /dev/shmem/nbt-cpd.pid 2>/dev/null); [ -n \"\$p\" ] && kill \$p; slay -f nbt_carplay 2>/dev/null; pfctl -a filter_USB -F rules 2>/dev/null; rm -f /dev/shmem/nbtcp-tap0.pf /dev/shmem/nbt-cpd.pid; [ -x $ADIR/conn_patch ] && $ADIR/conn_patch revert w1; ls $ADIR" < /dev/null 2>&1 \
            | sed 's/^/  /' | tee -a "$R/summary.txt"
        if [ "$what" = uninstall ]; then
            run "rm -f $ADIR/nbt_carplay $ADIR/conn_patch $ADIR/cpd.sh${PURGE:+; rm -rf $ADIR/state}; ls $ADIR" < /dev/null 2>&1 | sed 's/^/  left: /' | tee -a "$R/summary.txt"
            [ -n "${PURGE:-}" ] || note "  the pair record ($ADIR/state) stays: PURGE=1 removes it (the iPhone then asks Trust again)"
        fi
        ;;
    esac
}

# Patches c1/c2 in the running Connectivity (conn-patch/README.md). conn_patch compares the whole function
# around each patch with the car's binary first and writes nothing if anything differs; the write goes to the RAM page
# behind the process's code (the file on flash is not touched) and is gone at the next restart, or after conn-revert.
CPATCH_BIN=$HERE/../conn-patch/out/conn_patch
CPATCH=/dev/shmem/conn_patch
do_conn() {
    local what=$1 p want got
    shift
    for p in "$@"; do
        case "$p" in c1|c2|w1) ;; *) note "  unknown patch \"$p\" (c1, c2, w1)"; return 1 ;; esac
    done
    if [ "$what" != check ] && [ $# -eq 0 ]; then note "  name the patches: $0 conn-$what c1|c2|w1 ..."; return 1; fi
    step "conn-$what: push conn_patch to $CPATCH"
    [ -f "$CPATCH_BIN" ] || { note "  no $CPATCH_BIN: build it with conn-patch/build.sh"; return 1; }
    want=$(shasum -a 256 "$CPATCH_BIN" | cut -d' ' -f1)
    run "rm -f $CPATCH; cat > $CPATCH && chmod 755 $CPATCH" < "$CPATCH_BIN"   # /dev/shmem: no rename over a file
    got=$(run "cat $CPATCH" < /dev/null | shasum -a 256 | cut -d' ' -f1)
    [ -n "${DRY:-}" ] && got=$want
    [ "$want" = "$got" ] && note "  OK, SHA-256 ${want:0:16}" || { note "  MISMATCH: local ${want:0:16}, head unit ${got:0:16}"; return 1; }
    step "conn-$what $*"
    run "$CPATCH $what $*; echo rc=\$?; pidin -F '%a %N %J' | grep Connectivity" > "$R/conn-$what.txt" 2>&1 < /dev/null
    sed 's/^/  /' "$R/conn-$what.txt" | tee -a "$R/summary.txt"
}

do_conn_pull() {   # read only: the two binaries conn-patch/build.sh makes its table from
    local f out
    step "conn-pull (read-only)"
    mkdir -p "$R/files-conn"
    chmod 700 "$R/files-conn"
    for f in /opt/conn/bin/Connectivity /opt/conn/lib/carplay/devctrl/libconn_carplay.so; do
        out=$R/files-conn/$(echo "${f#/}" | tr / _)
        run "cat $f" > "$out" < /dev/null 2>/dev/null
        if [ -s "$out" ]; then
            note "  $f: $(wc -c < "$out" | tr -d ' ') bytes, SHA-256 $(shasum -a 256 < "$out" | cut -c1-16)"
        else
            note "  $f: could not read it"
            rm -f "$out"
        fi
    done
    note "  next: conn-patch/build.sh (it takes the newest runs/*/files-conn)"
}

do_cleanup() {
    step "cleanup of /dev/shmem"
    run "rm -f $CPATCH $CPLOG $P $P.part /dev/shmem/acp-cert.der /dev/shmem/carplay-pair-* $CP $CP.part /tmp/cp-session.h264 /tmp/cp-session.pcap /tmp/cp-bridge.pcap /tmp/slog-before-bridge.txt /tmp/if-before-bridge.txt; ifconfig tap0 destroy 2>/dev/null; pfctl -a filter_USB -F rules 2>/dev/null; rm -f /dev/shmem/tap0.pf; rm -rf /tmp/nbt-carplay-state /tmp/nbt-aa-native; ls /dev/shmem /tmp" > "$R/cleanup.txt" 2>&1 < /dev/null
    grep -c -E "nbt_carplay|carplay-pair|acp-cert|cp-session|nbt-carplay-state|nbt-aa-native" "$R/cleanup.txt" | sed 's/^/  probe files left: /' | tee -a "$R/summary.txt"
    hide_wifi_off
}

cmd=${1:-}
case "$cmd" in
    check|push|acp-read|acp-sign|iphone|iphone-free|session|bridge|bridge-start|bridge-ctrl|pf-check|trace|nbt-aa-pull|iap2-check|iap2-install|iap2-uninstall|auto-check|auto-install|auto-try|auto-on|auto-off|auto-uninstall|pcm-check|pcm-apply|pcm-revert|conn-pull|conn-check|conn-apply|conn-revert|cleanup|all) ;;
    *) sed -n '2,/^set -u/p' "$0" | grep -v '^set -u' | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
need_ssh
if [ -n "${DRY:-}" ]; then R=$(mktemp -d "${TMPDIR:-/tmp}/car-chip-test-dry.XXXXXX")   # never the real runs/
elif [ "$cmd" = all ] || [ "$cmd" = check ]; then new_run; else cur_run; fi
note "car-chip-test $cmd, $(date), logs in $R"
case "$cmd" in
    check) do_check ;;
    push) do_push ;;
    acp-read) do_acp read ;;
    acp-sign) do_acp sign ;;
    iphone) do_iphone ;;
    iphone-free) do_iphone_free ;;
    session) do_session ;;
    bridge) do_bridge ;;
    bridge-start) do_bridge start ;;
    bridge-ctrl) do_bridge ctrl ;;
    pf-check) do_pf_check ;;
    trace) do_trace ;;
    iap2-check) do_iap2 check ;;
    iap2-install) do_iap2 install ;;
    iap2-uninstall) do_iap2 uninstall ;;
    auto-check) do_auto check ;;
    auto-install) do_auto install ;;
    auto-try) do_auto try ;;
    auto-on) do_auto on ;;
    auto-off) do_auto off ;;
    auto-uninstall) do_auto uninstall ;;
    pcm-check) do_pcm check ;;
    pcm-apply) do_pcm apply ;;
    pcm-revert) do_pcm revert ;;
    nbt-aa-pull) do_nbt_aa_pull ;;
    conn-pull) do_conn_pull ;;
    conn-check|conn-apply|conn-revert) shift; do_conn "${cmd#conn-}" "$@" ;;
    cleanup) do_cleanup ;;
    all)
        do_check
        do_push || exit 1
        do_acp read
        if ! grep -q "device version" "$R/acp-read.txt"; then note "The chip did not answer; stopping before the signature. Logs: $R"; exit 1; fi
        do_acp sign
        echo; read -r -p "Plug the iPhone in, unlock it, then press Enter (Ctrl-C to stop here) " _
        do_iphone
        note ""; note "Done. The probe stays in /dev/shmem until the head unit restarts or: $0 cleanup"
        ;;
esac
note "summary: $R/summary.txt"
