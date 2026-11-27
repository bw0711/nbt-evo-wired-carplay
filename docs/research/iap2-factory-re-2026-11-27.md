# iAP2 for factory CarPlay over the cable: instrument cluster, HUD, steering-wheel buttons (analysis of 2026-11-27)

Task: in a cable session the head unit must get from the iPhone what it gets in a wireless one: "now playing" (the track title
in "Media", the instrument cluster, the HUD), calls, location, commands. Car visit of 2026-11-27: over the cable there is no track title in the cluster/HUD,
the track buttons on the steering wheel do not work; the iDrive controller and Siri from the wheel work (they go through the AirPlay session itself).

📄 = seen in the code/log, 🤔 = inference. The car's binaries: `car/runs/20261126-195127/files-conn/`, logs and PPS of 2026-11-27:
`car/runs/20261127-105910/`. Tools: `tools/annot.py` and a local disassembly helper.

## How it works in a wireless session
1. 📄 `libconn_carplay.so` `CCarPlayServiceCore::handleSessionStart()` (0x293ac): channel type `[+0x68]`:
   1 Enet, 2 WiFi, 0x40 WFD, 8 USB, 0x10 Direct, 0x20 BTLE, otherwise "?" (our tap0). Then (0x294a0):
   `if ((type == 2 || type == 0x40) && adapter [+0x58]) { adapter->setup(); devctrl[+0x54]->vtbl[0x48](); }`.
   The second call reaches Connectivity as `CSGCarPlayClient startWLANDriver` (0x60402c) →
   `requestStartCarplayWlanDriver` → OSCAR starts the package `CONN_CARPLAY_WLAN_DRIVER`.
2. 📄 The package = `mm-ipod -a har-i2c-iap2 -m /dev/ipod_wifi -d iap2,config=/var/opt/conn/carplay/iap2_Wifi.cfg,
   ppsdir=/pps/services/multimedia/iap2_wifi,probe -t bt,path=/dev/carplayiap,type=wifi` (+ LD_LIBRARY_PATH as for
   Connectivity). The `bt` transport (`ipod-transport-bt.so`) does only `open/fstat/read/write/close` on the path: raw iAP2
   bytes (`iAP2Probe FF 55 02 00 EE 10` goes first).
3. 📄 `/dev/carplayiap` is held by the adapter library from `carplay.ini [iAP2] LibPath` (the factory one is
   `libconn_iap2_wifi_qnx.so`, 8 KB; on the car it is the nbt-aa wrapper `libnbt-cpadapter.so`). Interface:
   `iAP2WiFiAdapterGet()/Dispose()`, class: `initialize(CConfigReader*, void* ctx, int (*send)(const uint8_t*, unsigned,
   void*))`, `setup()`, `teardown()` (empty in the factory one), `messageReceived(const uint8_t*, unsigned)`. The node is created through
   `libmmipodtrresmgr.so.1`. What `mm-ipod` writes → `send` → `CCarPlayServiceCore::iAP2WiFiSendData` →
   `AirPlayReceiverSessionSendiAPMessage` (it has `assert NetTransportTypeIsWireless`), that is, iAP2 goes **inside the AirPlay
   session**. The iPhone's replies → `messageReceived` → the node. `readConfig()` loads the adapter at start (`iAP2 transport on
   CarPlay is supported`).
4. 📄 Connectivity `checkIAP2WifiMount` (0x60751c), called only from `responseStartCarplayWlanDriver` (0x5fa19c) and
   the retry timer (0x5fa458): waits for `/dev/ipod_wifi`; if it is there and `DipoState == ACTIVE` → connect (0x602a10, PPS
   `/pps/services/multimedia/iap2_wifi/`, `iap2_connect`); if `INACTIVE` → stop the package.
5. 📄 PPS of a wireless session (captured 2026-11-27): `device` (`Authenticated::true`, name, battery, `DeviceWirelessCarPlayStatus`),
   `nowplaying`, `communications` (carrier, signal, calls), `location`, `vehicle`. In the log: `NowPlayingUpdate (0x5001)`
   from the iPhone, `LocationInformation (0xFFFB)` from the head unit.

## Why this is missing over the cable
- 📄 Channel type "?" → the branch at 0x294a0 does not run → the package is not started, Connectivity does not connect.
- 📄 Probe of 2026-11-27: a manually started `mm-ipod` opened `/dev/carplayiap` and sent `iAP2Probe`, no reply: over the cable the iPhone
  does not run iAP2 inside AirPlay (it has iAP2 over USB, with the bridge).
- 📄 There is no factory iAP2 config for CarPlay over USB: `iap2.cfg` is for Bluetooth (+ Wi-Fi handover), `iap2_Wifi.cfg` is for
  iAP2 inside a wireless session.
- 🤔 The track buttons on the wheel go through the HID device `ButtonInput` of the AirPlay session (`carplay.ini [ButtonInput]
  Buttons=NextTrack,PreviousTrack,Play`; in the cable session the device is registered). They may not have worked because
  there was no CarPlay audio in the car; check after PCM, before fixing it through iAP2.

## Scheme (proposal)
The bridge stays the only one that talks to the iPhone over USB (identification, authentication, CarPlay start), and
becomes the "iPhone" for the factory `mm-ipod`:

```
iPhone ──USB iAP2── bridge (accessory to the iPhone | "device" to the head unit) ──socket── our adapter (LibPath, in Connectivity)
                                                                              │ /dev/carplayiap
                                                                        mm-ipod (factory package) ── /dev/ipod_wifi ── Connectivity → HMI/instrument cluster
