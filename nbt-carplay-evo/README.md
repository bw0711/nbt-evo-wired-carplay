# nbt_carplay for NBT Evo: a port of wired CarPlay

A port of Valentin Radu's `nbt_carplay` (GPL-3.0-only) from the old NBT (QNX 6.5 x86) to NBT Evo (QNX 6.5 ARMv7).
Upstream and commits: `UPSTREAM.txt`; licence: `LICENSE-GPL-3.0.txt` and `NOTICE-upstream.md`.
**GPL code: keep it separate from the paid apps.**

Basis: on 2026-11-26 in the car the probe passed iAP2 authentication with the Evo chip, and the iPhone sent `CarPlayAvailability`
(`car/RESULT-2026-11-26.md`).

> **State on 2026-11-27.** The working mode is `--auto` (a service, `../car/autostart/README.md`): the `--bridge-start` bridge
> to the head unit's factory receiver, PCM audio, iAP2 for the instrument cluster (`--iap2-factory`). Stage 2 below (our own video and audio output) was not
> needed: the picture, audio, microphone and buttons come from the factory receiver. The sections below are the history of the port. The overall picture and
> the installation: `../README.md`; what was changed against upstream, file by file, is there too.

## Stages

1. **A session without output**: now. USB, lockdown, carkit, iAP2, `CarPlayStartSession`, the network over NCM (lwIP inside the
   process), AirPlay. The screen is written to a file (`--dump`), no audio and no microphone (`--no-audio --no-mic`).
2. **Output.**
   - Video to the screen through the Evo's hardware decoder. Factory CarPlay and Android Auto on Evo output through
     mm-renderer to a 1280 window; on the old NBT `nbt_decoder` did this.
   - Audio and microphone through A2I: `/opt/mm/lib/liba2i_client.so`, the point names on Evo are still to be found out.
   - The iDrive controller, entry in the menu, storing the pairing.

## What was changed against upstream

| File | Change |
|---|---|
| `cp_acp.c` | The Evo chip: `/dev/i2c/i2c_1/acp` (`NBT_ACP_NODE`), reading with Harman's `devctl 0xC80C0502`, the factory pauses, the shared lock `/dev/shm_acp_lock` around every use of the chip. The NBT method is kept as a fallback. Debugging on the bench: `NBT_ACP_CERT_FILE` (if there is no chip node, the certificate comes from a file, the signature does not pass) |
| all `.c` | `"/fs/ram3/…"` → `CP_RAM "/…"`, `CP_RAM` = `/tmp` (in the car the RAM disk is `/fs/ram5`), changed with `-DCP_RAM=…` |
| `nbt_carplay.c` | `--state` defaults to `CP_RAM/nbt-carplay-state` (RAM). Upstream has `/var/opt/hmi/…`, that is, flash. At start `CP_RAM/nbt-aa-native` is created |
| `cp_audio.c` | `liba2i_client.so` is looked for first in `/opt/mm/lib` (Evo) |
| `cp_usb.c` | iPod handlers (`-dipod`, `busno=`, `devno=`) on Evo are recognised the same way as in upstream (checked in the car); on 2026-11-27 the `--auto` wait was added: the iPhone is not touched until the head unit is looking for a phone |

## Build

`./build.sh` → `out/nbt_carplay` (+ `.stripped`).
- lwIP 2.2.1 and Monocypher 4.0.2 are in `.cache/`, at the commits from upstream's `carplay_deps.py`.
- Mbed TLS 3.6.7 is taken from `../probe-evo/.cache` (build `../probe-evo/build.sh`).
- The compiler is GCC 4.9 ARM with `-mno-unaligned-access`.
- The imports are checked against QNX 6.5's `libc.so.3`, `libusbdi.so.2` and `libsocket.so.3`.
- 2026-11-26: builds without warnings, the import check passed.
- **2026-11-26 20:08, car: stage 1 passed on the first run.** A full CarPlay session: H.264 1280×480, 614 frames
  in 20 s. Details: `car/RESULT-2026-11-26.md`. The picture is not output to the head unit's screen: that does not exist yet
  (stage 2), it is only written to a file (`--dump`).

## Testing

- **IDEmu with an iPhone** (`../emu-usb/README.md`). It needs the server started with `sudo emulators/emu-usb-server.sh` and an iPhone
  connected to the Mac. The binary and the car's real chip certificate are already in `IDEmu/idemu/plugins-car/_tools/`.
  ```bash
  emu-usb/guest-run.sh cp-iap2 "NBT_ACP_CERT_FILE=/fs/shim/carplug/_tools/acp-cert.der /fs/shim/carplug/_tools/nbt_carplay --stop-after iap2 --fifo none --no-audio --no-mic" 300
  ```
  Expected: USB, pairing ("Trust?" on the iPhone), carkit, iAP2 identification. It stops at the signature: the emulator
  has no chip.
- **The car:** `car/car-chip-test.sh session`. It puts `nbt_carplay` in `/dev/shmem` and runs
  `--stop-after video --fifo none --no-audio --no-mic --dump /tmp/cp-session.h264 --pcap …`. Then it fetches the dump and the pcap
  and on the Mac saves a frame as a PNG (`cp-session-frame.png`). Nothing is written to flash; `cleanup` removes everything from RAM.

## "Factory-like" modes (the bridge, 2026-11-26)

- `--bridge`: the iPhone's NCM → the head unit's interface `tap0`, no session of its own.
- `--bridge-start`: plus IPv4 on tap0 (172.16.226.1/24) and the iPhone's DHCP reply (172.16.226.100), parsing the factory
  receiver's announcement (`_airplay._tcp`, port 7000) and `CarPlayStartSession` to it. In the car on 2026-11-26 factory CarPlay
  showed a picture, but without an entry in iDrive.
- `--bridge-ctrl BTADDR`: plus a "CarPlay controller" `_carplay-ctrl._tcp` in tap0 on behalf of the iPhone (TXT `id` = the iPhone's BT address,
  the address is the head unit itself, the server is `cp_ctrl.c`). `CarPlayStartSession` goes out after `GET /ctrl-int/1/connect` from the head unit
  (or after 30 s). On exit, a "goodbye" (TTL 0). Not yet run in the car.
- A failure (SIGSEGV and so on) ends the program without a dump: on this head unit a dump reboots it.
- Testing on the Mac: `test-host/bridge_replay.c`: frames from the car's recordings and 200 thousand corrupted ones through the bridge's parser under
  ASan/UBSan; `test-host/ctrl_server.c`: the controller server answers a request as the factory plugin sends it.
  The build is in the file headers (clang, `-fsanitize=address,undefined`).
