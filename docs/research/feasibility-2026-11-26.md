# Wired CarPlay on NBT Evo: is it possible

Assessment of 2026-11-26. Car: NBT Evo `N18032I`, wireless CarPlay already activated and working.

## Proof labels

- ✅ seen on the car (tests of 21–22.10.2026, project DUMPEVO);
- 📄 only from firmware on disk (24-03 `007_051_002` in IDEmu and 2020 `x20511i-007_030_001` in the bimmulator cache), not checked against the car's 18-03 firmware;
- ❓ nobody has checked.

## ✅ 26.11, car: the iPhone accepted the NBT Evo chip for CarPlay

The probe on the car went through the whole chain up to `CarPlayAvailability` (`4300`). Steps:
- the chip was read by the factory `devctl` (2.0B) and produced a signature in 367 ms;
- the iPhone switched to the CarPlay configuration through `0x52`;
- lockdown, pairing and TLS passed, the carkit service is connected;
- iAP2: **identification accepted, authentication succeeded**.

We had to terminate the iPod handlers for this iPhone that `vdev-medialaunch` starts. The head unit stays fine.
`vdev-medialaunch` on the car matches 007_001_016 byte for byte. Details:
[car/RESULT-2026-11-26.md](../../car/RESULT-2026-11-26.md). Path B is confirmed, the next
stage is the full port of `nbt_carplay`.

## Update 26.11, late evening: other people's work

There is an open, working receiver for wired CarPlay on the old NBT: `nbt_carplay`, GPL-3.0. It works without role
switch, through vendor request `0x52`, using the head unit's MFi chip. Porting it to the Evo is the second path (B),
and right now it looks more reliable than enabling the factory stack. Details: [prior-art-2026-11-26.md](prior-art-2026-11-26.md).

## Update 26.11, evening: analysis on the Mac without the car

Details: [medialaunch](medialaunch-re-2026-11-26.md), [Connectivity](connectivity-re-2026-11-26.md).
Six builds from PSdZData are unpacked into `emulators/fw-extract/carplay/`, the tools are in `tools/`.

- 📄 `vdev-medialaunch` from 007_001_016 matches the car's binary by layout: the October addresses and line 1495
  agree. Patches and tests can now be prepared on the Mac.
- 📄 The factory port flip exists in `medialaunch`. It works with `iap2_device_mode` **off**: removal of the
  iPhone after role switch → `Role switch done!!!` → xHCI is stopped → `io-usb-dcd` → `ncm0` → `mm-ipod`.
  In October it did not work. 🤔 The main candidate is a race: the removal arrives while the record is still
  "being processed" and is ignored. It can only be confirmed from a log.
- 📄 **The October STEP8 injection was built on a mistake** and would probably have crashed `medialaunch`. Do not use it.
- 📄 The car has the 2016 `medialaunch`. Starting with 007_030_001 it sends the iPhone a CarPlay flag (`cp cap`)
  during role switch (since 2018-12) and understands `-X cp_default=1`. The imports are the same 163 symbols, and the
  scripts, configs and drivers around it are byte for byte the same. 🤔 The new binary can be installed on the car
  the same way, through `/var`.
- 📄 iAP2 over USB in device mode is brought up by `mm-ipod`, not by `io-fs-media`. Without an `mm-ipod` entry in
  `umass-enum`, the QNX reference defaults are used: `/etc/iap2.cfg` (no such file exists) and a wrong MFi chip address.
  We need our own entry: `-a har-i2c-iap2`, as for the wireless one, and our own `iap2.cfg` with the USB transport and CarPlay.
  ❓ Whether two `mm-ipod` instances can share one MFi chip.
- 📄 **Connectivity apparently does not need to be rewritten.**
  - The Apple plugin looks for the iPhone (`_carplay-ctrl._tcp`) on all interfaces.
  - Connectivity connects to the iPhone if its BT address matches the phone that is connected over BT and reported
    "wireless CarPlay is on". The owner's phone already meets these conditions.
  - `mdnsd` itself reads `CARPLAY_DIRECT_IFNAME` ("Enabling DIRECT_LINK"), but does not receive it now. This needs a
    `Connectivity.cfg` edit: env `CARPLAY_DIRECT_IFNAME=ncm0` for `mdnsd`.
  - Plus, possibly, a 3-instruction patch in `libconn_carplay.so` so that `ncm0` counts as a wired transport.
  - 🤔 So the "wired" mode will run on top of the wireless scenario: the phone must stay paired over BT.
- The emulator is still no help for the USB part: the board model has no USB. It is useful to run the boot
  scripts on the real QNX ksh and to check that the new `medialaunch` and `mm-ipod` accept our arguments.

