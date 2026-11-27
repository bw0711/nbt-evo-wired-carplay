# nbtcp_iap2_adapter: the head unit's factory iAP2 for wired CarPlay

Why and how it works on the head unit side: `../docs/research/iap2-factory-re-2026-11-27.md`. In short: the instrument cluster, the HUD and "Media" get
"now playing", calls and the rest from the factory `mm-ipod` driver, which talks to the iPhone over iAP2 **inside** the wireless
AirPlay session. Over the cable the iPhone runs iAP2 over USB, with our bridge. This library and the bridge's `--iap2-factory`
connect the factory `mm-ipod` to the bridge, and the bridge answers it in place of the iPhone.

```
iPhone ──USB iAP2── nbt_carplay --iap2-factory (cp_iap2dev.c: the "device" for mm-ipod)
                          │ 127.0.0.1:47600, frames [1][length 2 bytes][iAP2 bytes]
              nbtcp_iap2_adapter.so (carplay.ini [iAP2] LibPath, in the Connectivity process)
                          │ passes everything to the next adapter (the factory one or nbt-aa), replacing only send()
              next adapter → node /dev/carplayiap → mm-ipod (package CONN_CARPLAY_WLAN_DRIVER) → /dev/ipod_wifi → Connectivity
```
Without the bridge `send()` goes into the AirPlay session as before: wireless CarPlay and nbt-aa do not change. For a cable session
the in-memory patch `w1` is also needed (`../conn-patch`): otherwise the CarPlay module starts `mm-ipod` only for Wi-Fi.

## Files on the head unit (installed by `car-chip-test.sh iap2-install`, flash)
- `/var/opt/nbt-cp/nbtcp_iap2_adapter.so` — the library;
- `/var/opt/nbt-cp/iap2-next` — the path of the adapter it stands in front of (the one that was in LibPath);
- `/var/opt/nbt-cp/starts` — the start counter (protection against a reboot loop; two short flash writes on every
  start of Connectivity, i.e. on every head unit boot), `/var/opt/nbt-cp/disable` — the kill switch;
- one line is changed in `/var/opt/conn/carplay/carplay.ini`: `[iAP2] LibPath`; the copy of the file before the edit is
  `carplay.ini.before-iap2`. The line is rolled back by `iap2-uninstall`.
- the adapter's log: `/dev/shmem/nbtcp-iap2.log` (RAM).

## Safety
The library lives inside Connectivity: a crash = a dump and a head unit reboot. The CarPlay module creates the adapter once per
Connectivity start (`CCarPlayServiceCore::start` → `readConfig`; on the car the nbt-aa adapter writes to the log about 22 s
after boot). Therefore:
- if two starts in a row did not last 120 s, or `/var/opt/nbt-cp/disable` exists, `iAP2WiFiAdapterGet` gives the CarPlay
  module the object of the next adapter (nbt-aa or the factory one) as it is: after that not one of our functions is between them, everything is as
  before the installation, only iAP2 over the cable is off. The counter is reset after such a start, the next one tries again.
  The CarPlay module itself checks the object for 0 and calls `Dispose` from our library (analysis of `readConfig` 0x35cb0..0x35d40),
  our `Dispose` recognises a foreign object and hands it to the next adapter's `Dispose`. The next adapter is loaded the same way as
  the module loaded it (`dlopen(path, RTLD_LAZY)`): the nbt-aa library needs `__cxa_*` from the C++ environment of Connectivity itself;
- a start that lived 120 s or ended with a clean `Dispose` resets the counter (short head unit power-ons in a row give
  at most one start without iAP2 over the cable, and nbt-aa works meanwhile);
- `setup`/`teardown` are passed on as they are: the module calls `teardown` when any session stops, also without
  `setup` (this was the case in the car for all cable sessions of 26–27.11 with the nbt-aa adapter, with no failures); with `w1` the pairs over the cable are
  the same as over Wi-Fi (`handleSessionStop` 0x29ec8). The stop of `mm-ipod` after a session was confirmed in the car on 2026-11-27: at the end of every cable session
  OSCAR writes "Daemon mm-ipod terminated!", and the Bluetooth `mm-ipod` comes up again; the next session goes like the first;
- the code is small and does not parse foreign data: it only forwards frames; one bridge at a time; write timeouts are 1 s.

## What will go to the instrument cluster over the cable
The bridge (`cp_iap2dev.c`) passes between `mm-ipod` and the iPhone only three pairs: "now playing" (5000/5001), calls
(4154/4155), communications (4157/4158); the bridge's own identification to the iPhone (accepted in the car on 2026-11-23) declares them.
The `mm-ipod` config (`/opt/conn/data/carplay/iap2_Wifi.cfg`) also asks for the cover art (file transfer), the car's GPS for the iPhone,
the battery charge, app launch, an external protocol; these will not be there over the cable (cover art is the next step, if the track works).

## Checked
- Mac (ASan/UBSan, `test_host.c`): calls are passed on unchanged; without the bridge, to "AirPlay" with the original ctx;
  with the bridge, to the bridge, and the bridge's frames (also split ones) to `messageReceived`; a second bridge is rejected; the bridge leaving/returning;
  the reboot-loop protection (safe mode gives the next adapter's object, which sends to AirPlay directly;
  the `disable` kill switch; the counter reset on a clean `Dispose`); `teardown` without `setup` and a repeated `setup`/`teardown` pair.
  A test instability (header and data as separate writes) was found and fixed: 25, 10 and 12 runs without failures.
