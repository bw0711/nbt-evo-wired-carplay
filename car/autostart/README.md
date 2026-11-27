# Wired CarPlay without a laptop

The owner's goal (27.11): wired CarPlay starts by itself, "like the factory one": uncompressed audio, Siri with the car's microphone,
steering wheel buttons, tracks on the instrument cluster and the HUD; "Trust" only once; wireless stays factory and is not affected.
The nbt-aa navigation arrows on the HUD over the cable come later, separately.

## How to use
- **Over the cable:** on the iPhone turn Wi-Fi off in Settings (not in Control Center) and plug in the cable. When the head unit starts
  looking for the phone, the service takes the iPhone over, and CarPlay appears after ~10-30 s. The first time, the iPhone asks
  "Trust This Computer?" - tap "Trust".
- **Over the air:** with Wi-Fi on the service does not touch the iPhone, even if it is on the cable (charging): wireless connects
  in 1-3 s, while the service waits for a search longer than 6 s.
- If the head unit has already stopped looking for the phone (the cable was plugged in more than a minute after Wi-Fi was turned off), in iDrive
  use "Mobile devices -> iPhone -> Apple CarPlay" or "Retry" in the error window: the head unit looks again, and the service connects.

## How it works
```
Connectivity starts -> nbtcp_iap2_adapter.so (LibPath) -> /var/opt/nbt-cp/autostart exists -> starts cpd.sh once
cpd.sh: a loop of "nbt_carplay --auto" (restarted after every session; 5 failures in a row - stop until the next boot)
nbt_carplay --auto:
  1. waits for an iPhone on USB, touching nothing (charging, the factory iPod handler, the wireless session);
  2. when an iPhone is plugged in, reads the head unit's log (sloginfo -w) and waits until the head unit has been looking for the phone for >= 6 s in a row
     ("not found in available device list" once a second; with Wi-Fi off the search lasts 58 s, with Wi-Fi on wireless
     connects in 1-3 s - measured 26-27.11);
  3. before taking over: the adapter listens on 127.0.0.1:47600 -> sets w1 (conn_patch) and turns on iAP2 for the cluster;
     otherwise it removes w1 and works without tracks; a pf rule for tap0 in the filter_USB anchor;
  4. the bridge as in bridge-start (USB CarPlay mode, tap0, iAP2 with the iPhone, CarPlayStartSession to the head unit's receiver),
     the pair for Trust in /var/opt/nbt-cp/state;
  5. the end: the iPhone is pulled out or the head unit closed the session -> the pf rule is removed, the process exits,
     cpd.sh starts a new one (the head unit looks for the phone again 2-3 s after the session ends).
```

## Files on the head unit (`car-chip-test.sh auto-install`, flash)
`/var/opt/nbt-cp/`: `nbt_carplay` (the bridge), `conn_patch` (w1), `cpd.sh` (the service), `state/` (the pair and the identity;
written once), `autostart` (the flag, `auto-on`/`auto-off`), plus the iAP2 adapter (`iap2-install`).
Log: `/dev/shmem/nbt-carplay.log` (RAM, moves to `.1` above 1 MB); adapter: `/dev/shmem/nbtcp-iap2.log`.

## Safety
- The service is a separate process; a bridge crash does not touch Connectivity (the signal handler exits without a dump - a dump
  rebooted the head unit on 26.11). The adapter starts the service without Connectivity's files (only `/dev/null`), in its own process
  group, with no zombies; it does not start it a second time while the pid from `/dev/shmem/nbt-cpd.pid` is alive.
- w1 is set only if the adapter is working. If a cable session with iAP2 broke off faster than 15 s, w1 and iAP2 for the
  cluster are turned off until the end of the trip (`/dev/shmem/nbtcp-now1`): CarPlay without tracks is better than a loop of drops.
- conn_patch checks all the code around w1; after a head unit firmware update it will refuse (CarPlay will work, with no tracks).
- It cannot be turned off without a laptop, so: 5 failures in a row - the service stays down until the next boot; 10 short runs
  in a row - no more often than once a minute. Over SSH: `rm /var/opt/nbt-cp/autostart` (from the next run) or `auto-off`.
- To flash: the pair and the identity - once; the adapter's start counter - 2 writes per boot; logs - RAM only.
- The head unit's `/tmp` is a 14 MB RAM disk (on 27.11 an overflow brought Siri down): the service writes there only small
  track-state files (`/tmp/nbt-aa-native`); pcap and dumps are turned off.

## Known limitations
- The first Trust: while you answer on the iPhone, the head unit's 58-second search may end - then "Retry".
- Without the c1 patch (not tested on the car), a session is accepted only during the head unit's search: a cable plugged in later needs
  "Retry" or selecting the mode.
- An iPhone with Wi-Fi off shows "Turn On Wi-Fi to Use Wireless CarPlay" when the head unit tries wireless.
- There is only one pair record: a second iPhone overwrites it with its own Trust (switching between phones - Trust again).
- nbt-aa navigation on the HUD over the cable: the bridge does not pass its 5200/5201/5202.

## Verified
- Mac: `nbt-carplay-evo/test-host/auto_test.c` (timestamps, the gate based on the car's measurements, old lines, the end of a session,
  the fallback mode without w1, reading through a child process), `iap2_factory_test.c`, `iap2-adapter/test_host.c` (starting the
  service: not without the flag, once, its own process group).
- IDEmu (QNX 6.5): the adapter, from a stand-in CarPlay module, started `cpd.sh` -> `nbt_carplay --auto` (the identity on
  flash was created once and read in the second run); the service has only `/dev/null` + its own script; the service survived the
  death of "Connectivity"; on a repeated start it was not started a second time; without the flag it ended and removed the pid file.
  There is no USB in the emulator: the gate and taking over the iPhone were verified only on the Mac.
- The car, 27.11 (step by step with the laptop): PCM, Siri with the car's microphone, steering wheel buttons, tracks on the HUD over the cable.

- **The car, 27.11 evening (`../RESULT-2026-11-27-evening.md`):** installation, manual runs (Trust once, Wi-Fi on -
  wireless stays), autostart after a reboot, and all the scenarios without the laptop, including a head unit reboot with the
  cable in.

## Not verified
Long driving over the cable; a second iPhone; `c1` (a cable after the head unit's search ends - for now "Apple CarPlay"/"Retry" in iDrive).
