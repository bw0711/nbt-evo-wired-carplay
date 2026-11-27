# Car visit 27.11, evening: wired CarPlay without a laptop installed and checked

Logs: `runs/20261127-163934/` (`auto-try-1/2.txt`, `auto-try.txt` — the run with Wi-Fi on, `slog-live-auto-*.txt`).
Checklist: `CHECKLIST-autostart.md`. Builds: bridge 32ec4c09, iAP2 adapter 66f20371, conn_patch 049887d8, cpd.sh
4eeb9890.

## Result
Everything per the checklist, steps 0–3 — without deviations.
- **Installation (flash, the owner's "yes"):** `/var/opt/nbt-cp/` — `nbt_carplay`, `conn_patch`, `cpd.sh`, `state/`, the
  new build of the adapter; the `autostart` flag (a separate "yes"). PCM and LibPath — from the daytime of 27.11.
- **Manual `--auto` runs:**
  1. Wi-Fi off, cable, "Apple CarPlay" in iDrive (after boot the head unit was no longer looking for the phone) →
     capture, `w1` by itself, the pair was created, Trust → CarPlay; music, Siri, wheel buttons, track on the head-up
     display; 2 min 17 s without breaks, the end — the cable was unplugged.
  2. Again: the pair from flash ("record from the state directory"), **Trust was not asked**, CarPlay 2.5 s after the
     cable.
  3. Wi-Fi on, wireless, cable for ~50 s: the bridge only waited, the wireless session was not interrupted.
- **Autostart after a restart:** the adapter "wired CarPlay service started", `cpd: started`, the bridge waits; wireless
  as before (audio, track, buttons).
- **Without a laptop (only watched the logs):** cable → Wi-Fi off → CarPlay over the cable without Trust, track on the
  instrument cluster (capture 20 s after the cable); cable unplugged → `cpd.sh` started a new bridge, pf removed; Wi-Fi
  on → wireless; Wi-Fi on + cable → wireless stayed; Wi-Fi off + cable + head unit restart → CarPlay over the cable by
  itself (capture at the 33rd second after the service start).
- Observations: at the start of the capture "attach … Resource busy" and a reconnect to the USB stack (+5 s) — normal,
  the factory iPod handler still holds the interfaces, the bridge copes. The first "bridge connected / gone" in the
  adapter log is the check of port 47600 before the capture, not a break.

## gpsmux (step 4)
(gpsmux belongs to a separate project and is not part of this repository.)

GPS 2 IP over the cable (172.16.226.1, Network selection Wifi IP) reaches the head unit, once a second. gpsmux itself
got no data from the receiver in `start-pass` (0 epochs): the navigation of this car takes GPS from the telematics unit
(`TELEMATIC_ECU=03`), the head unit's built-in receiver is not used and is silent on every boot. `stop` and `cleanup`
were done, the factory GPS scheme is in place. Details: `phone-gps/car/runs/20261127-165542/RESULT.md`. Until the
restart 2 `pidin` hang on the head unit (waiting for VersionManager) — harmless.

## What stays on the head unit
Everything from the installation + the autostart flag. Rollback: `auto-off` / `auto-uninstall` (see the checklist).

## Next
- nbt-aa navigation on the HUD over the cable (5200/5201/5202) — separately.
- `auto-check`: the service process list was fixed (do not show `dhcpd`/`acpd`).