```
1. **Our own adapter** (a `.so` on the flash, `carplay.ini [iAP2] LibPath` → it): creates `/dev/carplayiap` the same way as the factory one;
   in a wireless session it passes everything on to the wrapped library (now nbt-aa, which passes it to the factory one), so
   wireless and nbt-aa work as before; in a cable session it connects the node to the bridge through a local socket.
2. **Starting the package for the cable:** either an in-memory patch of `libconn_carplay.so` 0x294a8 (`bne` → nop: the branch for any channel
   type when an adapter is loaded; `conn_patch` must be taught to patch the library by its mapping base), or the adapter itself
   (it is in the Connectivity process, `ctx` = `CCarPlayServiceCore*`) makes the same call `devctrl[+0x54]->vtbl[0x48]()`
   when the bridge connects, with a code check before it. After that everything is factory: OSCAR → `mm-ipod` → Connectivity
   connects (we have `DipoState == ACTIVE`).
3. **The bridge is iAP2 "in the middle":** towards the iPhone as now (accessory), towards `mm-ipod` in the role of the device: channel
   synchronisation, `mm-ipod`'s authentication need not be required (the device decides), accept its identification, then forward
   the control session messages in both directions: NowPlaying (5000–5002), HID (6800–6803, with component numbers recalculated),
   calls/communications (4154–4159), navigation (5200–5203), location (FFFA–FFFC), app launch, and EAP if needed.
   The bridge's identification to the iPhone must declare all this in advance (now part of it is declared).

## What to check before development (next car visit, read-only, together with PCM)
- **A full trace of a wireless session:** `sloginfo -w` to a file **before** Wi-Fi is turned on on the iPhone: the head unit's identification
  (`IdentificationInformation` with `iap2_Wifi.cfg`), which messages the head unit sends/expects, what goes out on the "next track" button on the
  wheel and on a call. The head unit's ring buffer is overwritten by video logs within a minute, so only a live recording works.
- **Wheel buttons over the cable after PCM**: if they start working, iAP2 is needed only for "now playing"/calls/navigation.

## Not checked
- Whether `mm-ipod` will go on without authentication from the "device" side.
- The API of `libmmipodtrresmgr.so.1` (node creation): analyse it from the factory adapter, which is small.
- How the nbt-aa wrapper calls the factory adapter (for the chain we need only its external interface, which is the same).
- Whether `mm-ipod` 24-03 can be run in IDEmu against the "device" bridge without a chip (then most of it can be debugged at home).
