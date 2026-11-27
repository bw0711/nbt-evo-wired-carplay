# Wired CarPlay on NBT Evo: the Connectivity side (static analysis, 2026-11-26)

Build used for addresses: `007_001_016` (`fs/sda0/repository/istep/...`). For `Connectivity` the file offset = vaddr − 0x100000; for `.so` the file offset = vaddr.
Labels: 📄 = read in the code or strings of the firmware, 🤔 = my conclusion.

## In short

- 📄 Connectivity has no code that reacts to an iPhone over USB. There are no references to `/dev/ipod0`, `ADD_USB_IAP2D`, `ncm`, `/dev/medialauncher`. The only "USB" here is the enum `USBModeUnknown/ChargingOnly/Legacy` for coexistence with wireless CarPlay.
- 📄 The Apple plugin (`libconn_carplay.so`) already looks for `_carplay-ctrl._tcp` on all interfaces and advertises `_airplay._tcp` on all of them too. So in principle it will see a session over `ncm0`.
- 📄 Connectivity connects to a found iPhone only if the BT address from `_carplay-ctrl` matches the address of the phone that is connected to the car over BT with the iAP profile and sent over iAP2-BT "wireless CarPlay is on". A session started by the iPhone itself without a request from Connectivity is dropped at once.
- 📄 `CARPLAY_DIRECT_IFNAME` is really read in two places: `mdnsd` (enables DIRECT_LINK and registers IPv6 on this interface) and `libconn_carplay.so` (transport type). In the second place the variable has no effect while `CARPLAY_WIRELESS_IFNAME` is set.
- 🤔 Result: if the phone is paired over BT and has wireless CarPlay on for this car, then on the Connectivity side a cfg edit is enough (env for mdnsd). Plus, optionally, one patch in `libconn_carplay.so`. The main work stays in medialaunch and the iAP2 driver over USB (see item 5).

## 1. How Connectivity starts CarPlay (DipoDevCtrl)

The chain (📄):
- `CSGCarPlayClient` — the SGCarPlay component is connected (0x6054ac). Then `CCarPlayImpl::checkSystemReady` (0x607cb8). If everything is ready, then `startCPDevCtrl` (0x60541c): `initCarPlayImpl` (0x608154) and `requestStartDevCtrl("/var/opt/conn/carplay/carplay.ini")` (string 0x7769c8). This brings up `libconn_carplaydevctrl.so` → `libconn_carplay.so` (AirPlay server + CarPlayControlClient).
- Conditions in `checkSystemReady` (0x607cb8):
  - persistency is ready (impl+0x740, vfunc 0xcc), otherwise the log "wait for Persistency…";
  - the HU BT address is known (impl+0x520, vfunc 0xe4), otherwise "wait for HU BT-Address…";
  - WLAN is configured (byte impl+0x608), otherwise "wait for Wlan-Configuration…".
- If something arrives later, the start goes from one of these functions:
  - `updateHUBTAddressAvailable` (0x607c34);
  - `updatePersistencyAvailable` (0x608254);
  - `updateWlanConfigured` (0x608484). This function also always requests the start of the package `CONN_CARPLAY_IAP2_PROTOCOL_MGMT` (DSI request 0x80000006 through `CDipoPowermanagementClient`, 0x2e15dc).
- `/dev/ipod_bt` and `/dev/ipod_wifi` do not affect the start of DipoDevCtrl. They are needed only for the iAP2 devctrl:
  - `/dev/ipod_bt`: after the event "BT device connected" `CDipoConnectivityClient::onBluetoothConnectionInfoUpdate` (0x6006a4). Condition: profile == 9, state == 1, DipoState == INACTIVE. The function saves the BT address and name in `CCarPlayImpl` (+0x0c/+0x10), then `checkIAP2BtMountPoint` (0x609308) does a stat of `/dev/ipod_bt` (string 0x7eb58a). If the file does not exist yet, timer polling is turned on.
  - `/dev/ipod_wifi`: only after `informationIAP2TransportReady` (0x604a9c, when DipoState == ACTIVE). Then the request `StartCarplayWlanDriver` (0x2e1694, this is mm-ipod on `/dev/carplayiap`), then `checkIAP2WifiMountPoint` (0x607dcc). If the file appeared, `CSGIAP2Client::startIAP2DevCtrl` (0x6032bc). If not, after 10 s `MountPointTimeout` (0x608068) fires → `requestDisConnectCPDevice` if the session is ACTIVE.
