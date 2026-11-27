# Car: checking the MFi chip for wired CarPlay

One session, about 20-30 minutes. Nothing is written to the head unit's flash: the probe and its files live in `/dev/shmem` (RAM) and disappear
when the head unit restarts. Script: `car/car-chip-test.sh`.

## Bring

- A Mac with the repository, an ENET cable, an iPhone (charged, and you know the passcode).
- The head unit's root password. You type it yourself, once.

## At the car

1. Turn on the ignition (the engine can stay off) and wait for the head unit to boot. Connect the ENET cable to the OBD port and to the Mac.
2. **Do not plug the iPhone into USB, and turn Bluetooth off on the iPhone.** This keeps the factory wireless CarPlay from touching the chip.
3. Open SSH (it asks for the password):
   ```bash
   ssh root@169.254.199.119 true
   ```
4. Run everything in sequence:
   ```bash
   cd car && ./car-chip-test.sh all
   ```
   The script goes through these steps by itself:
   - `check` - read-only: processes, the chip node, the lock, USB, hashes of the factory files against 007_001_016;
   - `push` - puts the probe in RAM and verifies its SHA-256;
   - `acp-read` - the chip without a signature: access method, version, certificate;
   - `acp-sign` - one test signature.
   If the chip does not answer, the script stops by itself.
5. When the script asks: **you do not need to turn Bluetooth back on; plug the iPhone into USB, unlock it, and press Enter.**
6. The iPhone shows **"Trust This Computer?" -> Trust, then enter the passcode.**
7. At the end the script prints `summary: …/summary.txt`. That one line is enough for me. The logs are on the Mac in
   `car/runs/<date>/`.

You can also run the steps one at a time: `./car-chip-test.sh check | push | acp-read | acp-sign | iphone | cleanup`.
If SSH drops (ControlPersist 300 s), repeat step 3 and continue from the step you need.

## What to look for in the summary

| Line | Meaning |
|---|---|
| `way 0 (Harman send-and-receive devctl)` + `device version 0x03/0x05/0x07` | the chip can be read the factory way ✅ |
| `the chip is in use by the head unit` | the lock is held by factory CarPlay: check that Bluetooth is off on the iPhone, then repeat `acp-read` |
| `signature ready` / `acp: OK` | signing with the Evo chip works ✅ |
| `6 configurations` after `usb carplay` | the iPhone offered a CarPlay configuration ✅ |
| `lockdown: done` | the multiplexer and lockdown work on the real port ✅ |
| `every interface is held` | the factory iPod stack holds the interfaces - a bypass is needed, as in the full `nbt_carplay` |
| `carkit: connected` -> `iap2: … identification accepted, authentication succeeded` | **the iPhone accepted the Evo chip for CarPlay** - path B is open ✅ |
| `authentication not succeeded` | the iPhone did not accept the signature - we look at the log, possibly path A |

## If something goes wrong

- The head unit rebooted by itself: no problem, everything was in RAM. Tell me, and we will check `core.counter` in the `check` log.
- The iPhone is "stuck" in CarPlay mode: pull the cable out and it returns to normal.
- To remove the probe without restarting the head unit: `./car-chip-test.sh cleanup`.
