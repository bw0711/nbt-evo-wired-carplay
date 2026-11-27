# USB in IDEmu: a real iPhone in an emulated head unit

Since 2026-11-26 the IDEmu guest (firmware 24-03, QNX 6.5 on an emulated OMAP5) sees a real iPhone connected to the Mac over USB.
With it the `nbt_carplay_probe_evo` steps `usb`, `usb carplay`, `lockdown` and `carkit` pass up to the signature by the
MFi chip. There is no chip in the emulator, so the probe does not go past the signature.

## How it works

- **The board** (`IDEmu/src/qemu/omap5-uevm.c`). With `IDEMU_USB=1` it has an EHCI at `0x4a064c00` (SPI 77 = IRQ 109) with
  `id "ehci"` and an OHCI companion at `0x4a064800` (SPI 76), as in the bimmulator board. xHCI (DWC3, the car's console port)
  is not modelled: QNX's `omap5-xhci` crashes on QEMU's ordinary xHCI. This does not matter for `libusbdi` and the probe.
- **The guest** (`IDEmu/idemu/usb/start.sh`). It starts the factory `io-usb -d hb-ehci-omap3 …` with the same arguments as the car's
  `start_usb.sh`, but without xHCI. Log: `/dev/shmem/usb.log`. `boot.sh` starts it from the stage
  `IDEMU_STAGE usb`, if the script is on the shim disk.
- **Runtime and builder.** USB is turned on by the environment flag `"usb": true` or by `IDEMU_USB=1` on the server. Then the builder puts
  `usb/start.sh` on the shim, and the runtime adds `-device usb-host,bus=ehci.0,vendorid=0x05ac` (`IDEMU_USB_HOST` or
  `"usb_host"` choose another device) and disables snapshots.
- **Root.** macOS gives QEMU the iPhone's interfaces only to root. Without root 4 interfaces stay with macOS.
  `emulators/emu-usb-server.sh` starts the ordinary IDEmu server with `IDEMU_USB=1` and on exit returns the file ownership
  to the user.
- The patch with the IDEmu changes for USB only: `emulators/idemu-usb-2026-11-26.patch`. The other local changes of the bench
  are in their own patches.

## How to use it

```bash
sudo emulators/emu-usb-server.sh
```

The server runs in this window, Ctrl-C stops it. The iPhone is connected to the Mac and unlocked. After that, without sudo:
- put the probe in `IDEmu/idemu/plugins-car/_tools/nbt_carplay_probe_evo`;
- rebuild the environment `f30-business-24-03` (`POST /api/env/<id>/update`) and start it cold;
- in the guest check `/dev/shmem/usb.log`: it must have `Product : 0x12a8 (iPhone)` and `usb: ready`.

The probe steps go through `emu-usb/guest-probe.sh <step>`. It runs the step in the guest in the background with a log in
`/dev/shmem/probe-<step>.log` and fetches the result with short commands, because the console bridge has a 30 s limit.
**The order in the emulator:**

1. `usb carplay`: request `0x52`. The iPhone reconnects with 6 configurations: 5 and 6 are CarPlay.
2. `lockdown`: the first time it selects configuration 6 and asks to run again. The second time: the multiplexer and
   lockdown.
3. `carkit`: pairing ("Trust?" with a code on the iPhone), lockdown TLS, the service `com.apple.carkit.service`, TLS.
   It stops at the chip.

While the guest runs, the Mac does not see the iPhone. After the guest is stopped (`POST /api/env/<id>/stop`), macOS gets it back.

## Results of 2026-11-26 (iPhone15,2, iOS 27.2.1)

- Passthrough without root: 4× `libusb_detach_kernel_driver: ACCESS`: macOS holds the interfaces. With root there are no errors.
- `usb`: the probe through the factory QNX stack sees the iPhone (`05ac:12a8`). macOS switches it beforehand to the mode with NCM,
  so there are 5 configurations, and the fifth already has the multiplexer and NCM.
- `usb carplay`: request `0x52` gave result 0. After ~3 s the iPhone reconnected with **6 configurations**, as on the old
  NBT. The passthrough survived the reconnection.
- `lockdown`: multiplexer v2, lockdown, `ProductVersion 27.2.1`, `ProductType iPhone15,2`.
- `carkit`:
  - two RSA-2048 keys in 4–9 s on the emulated core;
  - "Trust?" → `pair: paired`;
  - lockdown TLS `TLSv1.2 ECDHE-RSA-AES256-GCM-SHA384`;
  - `com.apple.carkit.service` over TLS: connected;
  - `acp: /dev/i2c/i2c_1/acp: No such process`: there is no going further without the chip.
  The second run took the pairing record from `/dev/shmem/carplay-pair-*` and passed without "Trust?".
- **A catch:** if, without `0x52`, you select CarPlay configuration 5, which the iPhone left to macOS, the iPhone drops off
  (`libusb_set_configuration: NO_DEVICE`). Once QNX survived this (`Input/output error` and a reconnection), twice the guest
  hung: the console did not answer, QEMU at 99 % CPU, only rebooting the guest helped. So always run
  `usb carplay` first. In the car the iPhone does not have such a mode.
- Found in the probe: in the guest `/tmp` leads to the nonexistent `/fs/ram5`, and the pairing record was not saved. Now
  the probe chooses a working RAM folder itself; in the guest these are flat files in `/dev/shmem`.

## What the emulator will not check

- The MFi chip (the Evo signature, `devctl`), and therefore iAP2 identification, authentication, the CarPlay session, the picture and audio.
- The car's real xHCI port and `vdev-medialaunch`, which in the car grabs the iPhone as an iPod (it is not
  running in the IDEmu guest).
