# Factory CarPlay: the "device list", the HMI entry and the wired session (analysis, 2026-11-26)

Files: `car/runs/20261126-195127/files-conn/` — what is running in the car now. The original Connectivity is
`files/opt_conn_bin_Connectivity`. Logs: `sloginfo-wireless-baseline.txt`, `slog-live-*.txt`, `summary.txt`.

Addresses:
- `Connectivity` of the car: file offset = vaddr − 0x100000 (code and `.rodata`). The code addresses of the running and the original
  binary are the same: nbt-aa changed only two words in `.text` (item 5).
- The addresses of this binary do not match 007_001_016 from `docs/connectivity-re-2026-11-26.md`. For example,
  `updateControllerAdded`: 0x60959c there, 0x608cc4 here.
- Important: `strings -t x` prints the file offset. The address of a string in Connectivity = offset + 0x100000.
- `.so`: file offset = vaddr. Strings in the code are taken as `GOT + offset`; the GOT of `libconn_carplay.so` = 0x14fe8c.

Marks: 📄 — read in the disassembly; 🤔 — a conclusion, not checked in the code or in the car.
In the logs, the phone's name and its BT address are replaced with `<iPhone>` and `<BT MAC>`.

## In short

- 📄 All three strings (`Adding/Updating Controller in view`, `not found in available device list`,
  `Connection successful with the device`) are written by `libconn_carplay.so` (class `CCarPlayServiceCore`), not by Connectivity.
- 📄 The "list of available devices" is a `CFMutableArray` of `CarPlayController` objects (field `core+0x260`). It is filled
  only by the Bonjour search for `_carplay-ctrl._tcp` on all interfaces. The search key is the BT address of the controller: it is taken
  from the TXT key **`id`** of the `_carplay-ctrl` service. BT/iAP2 puts nothing into this list.
- 📄 The HMI menu entry depends not on this list, but on the `DipoState` state in Connectivity and on the BT part of Connectivity:
  - the event `InformationCarPlayConnect(BT address, state)`;
  - the MOST property `Bluetooth.CarPlayStatus` (Idle/Establishing/NotCapable/Error/Success/Checking/Disclaimer);
  - HMI: `CDevCarBluetooth::onMost_BTCarPlayStatus_Status`.

  So the menu entry is tied to a session that Connectivity opened, **plus the BT address of the device**.
- 📄 The wired session arrived while `DipoState = INACTIVE`. Then Connectivity asks to tear it down
  (`informationUpdateSessionStatus` 0x603e1c → `requestDisconnectFromDevice`). But the plugin has no selected controller,
  so the disconnect does not go through. 🤔 The log should contain the line "Disconnection FAILED as there is no currently selected
  device". As a result the session lives on, while `DipoState` stays INACTIVE — nothing reaches the HMI.
- 📄 Returning the screen (`requestUI`) needs only an active AirPlay session, no controller is needed for it. There is simply nobody to request it:
  the HMI does not show the entry.
- Recommendation 🤔: a wired session while the iPhone is connected over BT. Then Connectivity itself keeps `DipoState =
  ACTIVATION` and repeats the connection once a second. Our `CarPlayStartSession` in this window gives the factory registration without
  patching binaries (variant a0). More reliable is variant (a): the bridge advertises `_carplay-ctrl` on tap0 and answers
  `GET /ctrl-int/1/connect`.

## 1. The "Controller in view" list (`libconn_carplay.so`)

**Where the strings are written 📄**
- `CCarPlayServiceCore::ClientEvent(event, controller)`, 0x2cadc — the callback of `CarPlayControlClient`:
  - `event` 0 (Add/Update): if the controller is not in the array `core+0x260` (`CFArrayGetFirstIndexOfValue`, comparison by
    pointer), then `CFArrayAppendValue`. Log "Adding/Updating Controller in view" (0x2cbe0). Then a callback upward:
    `callback(+0x54)->vfunc+0x38(name, btaddr)`.
  - `event` 1 (Remove): `CFArrayRemoveValueAtIndex`, log "Deleting Controller in view" (0x2cc94), callback `vfunc+0x3c(btaddr)`.
- `getControllerInfo`, 0x2ca3c: the name is `CarPlayControllerCopyName`; the address is `CarPlayControllerGetBluetoothMacAddress` →
  `HardwareAddressToCString` (of the form "XX:XX:…", capital letters).
