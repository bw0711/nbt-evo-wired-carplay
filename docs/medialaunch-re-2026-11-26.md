# vdev-medialaunch: the wired CarPlay path, analysis of 2026-11-26

Static analysis on the Mac only, the car was not touched. Labels: ✅ seen on the car (October), 📄 from firmware code,
🤔 conclusion.

## Firmware on disk

From PSdZData (`/Volumes/localhost/Data/psdzdata/swe/swfl/swfl_0000271a.bin.*`) we selectively unpacked
`vdev-medialaunch`, the USB scripts and configs, `devu-iap2ncm`, `io-usb-dcd` and the whole `/opt/conn`. They are in
`emulators/fw-extract/carplay/<build>/fs/sda0/repository/istep/`. The unpacking script:
`tools/swfl_pick.py`, based on `idemu/swfl.py`.

| Build | vdev-medialaunch | Binary build date | Connectivity |
|---|---|---|---|
| 001_056_001 / 001_060_001 | 238704 | — | 5004216 (a different line) |
| **007_001_016** | **254500** | 2016-10-17, `PL_qnx_vdev-medialaunch_16421A` | 7527444 |
| 007_030_001, 007_039_008 | 256300 | 2018-12-05, `…_18493A` | 7529836 |
| 007_051_002 (24-03) | 256300 | 2018-12-05 (the same) | 7529956 |

The car's 18-03 builds (006_016_002 / 006_017_006) are not in PSdZData.

- 📄✅ **`medialaunch` from 007_001_016 matches the car's by layout.** The same size, 254500 bytes. At address
  `0x125a54` there is `bne 0x125ae8` (`23 00 00 1a`), the literal `0x125bb8` = `0x13c32c` (`iap2_device_mode`), and the log at
  `0x125b20` prints line 1495. The string `ipod_check_iap2_rolesw:1495` was in the car's live log in October.
  🤔 Most likely it is the same file. Check the SHA-256 at the next connection to the car.
- 📄 `start_usb.sh`, `start_vdev_medialaunch.sh`, `umass-enum*.cfg`, `io-usb-dcd`, `devu-iap2ncm-omap543x-dwcotg3.so`,
  `Connectivity.cfg`, `iap2.cfg` are byte for byte the same in all three 007 builds. Only `vdev-medialaunch` and
  `Connectivity` change.

## How Harman designed the wired path (mode `iap2_device_mode` = 0)

1. An iPhone is plugged in, host stack. `fn 0x11f384` connects the device (`0x112d48`). For Apple with
   `iap2_device_mode == 0` (`0x11fc10`) it writes the handle to the global `0x13c3a4`, log `IAP2 device, take a copy of mentry`
   (`0x120134`–`0x120158`).
2. The iAP2 probe thread. `ipod_check_iap2_rolesw` sends the role switch (`fn 0x1246ec`). On success it sets in the device
   record `case = 15` (`0x125afc`), log `role_switching status = 0` (line 1495).
3. The iPhone becomes the host and disappears from our bus.
   - Removal callback `0x1236d8` → preprocessing `0x1230f8` (`usb_device_removal_process`).
   - Then the bus event thread (`0x122a40`, `Process bus event - Removal`) → `usb_device_removal()` `0x1238ec`.
4. In `0x1238ec` there is a jump table on `case`. For `case 15` with `iap2_device_mode == 0` (`0x123d54`) it does:
   - `Role switch done!!!`;
   - `0x10bd18` stops the xHCI stack (`slay -f -9`, `io-usb driver stopped successfully!`);
   - `0x128858` with `case 15` → `0x128aa0`;
   - `0x110d64` starts `io-usb-dcd` (`failed to start io_usb_dcd_driver` or success);
   - `0x110438` starts the other drivers: `usbdnet`/`ncm0`, `mm_ipod`.
5. Removal in `case 16` (`0x123e40`): `Stop iAP2D drivers`, the global `0x13c3a4` is zeroed.

Conclusion 📄: the factory port flip exists in the binary, and it is enabled exactly when `iap2_device_mode` is **off**.

## What this changes in the October conclusions

- 📄 **The STEP8 injection was built on a mistake.** The October note assumed that `0x13c3a4` is filled when
  `iap2_device_mode = 1`. In fact it is written only when `== 0` (`0x11fc10 beq 0x120134`). In STEP8
  (`device_mode = 1`) it would be 0 there. Then `0x128858` would not set `[mentry+0x54]`, and `0x126470` would get NULL.
  There would probably be the same SIGSEGV as with patch #2. It is good that STEP8 was not run. **Do not use.**