- The decision to connect to a specific phone (📄):
  - `updateCarPlayDeviceInfo` (0x609734) is called from onInfoUpdate of iAP2-BT (0x5fa260) or iAP2-WiFi (0x602edc). The CarPlay flag there is `deviceWirelessCarplayStatus`. On "on" `requestConnectCPDevice` (0x6094d0) is called with the saved BT address.
  - `updateControllerAdded` (0x60959c): a controller from Bonjour is connected only if its BT address equals the saved one (comparison 0x60962c, exit 0x609634) and DipoState is not ACTIVE/DEACTIVATION.
  - `requestConnectCPDevice` sets DipoState = ACTIVATION, then `CSGCarPlayClient::requestConnectDevice(btaddr)` → `CCarPlayServiceCore::connectToDevice` (`libconn_carplay` 0x3076c). There it searches for the controller by BT address and calls `CarPlayControlClientConnect`. There is no filter by interface 📄.
- 📄 `informationUpdateSessionStatus(active)` (0x6046d0): if DipoState != ACTIVATION, `requestDisconnect` is called. So a session without a prior `requestConnectCPDevice` is dropped by Connectivity at once.
- There is no path that starts CarPlay for an iPhone over USB 📄. I did not find coding/FSC as a separate gate in this chain. `setCarplayCoding` sets only the screen resolution and NavAsia. I did not trace the HMI switch `requestCarPlayActivationSwitch`.

## 2. `CDConnBMWAuxInDipo`: SwitchUSBCarPlayDevice / USBDeviceMode

- 📄 The DSI service `DConnBMWAuxInDipo` is implemented in Connectivity (`imp_nbt_conn_projectionmode_dipo_CDConnBMWAuxInDipo`). The client is `NBTMediaMainApp` (proxy `DConnBMWAuxInDipo.DConnBMWAuxInDipo`, class `CPAuxiliaryInputDeviceCarPlay`).
- 📄 `updateSwitchUSBCarPlayDevice` / `informationSwitchUSBCarPlayDevice` (0x5fcc64): Connectivity → Media, data `{bool connected, device name}`. The code is live, called from:
  - `updateDipoState` on ACTIVATION (0x608fb0, connected = 1);
  - `updateDipoState` on INACTIVE (0x609218, connected = 0);
  - `initCarPlayImpl` (0x608238).
- 📄 `requestInfoUSBDeviceMode(name, mode)` (0x5fd0bc, pointer in the table 0x7eda18): Media → Connectivity. Only a log and the answer `true`. It saves nothing and affects nothing. The mode values: `USBModeUnknown/ChargingOnly/Legacy` (table 0x7ec9c8). There is no "CarPlay" mode among them.
- 📄 The purpose on the Media side: "Device in CarPlay mode. continue in charge only mode.", "Same Phone Connected via CarPlay. bringing USB in General Lingo". This is coexistence of wireless CarPlay with the same iPhone plugged into USB as an iPod. It has nothing to do with wired CarPlay.
- 🤔 Risk: in a wired session Connectivity on ACTIVATION will tell Media "device X is in CarPlay". Media may switch the USB device with the same name to charge-only and touch the iAP2 link `/dev/ipod0`. Check on the emulator or in the car.

## 3. `libconn_carplay.so` and `CARPLAY_DIRECT_IFNAME`

- 📄 `getenv` is called in only 6 places: `CARPLAY_TRACE_CONSOLE` ×2, `HOME` ×2, `CARPLAY_WIRELESS_IFNAME` and `CARPLAY_DIRECT_IFNAME` once each. The last two are in `SocketGetInterfaceInfo` (0xbb454 and 0xbb464). The logic:
  `if (WIRELESS) { ifname == WIRELESS → type = 2 (WiFi); otherwise the type is not set } else if (DIRECT) { ifname == DIRECT → type = 0x10 (DirectLink) } else { by SIOCGIFMEDIA: 802.11 → 2, otherwise 1 }`.
  With `CARPLAY_WIRELESS_IFNAME=uap0` the type of `ncm0` stays 0. `DIRECT` is not checked at all.
