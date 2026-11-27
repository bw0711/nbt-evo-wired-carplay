# Next car visit: uncompressed audio (PCM) and iAP2 for the instrument cluster over the cable

Why and what exactly: `../docs/research/audio-pcm-re-2026-11-27.md`. In short: the head unit's receiver offers the iPhone only the formats
from `[Audio]` in `/var/opt/conn/carplay/carplay.ini`. Right now it has only AAC (the wireless set), and over the cable the iPhone does not open
audio. The edit adds 16-bit PCM lines (the wired CarPlay set); the AAC lines stay.

**This writes to the head unit's flash** (one text file). The backup is next to it on the head unit and on the Mac; the rollback is one command and
a head unit reboot. Every write needs the owner's "yes" at the car.

Time: ~25 minutes for PCM (parts 0-3) and ~30 minutes for iAP2 (part 4, one more reboot); the rollback is one more reboot.

## 0. Preparation
1. The head unit is on, the laptop is on the ENET cable. SSH (macOS sends 169.254.* over Wi-Fi, so use the adapter):
   `ssh -B en7 root@169.254.199.119 true` (you type the password yourself). The script goes through this connection; it closes
   after 5 minutes without commands and on a head unit reboot. If a stage prints `Permission denied` or `timed out`, run this
   command again, then run the stage again.
2. `./car-chip-test.sh check` (a new log folder), then `./car-chip-test.sh pcm-check`: we expect
   "8 FormatSupport lines, 0 of them PCM; backup on the head unit: no".

## 1. The edit (with the "yes")
1. `./car-chip-test.sh pcm-apply` -> "29 PCM lines added", "written, SHA-256 …".
2. Wait 30 s. Reboot the head unit: hold the volume knob for ~30 s until the screen goes dark and the head unit starts booting
   (or ignition off -> wait until the head unit goes to sleep -> on).
3. After boot, run `ssh -B en7 root@169.254.199.119 true` again and `./car-chip-test.sh pcm-check`: "37 FormatSupport lines,
   29 of them PCM" - Connectivity did not rebuild the file.

## 2. Wireless did not break (and the iAP2 recording for the next part)
- The cable is out, Wi-Fi on the iPhone is still **off**. I run `TRACE_SECONDS=180 ./car-chip-test.sh trace` (read-only),
  and after the line "recording", turn Wi-Fi on on the iPhone.
- Wireless CarPlay connects. Play music, press "next track" on the steering wheel, and look at the track title on the head-up display (HUD);
  if convenient, make a short call. Siri from the steering wheel.
- In the recording: the head unit's identification to the iPhone (`IdentificationInformation`), which iAP2 messages go with the buttons and the call,
  and which audio format the iPhone chose (`Audio SETUP received … Audio Format: …`). Analysis: `../docs/research/iap2-factory-re-2026-11-27.md`.

## 3. Cable
1. Wi-Fi on the iPhone is off in Settings, the cable is in USB, and in "Mobile devices" the iPhone is set to "Apple CarPlay".
2. `WAIT_HU=1 WAIT_HU_MAX=60 ./car-chip-test.sh bridge-start`.
3. In the head unit's log we wait for: `Audio SETUP received, … Audio Format: PCM/48000/16/2` (or 44100) and `AudioStreamPrepare … OK`.
   If instead you see `format check error`, the receiver rejected the format when preparing the stream (see item 5 of the analysis).
4. Check by ear: music in the speakers; Siri from the steering wheel button hears you through the car's microphone; a call (optional).
5. **The track buttons on the steering wheel** over the cable: do they work now that audio goes through the car? (They go as the HID device
   `ButtonInput` of the AirPlay session, not through iAP2.) We do not yet expect the track title in the cluster/HUD over the cable.

