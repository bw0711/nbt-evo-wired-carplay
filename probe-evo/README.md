# nbt_carplay_probe_evo: a step-by-step check of wired CarPlay on NBT Evo

A port of Valentin Radu's `nbt_carplay_probe` (GPL-3.0-only) from the old NBT (QNX 6.5 x86) to NBT Evo (QNX 6.5 ARMv7).
The upstream source and commit: `UPSTREAM.txt`. The licence: `LICENSE-GPL-3.0.txt` and `NOTICE-upstream.md`.
**This is GPL code: keep it separate from the paid apps, do not include it in their build.**

The probe does not install CarPlay. It checks, one step at a time, that what CarPlay needs is present on Evo. It writes nothing to flash,
does not touch scripts, configs or `medialaunch`. It puts files only in RAM: `NBT_CP_DIR`, `/fs/ram3`, `/tmp` or `/var/tmp`, the first
folder where a subfolder can really be created; otherwise flat `carplay-pair-*` files in `/dev/shmem`.

## What was changed for Evo (marked `EVO:` in the code)

| | Old NBT | NBT Evo |
|---|---|---|
| MFi chip | `/dev/i2c/i2c_0/acp`, `read()` / `pread()` | `/dev/i2c/i2c_1/acp` (`NBT_ACP_NODE`). Reading is Harman's devctl `0xC80C0502` `{send_len=1, recv_len=n, stop=1, reg}`, as in the factory `libipod-acp-har-i2c-iap2.so`. The old methods are kept as fallbacks |
| Pauses | — | 10 ms after every write and read, 300 ms after starting a signature (register 0x10), as in the factory driver |
| Shared access to the chip | — | Before every use of the chip the factory users' mutex is taken: `/dev/shm_acp_lock`, created by `acpd`. We wait 5 s. If it is not given, the probe stops. If the object does not exist, it goes on and says so |
| Test signature | 20 bytes | 20 bytes for chip 2.0, 32 bytes for 3.0 (version 0x07). `NBT_ACP_NO_SIGN=1`: no signature, read only |
| USB | the default stack | The same. `NBT_USB_PATH`: another stack |
| Build | qcc x86 | `arm-unknown-nto-qnx6.5.0eabi-gcc` 4.9 from `qnx65-armv7-toolchain:4.9`, `-mno-unaligned-access`, Mbed TLS 3.6.7 with upstream's patches |

Build: `./build.sh` → `out/nbt_carplay_probe_evo`; `.stripped` is the version without debug information, and that is the one to take to
the car. The script checks that all external symbols are in QNX 6.5's `libc.so.3`, `libusbdi.so.2` and `libsocket.so.3`.

## In the car

You need an ENET cable and an SSH master, which the owner opens himself (he types the password). Keep the connections few: after ~40
connections to port 22 in a minute, inetd closes SSH for 10 minutes.

Put the binary in RAM, nothing is written to flash:

```bash
ssh nbt 'cat > /tmp/nbt_carplay_probe_evo && chmod 755 /tmp/nbt_carplay_probe_evo' < out/nbt_carplay_probe_evo.stripped
```

First, only read the surroundings:

```bash
ssh nbt 'ls -l /dev/i2c/i2c_1/; ls -l /tmp /fs/ram3 2>&1 | head -3; pidin -p acpd; pidin | grep -i -E "mm-ipod|io-fs-media|medialaunch"'
```

The steps in order. Do not plug in the iPhone before steps 1–2, and turn Bluetooth off on it, so that wireless CarPlay does not
touch the chip meanwhile:

| # | Command | What it does | What we want to see |
|---|---|---|---|
| 1 | `NBT_ACP_NO_SIGN=1 /tmp/nbt_carplay_probe_evo acp` | open the chip, find out the reading method, the version, the certificate (read only) | `way 0 (Harman …devctl)`, `device version 0x03/0x05/0x07`, `certificate N bytes`, the start `30 82` |
| 2 | `/tmp/nbt_carplay_probe_evo acp` | plus one test signature | `signature ready`, `acp: OK` |
| 3 | `/tmp/nbt_carplay_probe_evo usb` (iPhone in USB) | what the iPhone gives over USB, changes nothing | `inserted 05ac:12xx`, the list of configurations |
| 4 | `/tmp/nbt_carplay_probe_evo usb carplay` | vendor request `0x52`; the iPhone reconnects | after it, configurations with `Apple USB multiplexor (ff/fe/02)` and `CDC NCM (02/0d)`. Pull the cable out to return things as they were |
| 5 | `/tmp/nbt_carplay_probe_evo lockdown` | the multiplexer, lockdown, questions without "Trust?" | lockdown answers |
| 6 | `/tmp/nbt_carplay_probe_evo carkit` | pairing ("Trust?" on the iPhone), iAP2 as a CarPlay car, a signature by the Evo chip | the iPhone accepts the signature: **the deciding test for path B** |

Each step writes its progress to stdout. Save it like this: `ssh nbt '/tmp/nbt_carplay_probe_evo acp' > acp.log 2>&1`.

## Risks

- **The chip is shared with factory CarPlay.** It is shared through the same mutex as the factory processes, but in steps 1–2 it is
  still better to keep wireless CarPlay inactive. If the probe crashes while holding the mutex, `acpd` will "revive" it: it
  catches `PULSE_CODE_MUTEX_DEAD`.
- **USB.** The factory `vdev-medialaunch` grabs the iPhone as an iPod. The probe attaches to a free interface; if there are
  no free ones, it says so. It does not kill head unit processes: only the full `nbt_carplay` on the old NBT does that.
  Killing `medialaunch` on Evo is a Pathology reset.
- After step 4 the iPhone stays in the "CarPlay configuration" until the cable is pulled out.
- The probe is our process, Pathology does not watch it. Its crash does not reboot the head unit.
- Step 6 saves the pairing record in RAM (`<folder>/carplay-pair`). After the head unit reboots, the iPhone will ask
  "Trust?" again.

## Checked without the car (2026-11-26)

- The build and the import check passed, there are no compiler warnings.
- **IDEmu** (firmware 24-03, real QNX 6.5 on an emulated OMAP5; there is no USB and no MFi chip there):
  - the binary loads with the firmware's libraries, `usage` is printed;
  - `acp`: there is no node `/dev/i2c/i2c_1/acp` in the emulator → `No such process`, exit 1;
  - `usb`: there is no USB stack → `connect to the USB stack failed`, exit 1;
  - `NBT_ACP_NODE=/dev/zero acp`: method 0 (devctl) is rejected (`Inappropriate I/O control operation`), methods 1 and 2
    read zeros → "no way reads the chip", exit 1. Nothing hangs and nothing crashes;
  - **the shared lock** against a test replacement of `acpd` (`test/fake_acpd.c`: creates `/dev/shm_acp_lock` as `acpd`):
    - free: it is taken and released;
    - held by another process: after 5 s "the chip is in use … stopping", exit 1;
    - after it is released: it works again, and the other process can take it again.
  - `/tmp` in the guest leads to the RAM disk `/fs/ram5`, there is no `/fs/ram3` → the files will go to `/tmp`. Check the same in the car.
- **IDEmu with USB and a real iPhone** (`emu-usb/`, iPhone15,2, iOS 27.2.1):
  - `usb`: passes;
  - `usb carplay`: the iPhone reconnects with 6 configurations;
  - `lockdown`: passes on the second run, as intended;
  - `carkit`: pairing, TLS and the carkit service pass, it stops at the chip;
  - repeated pairing is taken from the saved record.
- **Not checked (the car only):** the real chip through devctl, the pauses, the signature. Further along the chain: iAP2,
  authentication, the CarPlay session, and also the xHCI port and coexistence with `vdev-medialaunch`.