- 📄 The Bonjour search for `_carplay-ctrl._tcp` (`CarPlayControlClientStart`, call of `BonjourBrowser_Start` 0x60e60) goes with ifname = NULL, that is, on all interfaces. `AirPlayReceiverServerSetProperty("interfaceName")` is not called anywhere, so `_airplay._tcp` is advertised with index 0, on all interfaces.
- 📄 `AirPlayReceiverSessionSendiAPMessage` (0x51284) lets only transport 2/0x20/0x40 through, that is, iAP over CarPlay works only for wireless. `[iAP2] LibPath=libconn_iap2_wifi_qnx.so` and `/dev/carplayiap` (trresmgr) are also only for wireless. 🤔 They are not needed to start a wired session. `informationIAP2TransportReady` will most likely not arrive in a wired session, and the 10-second `/dev/ipod_wifi` timeout will not be armed.
- 📄 **`mdnsd` (`/opt/conn/bin/carplay/mdnsd`) itself reads `CARPLAY_DIRECT_IFNAME`:**
  - 0x103f18, `SetupOneInterface`: "Enabling DIRECT_LINK on interface: %s";
  - 0x104150, `SetupInterfaceList`: separately looks for the IPv6 address (AF_INET6 = 24) of this interface and registers it.
  🤔 This is Harman's support for wired CarPlay (NCM, IPv6 link-local only). Now `mdnsd` (process 20 in the cfg) receives only env 26 `UDS_SERVER_PATH`.
- The answer to question 3: `CARPLAY_DIRECT_IFNAME=ncm0` is not needed for the search in the plugin itself, it already looks everywhere. It is needed for `mdnsd` 🤔. For the correct transport type in the plugin, patch b1 is needed (or remove `WIRELESS`, but then `uap0` is classified by media, which is risky for wireless).

## 4. `SET_HU_CARPLAY_SETTING`

- 📄 Connectivity has no string `/dev/medialauncher`. The devctl code `SET_HU_CARPLAY_SETTING` = 0x80040509 (medialaunch dispatcher 0x10a2ec, handler 0x10a6d0).
- 📄 A search for this constant (literal and the movw/movt pair) in Connectivity, `NBTMediaMainApp` and `libusbservice.so` found nothing; the literal does not occur in any ELF of the tree. Nobody in the firmware sends this devctl, and Connectivity does not either.

## 5. The `/dev/ipod0` path and which iAP2 config

- 📄 Connectivity does not use `/dev/ipod0`. Its iAP2 devctrls are hard-wired:
  - BT: `/dev/ipod_bt`;
  - WiFi: `/dev/ipod_wifi` + `/pps/services/multimedia/iap2_wifi/` (`CSGIAP2Client::startIAP2DevCtrl` 0x6032bc, strings 0x7f0173 / 0x7f014f; in 007_051_002 the strings are at 0x7f0a33 / 0x7f0a0f).
  `/dev/ipod0` is known only to `NBTMediaMainApp` (as an iPod source) and `libconn_iap2lib.so` (io-media graph for USB audio, not CarPlay).
- 📄 Device-mode iAP2 in medialaunch is apparently not `io-fs-media -dipod`. The strings `-dipod,transport=usb:busno=…` are the host-mode iPod. For device mode medialaunch looks in umass-enum for an `mm-ipod` entry ("NO mm-ipod entry in cfg file"). If it does not find one, it takes the default (0x1268e0, string 0x138d0c):
  `-diap2,probe,config=/etc/iap2.cfg -a i2c,addr=0x10,path=/dev/i2c1,speed=50000 -t usbdevice,path=/dev/otg/io-usb-dcd -vvvvvv`
  🤔 This is the QNX reference default, it does not work on NBT Evo:
  - `/etc/iap2.cfg` is not in the firmware;
  - the ACP here is `har-i2c-iap2` on `/dev/i2c/i2c_1/acp` 0x20;
  - `mm-ipod` is only in `/opt/conn/bin/carplay/`, which is not in PATH.
  There are no `mm-ipod` entries in any of the 4 `etc/umass-enum*.cfg` (the `_carplay` versions match the plain ones byte for byte). I did not trace the IAP2D branch of medialaunch to the end. This differs from the briefing ("io-fs-media -dipod"), worth re-checking.
- 📄 By the way: in `bin/start_vdev_medialaunch.sh` the functions `medialaunch_app_hu_usb_carplay` / `…2100mA_usb_carplay` with the key `-E` ("Enable iap2 probing and DiPO functionality") and `/etc/umass-enum_carplay.cfg` are commented out. They are tied to the flag file `/var/opt/sys/CARPLAY_USB_ON`. The working arguments have no `-E`.
- 📄 The driver `ipod-drvr-iap2.so` (of `mm-ipod`) supports `USBDeviceTransportComponent` + `TransportSupportsCarPlay` (`ident_info_request` 0xe230–0xe420). The parameter is added if all three conditions hold:
  - the driver flag 0x4000;
  - the transport reports support (`ipod-transport-usbdevice.so` has "Found NCM interface");
  - the key `carplay` in the `[usbhost]` section is not `off`/`no`.
  🤔 I did not find out where the flag 0x4000 comes from. The default mount point is `/dev/ipod#`, that is `/dev/ipod0` 📄.
