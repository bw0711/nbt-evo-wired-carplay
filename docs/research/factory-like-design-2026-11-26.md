# Wired CarPlay "like the factory one": design sketch (2026-11-26)

The owner's goal: wired CarPlay should look and work like the factory one. An "Apple CarPlay" entry in iDrive,
the native HMI screen, the iDrive controller and the steering-wheel buttons, Siri, audio over the CarPlay channels, hints in the instrument cluster and HUD,
switching iDrive ↔ CarPlay.

## The idea: the factory session, only the wire is ours

The factory wireless CarPlay on NBT Evo works like this:
- iAP2 goes over Bluetooth, through `/dev/ipod_bt`;
- the iPhone connects to the head unit's Wi-Fi (`uap0`);
- the Apple plugin in Connectivity finds the iPhone by Bonjour (`_carplay-ctrl._tcp`) and runs the AirPlay session over IP;
- video goes through `mdev → mm-renderer → IVA-HD → window 1280`, and the HMI shows it as the CarPlay screen.

Everything except Wi-Fi works on top of IP and does not depend on the transport. The plan is to **replace Wi-Fi with the wire**:
1. Our process (a trimmed-down `nbt_carplay`) does only what the factory stack does not have for USB:
   - the `0x52` request and the CarPlay configuration of the iPhone;
   - iAP2 over `com.apple.carkit.service` with authentication by the Evo chip (checked in the car on 26.11).
2. **The bridge:** the NCM frames of the iPhone go to the virtual interface `tap0` of the head unit's network stack (`io-pkt-v6-hc`, it has tun/tap and
   bridge). For the head unit, the iPhone becomes a neighbor on an ordinary IPv6 network, as over Wi-Fi.
3. After that everything is factory: Connectivity finds the iPhone on `tap0`, opens the session itself, and the HMI shows it as an
   ordinary CarPlay.

The advantage of the approach: output, audio, Siri, the controller, the instrument cluster/HUD and the switching do not need to be built — the factory does them. The disadvantage: we need
Connectivity to accept a session that is not over Wi-Fi.

## What is already known (📄 firmware analysis, ✅ car)

- ✅ The iPhone accepts the Evo chip for CarPlay over USB: iAP2 authentication succeeded, `4300` CarPlayAvailability.
- 📄 The Apple plugin (`libconn_carplay.so`) looks for `_carplay-ctrl._tcp` on all interfaces (ifname = NULL) and advertises
  `_airplay._tcp` on all of them. There is no filter by interface.
- 📄 Connectivity connects to a found iPhone if its BT address matches the address of the phone connected over BT
  with the iAP profile that sent "wireless CarPlay enabled" (`updateControllerAdded`, `updateCarPlayDeviceInfo`).
  The owner's phone meets these conditions. ✅ In the AirPlay session on 26.11 the iPhone sent its BT address in SETUP.
- 📄 A session started by the iPhone without a request from Connectivity is torn down by Connectivity (`informationUpdateSessionStatus`). So it is better that
  the request comes from Connectivity, as with the wireless case, and not from our `CarPlayStartSession`.
- 📄 The `mdnsd` of Connectivity separately supports a "direct link" interface (`CARPLAY_DIRECT_IFNAME`). This
  variable is not set now; whether it is needed remains to be checked.
- 📄 `io-pkt-v6-hc` contains tun/tap and bridge.
- ✅ The Connectivity in the car is older than the one in 007_001_016: the addresses from the analysis do not carry over to it, and the car's binary has been captured
  (`car/runs/20261126-195127/files/`).

## Unknowns (in the order of checking)

1. Whether `tap0` is created in the car (`ifconfig -C`, `ifconfig tap0 create`) and whether `/dev/tap0` appears. This is a change
   in RAM only; `ifconfig tap0 destroy` returns everything as it was.
2. Whether the iPhone advertises `_carplay-ctrl` on the wired NCM network after iAP2 over USB, but without our `CarPlayStartSession`.
   If not, which iAP2 signal switches it on.
3. Whether the `mdnsd` of Connectivity sees the iPhone on `tap0`: the interface is created after `mdnsd` starts, but it has tracking
   of interface changes.
4. Which network the iPhone picks if both Wi-Fi and the wire are available. By iOS logic the wire is preferred when the car
   has advertised CarPlay over USB. It may be necessary to keep the iPhone off the head unit's Wi-Fi: leave Bluetooth on, turn Wi-Fi off in the iPhone.