- 📄 Patch #1 (force role switch with `device_mode = 1`) is not needed for the factory path. With `device_mode = 0`
  role switch goes by itself ✅ (step5, October).
- 🤔 Why step5 stopped after `Role switch response: succeeded` → `Removal Callback`. There are two branches where the removal
  can be lost silently or almost silently:
  1. `0x123540`–`0x1235bc`: `Removal callback received for unprocessed device … ignoring`. The removal arrives while the
     record is still marked "being processed" (flags `0x40000000` / `0x20000000` in `[rec+0x18]`), and the probe thread has not
     exited yet. This is a **race**, it looks like the main candidate.
  2. `0x123f60`: `usb_device_removal(): device … not found`.
  The October logs (`slog_step5_after_plug.txt`) were lost together with `nbt-kit`, so the choice between them
  is possible only by repeating step5.

## The new `medialaunch` (2018-12) knows what the car's does not

📄 In builds from 007_030_001, `vdev-medialaunch` has:
- `Role switch sending cp cap command`, `Role switch cp cap = %d status = %d`,
  `ROLESW STALL ERR = %d phone carplay capability = %d`. Together with role switch the iPhone gets the "CarPlay is needed" flag.
- The option `-X cp_default=1,aa_default=…`: CarPlay by default until Connectivity sends
  `SET_HU_CARPLAY_SETTING`.

🤔 Without the CarPlay flag, after role switch the iPhone may wait for plain iAP2 without CarPlay. This is the second reason
to try the new binary.

📄 Compatibility: both binaries have the same dependencies (`libusbdi.so.2`, `libsocket.so.3`, `libc.so.3`) and the
**same set of 163 imported symbols**. The scripts, configs and drivers around it are byte for byte the same.
🤔 The new binary should start on the car. Check it the same way as in October: a file in
`/var/opt/sys/patched`, `PATH` in front, one-shot flag.

## The next test on the car (we patch nothing)

The step5 layout from October: xHCI on `/dev/otg/io-usb`, EHCI on `/dev/io-usb/io-usb`, in the config
`xhci_iousb_path`, `ehci_iousb_path`, `dcd_iousb_path=/dev/otg/io-usb-dcd`, `iap2_probe_enable`, **without**
`iap2_device_mode`, plus `-E`. All behind the one-shot flag.

- **A.** The car's `medialaunch`, full `sloginfo` after the iPhone is plugged in.
- **B.** The same with the 2018-12 `medialaunch` from PSdZData and `-X cp_default=1`.

Log markers, in the order of the path:

```
check_iAP2_compatibility status = 1
Role switch sending cp cap command / Role switch cp cap = 1 status = 0      (B only)
Role switch response: succeeded
ipod_check_iap2_rolesw:1495 role_switching status = 0
Removal Callback …
usb_device_removal_process entered...
  → Removal callback received for unprocessed device … ignoring             (race)
  → Signal bus event thread of new Removal entry
Process bus event - Removal (vid 0x05ac …
  → usb_device_removal(): device … not found
Role switch done!!!
io-usb driver stopped successfully!  |  XHCI hub connection is not release hence exit!
NO io-usb-dcd entry in cfg file  |  failed to start io_usb_dcd_driver  |  (success)
success in starting usbdnet driver … / NCM interface: ncm0
Success in starting mm_ipod_driver!!
*** Started the required drivers successfully! ***
```

If A runs into the race (`unprocessed device … ignoring`), a targeted patch at `0x123214`–`0x12323c` is needed: do not ignore the
removal when `case == 15`. It can be prepared in advance, but only after we have the log, so as not to shoot blindly.

## Tools

- Disassembler: GNU `ntoarmv7-objdump` from the docker image `qnx65-armv7-toolchain:4.9` (`--platform linux/amd64`).
  `llvm-objdump` crashes on these files.
- For executables the file offset = vaddr − 0x100000 (code), for data vaddr − 0x101000.
- The scripts are in `tools/`: `dis.sh` (disassemble an address window), `xref.py` (search for
  `ldr` literals and `bl` to an address), `swfl_pick.py` (selective unpacking of SWFL).