- `CCarPlayServiceCore::connectToDevice(btaddr)`, 0x3076c:
  - goes through `core+0x260` and compares the BT string of `getControllerInfo` with the argument;
  - found: `core+0x258 = controller`, then `CarPlayControlClientConnect(core+0x254, controller)`. On error 400 —
    up to 10 retries with `sleep(1)`. On success — the `ConnectTimeoutSec` timer from `carplay.ini` (=30 s, default 10) and
    the log "Connection successful…" (0x309ac);
  - not found: "…is not found in available device list" (0x30a7c), returns 0.
- `disconnectFromDevice`, 0x2a2dc: works only when `core+0x25c != 0`. Otherwise it writes "Disconnection FAILED as there
  is no currently selected device".

**Where the BT address of the controller comes from 📄**
- `CarPlayControlClientStart`: `BonjourBrowser_Start(browser, "_carplay-ctrl._tcp", "local.", ifname=NULL,
  flags=0x0800000000000000)`, 0x60e28–0x60e60.
- In `BonjourBrowser` (0x6d5c0…0x6d814), a set bit 59 means: the device ID is taken from the TXT key `"id"` through
  `TextToHardwareAddress`, 6 bytes. For `_airplay._tcp` the key is different — `"deviceid"`.
- `CarPlayControllerGetBluetoothMacAddress` (0x60f7c) = `BonjourDevice_GetDeviceID` of the best service of the controller.
- In total: the "BT address" in the list = `id` from the TXT of `_carplay-ctrl`.

**What calls it 📄**
- The chain upward on Add:
  - `ClientEvent`;
  - `libconn_carplaydevctrl.so`: `CCarPlayDevCtrlEvtListener::controllerAddedUpdated` 0x8044 → the SGCarPlay stub
    `informationControllerAddedUpdated` (vfunc 0xe0, DSI);
  - Connectivity: `CSGCarPlayClient::informationControllerAddedUpdated` 0x604590 → `CCarPlayImpl::updateControllerAdded`
    0x608cc4.
- Remove: `controllerRemoved` → `CSGCarPlayClient` 0x603c8c — only a log.
- The vtable slots of `CSGCarPlayClient` (0x7eea28): +0xa8 RequestClientUI 0x603c34, +0xc8 UpdateSessionStatus 0x603e1c,
  +0xcc ControllerAddedUpdated 0x604590, +0xd0 ControllerRemoved 0x603c8c, +0xdc IAP2TransportReady 0x6041ec.
- BT/iAP2 does not write into the list. It only **requests** a connection by address: `requestConnectToDevice(btaddr)` →
  `connectToDevice`.

**`CCarPlayImpl` in the Connectivity of the car 📄**

Fields:
- `impl = CDipo+0x14`;
- `impl+0x0c` — the BT address, `impl+0x10` — the BT name;
- `CDipo+0x17c` — `DipoState`: 0 ACTIVATION, 1 ACTIVE, 2 DEACTIVATION, 3 INACTIVE (string table 0x7ea2bc).

Functions:
- `CDipoConnectivityClient::onBluetoothConnectionInfoUpdate`, 0x5ffdf8: saves the address and name only if profile == 9,
  state == 1 and `DipoState == INACTIVE`.
- `updateCarPlayDeviceInfo`, 0x608e5c. Called from iAP2-BT (0x5f9a64) and iAP2-WiFi (0x6026c0). If
  `deviceWirelessCarplayStatus` is on → `requestConnectCPDevice`.
- `requestConnectCPDevice`, 0x608bf8: `updateDipoState(ACTIVATION)`, then `CSGCarPlayClient::requestConnectDevice(impl+0x0c)`.
- `updateControllerAdded(name, btaddr)`, 0x608cc4:
  - exits if `DipoState` ∈ {ACTIVE, DEACTIVATION} (0x608d38–0x608d44: `cmp r3,#1` / `cmp r3,#2`; rechecked
    by hand — with ACTIVATION and INACTIVE the function goes on);
  - exits if `btaddr != impl+0x0c` (`CHBString==` 0x608d54, `beq` 0x608d5c);
  - otherwise stops the retry timer and calls `requestConnectCPDevice`.
  
  This function publishes nothing to the HMI.
- A connection error: `responseConnectToDevice(false)` 0x604118 → the retry timer `CDipo+0x680+0x74` for 1000 ms
  (0x5fa5b0) → 0x608d94 → `requestConnectCPDevice` again, while `DipoState != ACTIVE`.

  `DipoState` stays ACTIVATION all this time. 🤔 This gives an endless retry once a second while the phone is on BT. In the
  wireless log, Bonjour found the iPhone in under a second, so there is just one "not found" line there.

## 2. How the HMI learns about CarPlay