## 4. iAP2: the track in the cluster and on the HUD over the cable (after PCM is verified)
What and why: `../docs/research/iap2-factory-re-2026-11-27.md`, `iap2-adapter/README.md`. A write to flash happens
only with the "yes" at the car: the library, the path of the next adapter, the LibPath line. While the adapter is installed, it writes a start counter itself
(two short writes per head unit boot). The adapter lives in Connectivity; if two starts in a row did not last
2 minutes, the head unit gets the previous adapter directly, as before the installation.
1. `./car-chip-test.sh iap2-check` (read), then `./car-chip-test.sh iap2-install` -> "chaining to …/libnbt-cpadapter.so",
   "installed". Reboot the head unit, SSH again, run `iap2-check`: LibPath is ours, the adapter log shows "next adapter … loaded",
   "initialized", and after 2 minutes "start count cleared".
2. **Wireless did not break** (Wi-Fi on, cable out): CarPlay, audio, the track on the HUD, the steering wheel buttons - as before.
3. Cable (Wi-Fi off, cable in, "Apple CarPlay"): `./car-chip-test.sh conn-apply w1` (RAM), then
   `WAIT_HU=1 WAIT_HU_MAX=60 IAP2_FACTORY=1 ./car-chip-test.sh bridge-start`.
4. In the run summary we wait for the `iap2dev:` lines: "link to mm-ipod up", "certificate asked", "authentication succeeded",
   "mm-ipod is …", "identification accepted", "mm-ipod wants now-playing updates"; in the PPS `iap2_wifi`: `Authenticated`,
   `MediaItemTitle`, `MediaItemArtist` (that is how everything went in IDEmu with the real `mm-ipod`). The `nbt-aa:` lines show what the nbt-aa
   adapter did (on the car it is armed: "RGD start submitted" is normal, "RX lost sync" means it got confused by the bridge).
   By eye: the track in "Media", the cluster, the HUD.
5. The end of a cable session, and a second one: pull the cable out, and after 10 s run
   `ssh -B en7 root@169.254.199.119 "pidin -F '%a %N' | grep -E 'mm-ipod|nbt_carplay'"` - we expect that `mm-ipod` with
   `/dev/ipod_wifi` is gone (Connectivity stopped the driver). Plug in the cable again and run `WAIT_HU=1 WAIT_HU_MAX=60 IAP2_FACTORY=1
   ./car-chip-test.sh bridge-start`: the second session comes up, and `iap2dev:` again shows "link to mm-ipod up".
6. At the end of the visit: `conn-revert w1` (or reboot), then `iap2-uninstall` + reboot. Leave the adapter in place for
   everyday use only after wireless has been verified with it over several trips.

## Rollback (if wireless broke or something is wrong)
The order is the reverse of the installation:
1. `./car-chip-test.sh conn-revert w1`, if the patch is installed (or just reboot: it is only in memory).
2. `./car-chip-test.sh iap2-uninstall` -> "LibPath is … again" (only the LibPath line changes; the adapter files in
   `/var/opt/nbt-cp` stay but are not loaded).
3. `./car-chip-test.sh pcm-revert` -> "carplay.ini is the backup again" (the whole file from the pre-PCM copy). While the adapter is in
   LibPath, `pcm-revert` refuses and asks you to do step 2 first; `FORCE=1 ./car-chip-test.sh pcm-revert` returns the whole pre-PCM file,
   with the old LibPath line.
4. Reboot the head unit.

To turn the adapter off without rolling anything back: `touch /var/opt/nbt-cp/disable` over SSH and reboot (the head unit gets the previous
adapter directly). The pre-PCM copy of the file is also in the run folder on the Mac (`carplay.ini.before-pcm`). A repeated `pcm-apply`
with the copy still on the head unit refuses - this is by design (an old copy is not overwritten); remove it by hand only after
everything is clear.

## What counts as success
- Over the cable: audio and Siri in the car, PCM in the log.
- Over Wi-Fi: everything as before (and after the iAP2 adapter is installed).
- iAP2 over the cable: `mm-ipod` passed identification with the bridge; the track in the cluster/HUD - great if it appeared; if not,
  the adapter log, `iap2dev:` and the PPS will say at which step it stopped.