**Assessment (judgement):** clearly better than in the morning — the main gap in Connectivity is most likely closed
by a config. The chance of reaching a picture is about 50%. The main unknowns are now on the USB side: the race in
`medialaunch`, iAP2 over USB with the MFi chip, and how the iPhone behaves on `ncm0`.

## Conclusion (morning)

**Possible in principle, not proven, can only be tested on the car.**

All of wired CarPlay is in the firmware — from USB role switching to the network interface and the Apple plugin. BMW turned it off
in scripts and configs and did not connect the pieces to each other. Writing our own CarPlay from scratch is neither needed nor realistic. The path is to enable the
factory one, with one or two binary patches.

The first half (USB) was almost completed in October. The main risk is in the second half: the factory Connectivity starts
CarPlay only through Bluetooth/Wi‑Fi, and for a cable it may have to be patched.

My assessment (judgement, not measurement): bringing up the `ncm0` USB channel with the iPhone is probably 1–2 sessions at the car. Reaching
the CarPlay picture is about 30–40%, everything depends on Connectivity.

## What was already done (October, project DUMPEVO)

Details are in the old DUMPEVO project note (it is not in this repository).

| Step | Status |
|---|---|
| `vdev-medialaunch -E` + own configs, xHCI and EHCI on different stacks (`/dev/otg/io-usb` and `/dev/io-usb/io-usb`) | ✅ the iAP2 probe of the iPhone passes (`check_iAP2_compatibility status = 1`) |
| Patch #1: 1 byte in `vdev-medialaunch` (0x125a54 `bne`→`b`) | ✅ role switch: `Role switch response: succeeded`, the iPhone becomes the USB host |
| Manual start of `io-usb-dcd -d iap2ncm-omap543x-dwcotg3 …` | ✅ the DWC3 controller works as a USB device: `/dev/otg/io-usb-dcd` appeared |
| Flip the port host→device right after role switch | ❌ the shell watchdog was too slow: the iPhone window is ~2–3 s, and fast polling of `sloginfo` overloaded the CPU and rebooted the head unit |
| Patch injection (STEP8): after role switch set `[mentry+0x54]` and case=16 so that medialaunch flips the port itself | built, **not tested** on the car |
| Deployment method: patched binary in `/var/opt/sys/patched`, `PATH` in front, one-shot flag, Pathology rolls back on a crash | ✅ |

**Lost:** the `nbt-kit` folder (patched binaries, step scripts, disassembly notes) disappeared from the Mac by 26.10.
`~/NBT-EVO-dump` is empty too. The offsets and instructions of both patches are recorded in the old note. They are valid only for the car's binary (254500 bytes). In 24-03 and 2020 the binary is
different (256300 bytes), and the offsets do not fit there.

**State of the car is unknown:** on 14.11 the files in `/var/opt/sys` from the October notes were already gone. Whether the
step scripts are still in `/fs/sda0/bin` — we did not look.

## What was found now in the firmware on disk 📄

The wired CarPlay chain as Harman designed it, and where BMW broke it:

1. **Enabling.** In `start_vdev_medialaunch.sh` the ready-made `medialaunch_app_hu_usb_carplay` branches are commented out:
   `-f /etc/umass-enum_carplay.cfg -E … -O nbt` and the flag `/var/opt/sys/CARPLAY_USB_ON`. This is so in 24-03 and in 2020.
   BMW had not enabled it up to the latest firmware.
2. **USB.** `vdev-medialaunch` itself does the role switch, stops the host, starts `io-usb-dcd` with the gadget
   `iap2ncm-omap543x-dwcotg3`, mounts `devnp-usbdnet.so` (`protocol=ncm`), brings up the interface **`ncm0`**,
   starts `io-fs-media -dipod` and broadcasts `ADD_USB_IAP2D_/dev/ipod%d`. All binaries and drivers are in place.
3. **iAP2.** The driver `ipod-drvr-iap2.so` supports `USBDeviceTransportComponent`, `USBHostTransportCarPlayInterfaceNumber`
   and `TransportSupportsCarPlay`. The transports `ipod-transport-usbdevice.so` and `ipod-transport-usbhost.so` exist.
   The MFi chip (ACP) sits on `/dev/i2c/i2c_1/acp` and already works for wireless CarPlay.
4. **Network and session.** The Apple plugin `libconn_carplay.so` understands the variables `CARPLAY_DIRECT_IFNAME` (cable) and
   `CARPLAY_WIRELESS_IFNAME` (Wi‑Fi). In `Connectivity.cfg` BMW set only `CARPLAY_WIRELESS_IFNAME=uap0`.
   Adding `CARPLAY_DIRECT_IFNAME=ncm0` is a config edit, not a code change. ❓ Whether it is enough is unknown.
