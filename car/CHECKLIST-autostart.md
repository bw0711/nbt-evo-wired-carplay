# Car visit: wired CarPlay without a laptop (installation and check)

How it all works: `autostart/README.md`. Writes to flash only with the "yes" at the car. ~45 minutes, one or two reboots;
then, last, gpsmux (step 4, another ~30 minutes and a drive).
Already installed since 27.11: PCM in carplay.ini, the iAP2 adapter (LibPath).

## 0. Preparation
1. The head unit is on, the laptop is on ENET: `ssh -B en7 root@169.254.199.119 true` (you type the password yourself; the connection closes after
   5 minutes without commands and on a head unit reboot - then run this command again).
2. `./car-chip-test.sh check`, `./car-chip-test.sh iap2-check` (LibPath is ours, the adapter says "initialized"),
   `./car-chip-test.sh auto-check`.

## 1. Installing the files (flash, "yes")
`./car-chip-test.sh auto-install` -> "installed (no autostart flag yet)". The new adapter build takes effect from the next
reboot; steps 2-3 do not need it.

## 2. One manual run (RAM)
1. Wi-Fi on the iPhone is off in Settings, the cable is still out, and the iPhone mode in iDrive is "Apple CarPlay".
2. `./car-chip-test.sh auto-try`, then plug in the cable. We wait for this in the summary:
   "auto: an iPhone is plugged in; waiting…" -> "auto: taking the iPhone over (with iAP2 for the cluster)" -> "pair: making
   the pair record" -> Trust on the iPhone -> CarPlay; `iap2dev: … authentication succeeded`, the track on the HUD.
3. Music, Siri, steering wheel buttons, the track. Pull the cable out -> the run ends.
4. **Trust only once:** run `auto-try` again and plug in the cable -> CarPlay **without** the "Trust" question (the summary has no
   "making the pair record").
5. **Wi-Fi on:** turn Wi-Fi on, wait for wireless, run `auto-try`, and plug in the cable (charging): wireless
   stays, and the summary shows only "waiting…". Pull the cable out -> the run is over.

## 3. Autostart (flash, "yes")
1. `./car-chip-test.sh auto-on` -> "autostart: on". Reboot the head unit, SSH again, run `./car-chip-test.sh auto-check`:
   the service is there (pid), the adapter log shows "wired CarPlay service started", and the service log shows "cpd: started".
2. Wireless as before (Wi-Fi on, no cable): audio, track, buttons.
3. Without the laptop (only look at `auto-check` after each):
   - Wi-Fi off -> cable -> CarPlay over the cable (no Trust), the track on the instrument cluster.
   - Pull the cable out -> the head unit looks for the phone; turn Wi-Fi on -> wireless.
   - Wi-Fi on, plug in the cable -> wireless stays.
   - Wi-Fi off, cable plugged in, reboot the head unit -> after boot, CarPlay over the cable comes up by itself.

## 4. Last: gpsmux - the iPhone's position into the factory navigation (session "GPS coordinates into CarPlay navigation")
gpsmux belongs to a separate project and is not part of this repository; the `phone-gps/...` files named in this step are not included here.
gpsmux lives only in RAM (`/dev/shmem`): any head unit reboot removes it, so do this after all the reboots of step 3.
If the head unit still has to be rebooted after that, run gpsmux again, starting with `push`.
1. Switch: on the iPhone turn Wi-Fi on and pull the cable out -> wireless CarPlay (the head unit sends GPS 2 IP over Wi-Fi, uap0).
   With Wi-Fi on, our service does not take the phone, and the cable can stay in (charging).
2. Then follow `phone-gps/car/CHECKLIST.md` from the section "At the car", steps 0-2 (`phone-gps/car/car-gps.sh check`, `pull`,
   `push`, `start-pass`, …, `start`). The same SSH. In RAM, no writes to flash.
3. **GPS 2 IP over the cable** (their section "3. Check over the cable", ~5 minutes, gpsmux is running with substitution):
   - to wired: plug in the cable (with Wi-Fi on, wireless stays), then on the iPhone turn Wi-Fi off in
     Settings -> the head unit looks for the phone -> after 10-30 s, CarPlay over the cable; `./car-chip-test.sh auto-check`: the service log
     shows "auto: taking the iPhone over";
   - in GPS 2 IP: IP `172.16.226.1`, port 50700 -> `phone-gps/car/car-gps.sh status`: "phone data from 172.16.226.100",
     is `fixes=` growing? If not within a minute, try the other Network selection items, a minute each (Wifi IP / Cellular IP / Hotspot);
   - put `172.16.224.1` back in GPS 2 IP; Wi-Fi on, pull the cable out -> wireless.
4. Their "At the end" (`log`, `stop`, `cleanup`).
5. The result for the GPS session: the path to `phone-gps/car/runs/<time>/` (logs with coordinates, do not publish) and one line -
   does GPS 2 IP arrive over the cable, and with which Network selection.

It does not overlap with our side: the service touches only `tap0` (a rule in the `filter_USB` anchor), not uap0 or port 50700;
it does not take ptys; files in `/dev/shmem` have other names. Over the cable (Wi-Fi off) gpsmux has no phone data -
it returns to the receiver by itself; this is expected.

## Rollback
- Stop the service and remove the flag: `./car-chip-test.sh auto-off` (removes the flag, stops cpd.sh and the bridge, removes w1).
- Completely: `./car-chip-test.sh auto-uninstall` (the files; the pair stays, `PURGE=1` removes it too), then, if wanted,
  `iap2-uninstall` and `pcm-revert` (the order is in `CHECKLIST-next-visit.md`).
- Without the laptop, the service stops itself after 5 failures in a row (until the next boot).