- Install and rollback (`car-chip-test.sh` on a local copy of the "head unit" with `carplay.ini` and the nbt-aa adapter from the car):
  PCM → iAP2 → rollback in order gives the original file byte for byte; `pcm-revert` refuses while the adapter is installed
  (`FORCE=1` returns the whole file to before PCM, after which `iap2-uninstall` refuses, and a repeated install works).
- QNX 6.5 in IDEmu (the `bench/` bench, build SHA-256 `82c4020e…`, 2026-11-27): the adapter loads the "next" one (`testnext.so`,
  creates the node of the factory `libmmipodtrresmgr.so.1`), the node `/dev/carplayiap` exists, the bridge intermediary (`iap2dev_run`)
  connects, the adapter switches to cable mode; counter 0→1. Safe mode (counter 2, then the `disable` file):
  the fake CarPlay module got the `testnext.so` object directly and created the node through it, the bridge port is closed, counter 0.
  (Files get into the guest only after its disk is rebuilt: before 2026-11-27 an old build of the adapter was there.) The factory `mm-ipod` 24-03 was started with the car's arguments, but without an MFi chip it waits for it forever
  (`Failed to initialize Auth co-processor`): the intermediary's exchange with the real `mm-ipod` can only be done in the car.

- **The whole chain on QNX in IDEmu with real libraries** (2026-11-27): the fake CarPlay module `bench/fakeplugin2`
  (gives the adapters `CConfigReader::getString` and `CUtilsMutex`, like Connectivity), our adapter → the nbt-aa adapter from the car
  (`libnbt-cpadapter.so`, not armed: "not armed, passthrough") → the factory `libconn_iap2_wifi_qnx.so` → the node
  `/dev/carplayiap` → the **real `mm-ipod`** with the car's arguments and its `iap2_Wifi.cfg`, with the fake chip
  `bench/fakeacp` (the car's certificate, an invented signature) → the bridge (`iap2dev_run`, checking the signature of `mm-ipod`).
  Result: the connection, the signature check, the identification, `mm-ipod`'s subscriptions to 5000/4154/4157, and an invented "now playing" reached
  it; in PPS `iap2_wifi`: `nowplaying` — `MediaItemTitle`, `MediaItemArtist`, `PlaybackStatus::Playing`; `device` —
  `Authenticated::true`, `DeviceName`. Without the bridge `mm-ipod`'s bytes go to the "AirPlay session" with the original context
  (the wireless path); a clean finish resets the counter; safe mode gives the module the nbt-aa object, and the chain
  nbt-aa → factory → node works as before the installation (`bench/mmipod_node` is a fake `mm-ipod` on the node).
- The whole bridge in the `--bridge-start --iap2-factory` mode (Mac, `nbt-carplay-evo/test-host/iap2_factory_test.c`,
  15 runs): the bridge's own work did not change (connection, chip, identification, `CarPlayStartSession` to the head unit's receiver, its
  `media.state`), `mm-ipod`'s subscriptions go to the iPhone unchanged, the iPhone's updates go to `mm-ipod`, also those longer than a packet.

## What `mm-ipod` wants from the iPhone (IDEmu, its identification with the car's config)
Sends: ea02, 5000, 5002, 5003, ae00, ae02, fffb, 4154, 4156, 4157, 4159, 415a–4161, 5700, 5703, 10da, 10dc, 10dd.
Accepts: 5001, ae01, fffa, fffc, 4155, 4158, 4e09–4e0d, 5701, 5702, 10db.
The bridge passes only "now playing", calls and communications. Will not go over the cable: call commands from the car (accept, reject and
so on, 415a–4161), the iPhone's time/UUID/language (4e0a–4e0d: they are in the wireless PPS `device`), battery charge, the car's GPS, app
launch. Adding them means changing the bridge's identification to the iPhone (its current form was accepted in the car), a separate step.

## Not checked
- That Connectivity will show the track in the instrument cluster and on the HUD from these PPS, and what a real iPhone will answer.
- **nbt-aa in the armed mode** (in the car it is armed: "armed persistently", "proxy active, RGD enabled"). Then it
  inserts its own navigation component and 5200 messages with packet renumbering into the `mm-ipod` ↔ iPhone stream (for
  navigation on the HUD). The bridge does not pass 5200 from it: nbt-aa's navigation on the HUD will not work over the cable; if nbt-aa
  loses sync because of this ("RX lost sync"), the run's results will have `nbt-aa:` lines. It cannot be checked without its licence.
- Safe mode on a real head unit.

## Build
`./build.sh` (Docker `qnx65-armv7-toolchain:4.9`) → `out/nbtcp_iap2_adapter.so`, a check of the exports (only
`iAP2WiFiAdapterGet/Dispose`) and the imports (libc.so.3, libsocket.so.3). The IDEmu bench: `bench/build.sh` (`testnext.so`,
`fakeplugin`, `fakeplugin2`, `mmipod_node`, `fakeacp`, `iap2dev_run`). Files are put into the guest in
`emulators/IDEmu/idemu/plugins-car/_tools/` and get there only after the guest's disk is rebuilt (stop, `POST
/api/env/<id>/update` with the same coding, start); put the factory adapter in the guest's `/opt/conn/lib/carplay/devctrl/`
(nbt-aa looks for it there), the car's certificate for `fakeacp` is `_tools/acp-cert.der` (local only).