5. **Connectivity is the main gap.** Before starting CarPlay it waits only for `/dev/ipod_bt` and `/dev/ipod_wifi`.
   It starts its own iAP2 with `-t bt,path=/dev/carplayiap,type=wifi`. The iAP2 library for a cable is missing in
   `carplay.ini [iAP2]`, only `libconn_iap2_wifi_qnx.so` is there. The wired `/dev/ipod0` from medialaunch goes to
   `NBTMediaMainApp` as iPod media (`USB Wired mode`), not to CarPlay.
   A lead: Connectivity has `CDConnBMWAuxInDipo::informationSwitchUSBCarPlayDevice`, `requestInfoUSBDeviceMode`
   and `updateSwitchUSBCarPlayDevice`. Some handling of USB CarPlay is built into the interface. ❓ Whether it is alive or dead.

## How what is in this repo helps

- **Firmware with a whole `/opt/conn`** (Connectivity 7.5 MB, CarPlay libraries, configs). In October Connectivity was not even
  unpacked. Now the static analysis of the second half can be done on the Mac. Before any patch, check everything
  against the car's binaries.
- **QNX toolchain (GCC 4.9) and building our own native binaries.** Instead of a shell watchdog — a small C daemon.
  It catches the USB event directly through `libusbdi` callbacks and flips the port within milliseconds, without polling `sloginfo`.
  This is the fallback if the STEP8 injection does not work.
- **Experience with safe deployment:** `hu-guard.sh` and `hu-rollback.sh`, the rule about SSH lock-out, one-shot flags.
- **The IDEmu emulator is almost useless here.** The OMAP5 board model has no USB controller. The guest has no `io-usb`,
  media engine or Connectivity (see `emulators/IDEmu/docs/PENDRIVE.md`). The Mac cannot be a USB device for the iPhone
  after role switch. There is no MFi chip in the emulator. The test bench is only the car.

## Other paths (worse)

- ~~**Our own CarPlay implementation, like NBT-AAD for Android Auto.** Unrealistic: Apple's closed protocol, MFi authentication,
  session encryption. There is no open CarPlay receiver.~~ **Wrong** (corrected on the evening of 26.11): an open receiver exists —
  `nbt_carplay` (GPL-3.0) for the old NBT, working since 23.11. See
  [prior-art-2026-11-26.md](prior-art-2026-11-26.md).
- **A dongle receiver (like Carlinkit) + our own app that shows its H.264 and plays the sound.** No
  Apple cryptography, and video, sound and input on this head unit are already solved by NBT-AAD. But it is a big separate project and the loss of
  factory integration (Siri button, mixing with navigation). It is "wired" only if the dongle takes the
  iPhone over a cable. A fallback plan.
- **A "wire→wireless" box in reverse** (iPhone on a cable, the car sees it as a wireless iPhone). It requires faking the
  iPhone side. Impossible.

## Cost and risks

- Writes are needed in `/fs/sda0` (scripts) and in `/var` (binary, configs, flags). Each test is a head unit reboot by a long
  press of the volume knob. A medialaunch crash causes a Pathology reset; the one-shot flag prevents a loop.
- With split USB stacks, flash drives on the external port may not mount. Wireless CarPlay should
  stay as it was — check every time.
- Editing Connectivity (C++, 7.5 MB) is noticeably riskier than patching medialaunch: it runs the phone, BT and Wi‑Fi.

## Plan, if we do it

1. **Session at the car, read only.** Copy to the Mac `vdev-medialaunch`, `Connectivity`, `Connectivity.cfg`,
   `carplay.ini`, `iap2*.cfg`, `start_usb.sh`, `start_vdev_medialaunch.sh`, `umass-enum*.cfg`. Look at what is now in
   `/fs/sda0/bin` and `/var/opt/sys`. Compare with 24-03.
2. **Mac.** Rebuild patch #1 and the STEP8 injection from the car's binary, step scripts for the one-shot flag. Fallback
   option — the C daemon for the port flip.
3. **Car, "USB" stage.** Goal: `ncm0` is up, the iPhone enumerates the gadget (`SET_ADDRESS`, speed≠0), the log shows
   `IAP2D case for starting drivers` and `success in starting usbdnet driver`.
4. **"iAP2 over the cable" stage.** Identification and authentication through ACP over USB. Does the head unit announce CarPlay on the USB transport.
5. **"Session" stage.** `CARPLAY_DIRECT_IFNAME=ncm0` in a copy of `Connectivity.cfg`, does mdnsd see the iPhone's `_carplay-ctrl._tcp`
   on `ncm0`, does the session start. If Connectivity does not start — analyse `CDConnBMWAuxInDipo` and decide about a patch.
6. Sound, microphone/Siri, iDrive controller, wired↔wireless switching.

After each stage we can stop, and the car returns to factory behaviour on the next boot.