**Connectivity → DSI 📄.** `CCarPlayImpl::updateDipoState(new, &reason)`, 0x6085f4: the log "CarPlayState: old -> new".
On every change:
- `CDConnBMWDipo` (`CDipo+0x124`) vfunc+0x2c — the attribute `DipoState`;
- `CDConnBMWAuxInDipo` (`CDipo+0x1d4`) vfunc+0x28;
- by state:
  - ACTIVATION: `SwitchUSBCarPlayDevice(1, name)` (0x5fc3f4), `InformationCarPlayConnect(address, 0, reason)`;
  - ACTIVE: stops the retry timer, stops the iAP2-BT devctrl (0x5f9c4c), `InformationCarPlayConnect(address, 1, reason)`;
  - INACTIVE: `SwitchUSBCarPlayDevice(0, name)`, `InformationCarPlayConnect(address, 3, …)` and others.
- `InformationCarPlayConnect` = `CDConnBMWDipo` 0x606160 → the stub 0x6fd334. The address is always `impl+0x0c`, that is, the BT address.
- Other attributes of `DConnBMWDipo`: CarPlayActivationState 0x605d34, DipoSiriState 0x605de0, DipoNaviState 0x605e40,
  DisableBluetooth 0x605ea0, DipoVideoReady 0x606204.
- A request from the client is `launchApp` (0x606090): "Launch DiPO's Last Active Screen" / "Launch %s app".

**The BT part of Connectivity ("SI-CARPLAY", client `DConnBMWDipo`) 📄**
- `handleSuccessfulCarPlayConnection(addr)` (log 0x24dbc8) looks for the address in its own device list. If it is not found —
  "Address of CarPlay device %s cannot be found".
- Then comes `CentralDeviceListStorage` and the MOST property `Bluetooth.CarPlayStatus` (the strings "setBTCarPlayStatus to …").
  🤔 I did not follow the chain to MOST in the code.
- 📄 The `BonjourDeviceList` handler is 0x24faa4: elements of 12 bytes `{address, name, index}`, then
  `handleBonjourDeviceList` / `tryToConnectAsBonjourDevice`. I did not find who writes this attribute on the `CCarPlayImpl` side:
  `updateControllerAdded` does not touch it.

**HMI (`HmiMain3D` from fw-extract; whether it matches the car is unknown) 📄**
- `CDevCarBluetooth::onMost_BTCarPlayStatus_Status`, 0xfda87c:
  - reads `Bluetooth.CarPlayStatus` through `CPxyBluetooth`, values 0..6 — Idle, Establishing, NotCapable, Error,
    Success, Checking, Disclaimer;
  - writes "CarPlayStateChanged %s => %s" and notifies the subscribers.
- The list of "gadgets" is built from the MOST `BTDFunctionalityViewer` (0xfda700): "CarPlay %s/%s/%s", the check
  `gadget.isValid() && m_pDevBt->isCarPlayConnected()`.
- The menu entry: `CHmiActivityMainMenu::onCarPlayStateChanged(bool)`, 0x30886c, log "CP available".
  🤔 How `CarPlayStatus` turns into "available" I did not follow to the end.