- The answer to question 5: `/dev/ipod0` does not reach the CarPlay logic in Connectivity in any way. It is not required to start a wired session (item 1). But without it there will be no iAP2 functions through Connectivity: NowPlaying, NMEA location to the phone and so on.

## 6. Minimal changes on the Connectivity side

Precondition 📄: the iPhone is connected over BT (iAP, profile 9), and sent over iAP2-BT `deviceWirelessCarplayStatus = on`. Without BT, Connectivity will never ask for a connection (without patch b2).

**(a) Config only**
1. `/opt/conn/bin/Connectivity.cfg` — it is loaded by `opt/sys/etc/starter.cfg` 📄; the copy `/opt/conn/Connectivity.cfg` is byte for byte the same. Edits:
   - add `<Environment>` #29: `CARPLAY_DIRECT_IFNAME = ncm0`;
   - raise `<EnvironmentCount>` from 29 to 30;
   - append `<RequiresEnvironment>29</RequiresEnvironment>` to process 20 (`mdnsd`).
   Confidence: high that `mdnsd` will read it 📄; medium that this is enough for discovery on `ncm0` 🤔 (an IPv6 link-local on `ncm0` is needed; I did not check the hot-plug behaviour).
2. Add the same env 29 to process 2 (`Connectivity`). It works only together with patch b1. Medium confidence.
3. Do not touch `carplay.ini` / `iap2*.cfg` for Connectivity: `[iAP2]` is needed only for wireless. The working copies are generated in `/var/opt/conn/carplay/` 📄 (`CDipoFileHandler` for `iap2*.cfg`; 🤔 ConnStarter for `carplay.ini`).

**(b) Small binary patches** (all optional, in order of usefulness)
- b1. `libconn_carplay.so`: `ncm0` must get DirectLink when `WIRELESS` is set. The offsets are the same in 007_001_016 and 007_051_002 (the pattern was checked):
  - 0xbb474: `00 30 a0 e1` → `00 a0 a0 e1` (`mov sl, r0`);
  - 0xbb498: `12 00 00 ea` → `0a 00 a0 e1` (`mov r0, sl`; instead of exiting — the DIRECT check);
  - 0xbb4a4: `03 10 a0 e1` → `0a 10 a0 e1` (`mov r1, sl`).
  Confidence: medium. Whether DirectLink is needed for a wired session at all, I do not know 🤔.
- b2. `Connectivity`: connect without BT. Only 007_001_016, the pattern was not found in 007_051_002:
  - 0x609634 (file 0x509634): `06 00 00 0a` → `00 00 a0 e1` (do not compare the BT address);
  - 0x609648 (file 0x509648): `06 20 a0 e1` → `05 20 a0 e1` (connect by the controller's address).
  Side effects: `CCarPlayImpl` +0x0c/+0x10 stay empty (HMI and Media will get an empty name); auto-connect to any controller. Confidence: low to medium.
- b3. `Connectivity`: do not drop a session started by the iPhone. Needed only if the iPhone itself opens an AirPlay session over USB:
  - 0x604720 (file 0x504720): `7c 31 90 e5` → `00 30 a0 e3`.
  Confidence: low.

**(c) Apparently not feasible cheaply**
- Let the iAP2 functions of Connectivity go over `/dev/ipod0`. The strings can be replaced (`/dev/ipod_wifi` → `/dev/ipod0`, the PPS path), but the devctrl start is triggered only from `informationIAP2TransportReady` (the wireless path), together with starting and stopping the WLAN driver package. New code is needed. Confidence in this estimate is medium.
- Everything on the USB side is not Connectivity: role switch, `-E`, an `mm-ipod` entry with a working `iap2.cfg` (usbdevice, ACP 0x20, carplay), `SET_HU_CARPLAY_SETTING`.

## What could not be determined
- How the iPhone behaves in wired mode: does it advertise `_carplay-ctrl._tcp` on NCM or connect itself to `_airplay._tcp`. This is not in the firmware.
- Which service the Apple `CarPlayControlClient` will choose if the phone is visible both over Wi-Fi and over `ncm0`. The code has `inactiveWiFiServices`, I did not analyse it.
- Whether `mdnsd` will pick up an `ncm0` that appears after the start: there is detection of interface changes, I did not check that the DIRECT branch is called again.
- Where the flag 0x4000 for `TransportSupportsCarPlay` comes from in `ipod-drvr-iap2.so`. What BT profile 9 is — 🤔 iAP over BT.
- Whether the HMI CarPlay switch (`CarPlayActivationState`) gates the connection. Whether Media will break the iAP2 link on charge-only.