5. Whether Connectivity will tear the session down because of the transport type: `IsWiFiNetworkInterface`, "wireless" in its state.
6. What to do with the iPod stack: `vdev-medialaunch` does not bring up iPod after the configuration change. This is expected;
   with the factory wired CarPlay, iPod is not needed either.

## First test in the car

1. Read-only: `ifconfig -C`, `ifconfig -a`, the `mdnsd`/`mm-renderer` processes, the CarPlay state in the log.
2. With permission: `ifconfig tap0 create`, check `/dev/tap0`, then `ifconfig tap0 destroy`.
3. The bridge (a new mode `nbt_carplay --bridge`): USB + iAP2 authentication + NCM ↔ `tap0`, without our own AirPlay. In the
   iPhone, Bluetooth is on (the phone is paired with the car) and Wi-Fi is off. We watch the Connectivity log and the screen: will
   the HMI start the effectively wireless CarPlay over the wire by itself.

## Fallback path: our own output (the recipe is known)

If Connectivity cannot be persuaded, our `nbt_carplay` runs the session (as on 26.11) and we do the output ourselves. The recipe comes
from the analysis of Android Auto for Evo (`evo-aa-video-path-2026-11-26.md`). That project has the PolyForm Noncommercial license,
so we do not take its code, only facts.
- **Video:**
  - the factory `/opt/conn/bin/carplay/mm-renderer`, a context through `mmr_*`;
  - input `mdevpush:/dev/name/local/mdev?fourcc=H264&…` (or `AVC1`, as in the factory one), frames as `MsgSendvs_r` messages:
    5 sync, 2 format, 4 SPS/PPS, 3 frame with PTS;
  - output `screen:/any?winid=1282&…` into our own window, on top of iDrive through LayerManager (render order `0x502`,
    surface 256 visibility 0).
  - There is only one hardware decoder: the factory `cpvideoctx` context must be detached, not destroyed.
- **Audio:** the CarPlay A2I points `/input_stereo_AirPlayMain`, `/input_stereo_AirPlayAlt`, `/input_mono_AirPlayDL`,
  `/output_mono_AirPlayUL`. The CarPlay sources are allocated only when the factory stack sees the iPhone.
- **Input:** the controller through MOST FBlock `0xE5`, touch by UDP 30567.
- It will work, but the HMI will not know about CarPlay: our own menu entry, our own switching — not "like the factory".

## Car, evening of 2026-11-26: the factory path over the wire showed CarPlay; how the factory wireless one gets into the menu

The wired run (`--bridge-start`, BT and Wi-Fi off in the iPhone): the factory receiver accepted the session, CarPlay is on the screen,
but there is no entry in iDrive (details in `car/RESULT-2026-11-26.md`, the third section).

The log of the factory **wireless** connection (`car/runs/20261126-195127/sloginfo-wireless-baseline.txt`) shows
that the initiator is Connectivity, not the iPhone:
1. BT iAP2 → `DevCtrl: Device with BT Address=… is not found in available device list`.
2. `CarPlayControl: Controller [0x…] Activated Service` — Bonjour found the `_carplay-ctrl._tcp` of the iPhone on uap0
   (`resolved to <iphone>.local. port 61988 interfaceNdx 9`).
3. `DevCtrl: Adding/Updating Controller in view: Name …, BT Addr …` — **the device is in the list, this is where the menu entry comes from**.
4. `CarPlayControl connecting to <iphone>.local.%9 on port 61988` → `Connection successful with the device with BT
   Address=…` — Connectivity asks the iPhone to start a session.
5. The iPhone connects to AirPlay :7000: pair-setup HK, pair-verify, MFi, Setup → `CarPlay Session created` →
   `Session is UP and RUNNING on Transport: WiFi` → `AirPlay session started: From=… D=0x<BT MAC> A=172.16.224.x`.

Over the wire, steps 2–4 are skipped: the iPhone does not advertise `_carplay-ctrl` into the cable (it waits for `CarPlayStartSession`), while the factory
`mdnsd` looks for it there. Options for the menu entry:
- (a) an iPhone known by BT (wireless CarPlay enabled) + our `CarPlayStartSession` over the cable: a session from the same
  `D=<BT MAC>` may attach itself to the already added "Controller in view";
- (b) advertise `_carplay-ctrl` on tap0 on behalf of the iPhone (its control port, if it listens on NCM too), so that
  Connectivity itself goes through steps 2–4 over the wire;
- (c) analysis of `libconn_carplaydevctrl.so` / Connectivity (captured from the car: `car/runs/20261126-195127/files-conn/`,
  the running Connectivity is modified by nbt-aa, the original is `Connectivity.nbt-orig`): what exactly makes the HMI entry and how
  to add a device without BT.