- The screen and the modes go another way: `CPxyAuxiliaryInput`, the instance `NBT_CarPlay` (`CDevCarPlay`, "CarPlay Device wants
  Screen", `onCarPlayModesChanged`). This path depends only on the session. This is why in the wired run the picture
  appeared but the menu entry did not.

**Conclusion 📄+🤔.** The menu entry is tied to the pair "`DipoState` went through ACTIVATION→ACTIVE" + "a BT address known to the BT part".
The "Controller in view" list is needed only as the way in which Connectivity opens the session itself.

## 3. A session without a controller (our wired case)

1. 📄 `libconn_carplay` `handleSessionStart` (0x293ac) → callback +0x08 `updateSessionStatus(true)` → devctrl
   0x7f20 → DSI → `CSGCarPlayClient::informationUpdateSessionStatus`, 0x603e1c.
2. 📄 Then by `DipoState` (0x603e70):
   - ACTIVATION → `updateDipoState(ACTIVE)`;
   - anything else → `requestDisconnectFromDevice` (0x5f29c8).
   
   The wired session arrives while INACTIVE → a disconnect request goes out.
3. 📄 The disconnect: `CCarPlayServiceCore::disconnectFromDevice`. There is no selected controller (`core+0x25c == 0`) → the disconnect
   is not performed. The session lives on, `DipoState` = INACTIVE.
4. 📄 So there is neither `InformationCarPlayConnect` nor BT `CarPlayStatus`. The modes still go:
   `modeChangeUpdate` → `CDipoResourceManager` → AuxIn. The screen appeared; after "minimize" there is no entry to return.
5. 📄 The return chain:
   - `CDConnBMWDipo::launchApp` → `CSGCarPlayClient` vfunc+0xf8 (requestRequestUI) → devctrl `requestRequestUI` →
     `CCarplayServiceMgr::requestUI`, 0x377f8;
   - there only `core+0x245` (the session is active) and `core+0x250` (the session) are needed → `AirPlayReceiverSessionRequestUI`, log
     "Sending Request UI command with URL=".

   🤔 I did not check whether `launchApp` depends on `DipoState`.

## 4. How to register a wired session (from least to most invasive)

**(a0) Without patches 🤔 (by the code of items 1 and 3 it should work)**

Conditions:
- the iPhone is paired and connected over BT (iAP2 over BT, profile 9);
- wireless CarPlay is enabled for the car;
- the iPhone cannot reach the head unit's Wi-Fi: Wi-Fi is turned off in "Settings", not in Control Center.

The course:
1. Connectivity saves the address and goes to ACTIVATION.
2. Every second: "not found in available device list".
3. The bridge sends `CarPlayStartSession` over USB.
4. The session at ACTIVATION → ACTIVE → `InformationCarPlayConnect(<BT MAC>)` → the BT part → HMI.

Downsides:
- the plugin has no selected controller, so the head unit will not tear the session down by itself (item 3.3);
- in run 4 the iPhone went wireless when Wi-Fi was available.

**(a) Network: the bridge advertises `_carplay-ctrl._tcp` on tap0 📄+🤔**

What to advertise:
- PTR/SRV/TXT/A: `<name>._carplay-ctrl._tcp.local`;
- the A record is an address on tap0. 172.16.226.1 (the head unit's own address) with our TCP socket will do, or a separate address in the bridge's lwIP;
- TXT `id=<BT MAC of the iPhone>` 📄 — in the format "XX:XX:XX:XX:XX:XX", it must match the address from BT;
- TXT `srcvers` is optional: only `CarPlayControllerCopySourceVersion` reads it.

What will arrive on this port 📄 (0x60728–0x60864):
```
GET /ctrl-int/1/connect HTTP/1.1
Host: <host>
User-Agent: AirPlay/280.33.8
AirPlay-Receiver-Device-ID: <deviceID of the head unit, %llu>
```
It waits for 2xx. The body is not read; 🤔 `HTTP/1.1 200 OK` with `Content-Length: 0` should be enough.

After the answer, the bridge sends `CarPlayStartSession` over iAP2-USB to 172.16.226.1:7000. The session must start within
`ConnectTimeoutSec` (30 s).

Advantages:
- the whole factory chain: "Adding/Updating" → `updateControllerAdded` → `connectToDevice`;
- there is a selected controller, the head unit will be able to tear the session down by itself.

Condition: `impl+0x0c` must be filled (the iPhone is on BT), otherwise `updateControllerAdded` drops the controller.

🤔 Not checked:
- how `CarPlayControllerCopyBestService` chooses between tap0 and uap0 if the iPhone is visible on both
  (`inactiveWiFiServices`);
- whether `mdnsd` will pick up a tap0 that came up after the start (according to `RESULT` it does; IPv4 is needed).

**(b) Configuration — no 📄**
- The `carplay.ini` keys that the plugin reads (General/Screen/Audio/HID/iAP2/Bluetooth/MAC…) neither register a device
  nor set the transport.
- `CARPLAY_DIRECT_IFNAME` changes only the transport type in `SocketGetInterfaceInfo`. It does not affect `DipoState` or registration.
- I did not find keys in `Connectivity.cfg` that would bypass the logic of items 1–3. I did not go through all of them.

**(c) Binary patches.** The running Connectivity; the original has the same bytes. File offset = address − 0x100000.
- **c1. Accepting a session started by the iPhone without a BT controller.** One word:
  - 0x603e70 (file 0x503e70): `7c 31 90 e5` (`ldr r3,[r0,#0x17c]`) → `00 30 a0 e3` (`mov r3,#0`);
  - effect 📄: `informationUpdateSessionStatus(true)` always moves to ACTIVE with reason 0, without `requestDisconnect`.
    INACTIVE→ACTIVE bypasses ACTIVATION, so Media will not get `SwitchUSBCarPlayDevice` — this is even better for USB 🤔;
  - limitation 🤔: without BT `impl+0x0c` is empty → `InformationCarPlayConnect("")` → the BT part will not find the device → there will
    most likely be no menu entry. With BT the patch is not needed (a0).
- **c2. A controller without BT** (together with (a)): write the controller's address into `impl+0x0c` instead of the comparison:
  - 0x608d4c: `05 00 a0 e1` → `06 00 a0 e1` (`mov r0, r6` = &impl+0x0c);
  - 0x608d50: `06 10 a0 e1` → `05 10 a0 e1` (`mov r1, r5` = the controller's address);
  - 0x608d54: `69 a8 ec eb` → `91 a8 ec eb` (`bl CHBString::operator=` 0x132fa0 instead of `operator==` 0x132f00;
    it returns `*this` ≠ 0, so `beq` does not fire later).

  After that the ordinary `requestConnectCPDevice` runs with this address. The name is empty.

  🤔 The HMI entry still depends on the BT part: whether the phone must be in the BT device list has not been checked. This is variant b2
  of the previous analysis, but here the address is saved.
- Without BT pairing, for the menu entry we would apparently have to patch the BT part (`handleSuccessfulCarPlayConnection`) or the HMI.
  That is already deep, and I do not propose it.

## 5. What nbt-aa changed in the running Connectivity 📄

- **ELF:**
  - the first LOAD is extended by 0x19c bytes (`filesz` 0x71da30 → 0x71dbcc). Their own code is placed there, at 0x81da30–0x81dbcc (right after
    `.eh_frame`, outside the sections);
  - the data segment is shifted in the file by +0x1000, the addresses are the same;
  - `e_shoff` and the section offsets are fixed.
- **Patch 1, 0x131fd8:** `bl 0x5d3418` → `bl 0x81da7c`. This is the factory that does `new(0x850)` and calls the constructor of
  **`CDipo`** (0x5d3418, log "[CDipo] Contructor"). `CDipo` is the factory CarPlay component, `CCarPlayImpl` in it is at +0x14.
  Their own code:
  1. calls the real constructor;
  2. calls `getenv("NBT_AA_CP")` — 🤔 so that the GOT slot of `getenv` gets resolved (0x820dc8);
  3. from the address of `getenv` in libc it computes `dlopen` (+0x16244) and `dlsym` (+0x104fc), checking the first words of the code;
  4. `dlopen("/var/opt/nbt-aa/libnbt-aa-cp.so", RTLD_NOW)` → `dlsym("nbt_aa_cp_attach")` → `nbt_aa_cp_attach(CDipo*, 1)`.
- **Patch 2, 0x281adc:** the prologue → `b 0x81da30`. If the string of the 4th argument (`CHBString`) equals `"AndroidAuto"`, the function
  returns immediately. This is the BT handler **'ServiceRequest'** (incoming requests of the A2DP/AVRCP/IAP2/OPP/HID profiles;
  strings 0x7585d8…0x758c36). 🤔 Apparently this is how the handling of nbt-aa's fake BT request is silenced.
- **Result:** the change concerns CarPlay. A foreign library gets a pointer to `CDipo`, that is, to the whole CarPlay state
  in Connectivity. 🤔 Judging by the files in `/tmp` of the car (`nbt-aa-cp.log`, `nbt-aa-label.status`), nbt-aa seems to present
  Android Auto to the HMI as a factory CarPlay — it already solves the same "register a device" task from inside the process.
  `libnbt-aa-cp.so` is not available locally.

## What is not checked and what next

- There is no full head unit log of wired run 3: the folder has only the tail in `summary.txt`. Next time, write
  `sloginfo -w` from the very start. We expect "Disconnection FAILED as there is no currently selected device".
- The link `Bluetooth.CarPlayStatus` → "CP available" in the HMI and the HMI build of the car.
- Who fills `DConnBMWDipo.BonjourDeviceList`.
- The behavior of an iPhone with BT but without the head unit's Wi-Fi: whether it will accept `CarPlayStartSession` over the cable.
- The next step:
  1. Read-only in the car: capture `/var/opt/nbt-aa/libnbt-aa-cp.so` and `/tmp/nbt-aa-cp.log` and analyze what
     `nbt_aa_cp_attach` does with `CDipo` (facts only, not code).
  2. Run (a0): the iPhone on BT, Wi-Fi turned off in "Settings", the bridge with `CarPlayStartSession`, a full `sloginfo`.
     We watch for `DipoState` ACTIVE and the menu entry.
  3. If (a0) works — add to the bridge the `_carplay-ctrl` advertisement and the answer to `/ctrl-int/1/connect` (variant a).
     Check it in IDEmu first.
