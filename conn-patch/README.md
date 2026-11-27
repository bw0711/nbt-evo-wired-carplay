# conn_patch: c1/c2 patches in the memory of the running Connectivity

What the patches are and why: `../docs/research/factory-like-device-list-re-2026-11-26.md`, section 4c. In short:
- **c1** (0x603e70, 4 bytes): a session started by the iPhone becomes "active" in any state of Connectivity,
  not only when it is itself waiting for a phone.
- **c2** (0x608d4c, 12 bytes): the found CarPlay "controller" is remembered as the phone's address, instead of being compared with the
  Bluetooth address. Without Bluetooth the factory connection chain would not start.
- **w1** (`libconn_carplay.so` 0x294a8, 4 bytes): the CarPlay module starts the factory iAP2 driver (`mm-ipod`) also for a
  cable session, not only for Wi-Fi/WFD (`../docs/research/iap2-factory-re-2026-11-27.md`, `../iap2-adapter`). The patch is in the
  library: the base is taken from procfs (the mapping address − its address inside the library by `DCMD_PROC_MAPDEBUG`;
  IDEmu 2026-11-27: MAPDEBUG returns the ELF address, not the base — found on the bench). On the Mac: `--lib <copy of the library>`.

## How it works
- The patch table `out/conn_patch_tab.h` is no longer kept in the repository: `build.sh` generates it with `gen_tab.py` from copies
  pulled from the builder's own head unit with the read-only stage `car/car-chip-test.sh conn-pull`. For each patch the table holds
  the whole function around it. `gen_tab.py` takes a patch only if its function's SHA-256 matches the firmware the patches were made
  for; otherwise it leaves that patch out with a warning. `build.sh` checks that the table matches what the script gives.
- It writes only if the function matches byte for byte (the patch place holds either the factory bytes or the new ones). If even one
  selected patch is in an unknown state, it writes nothing. After writing it reads back; if it does not match, it returns the factory bytes.
- On the head unit: it reads the process through `/proc/<pid>/as`. QNX 6.5 does not allow writing to code there (EFAULT, even with the process
  stopped — checked in IDEmu). So it writes to the physical page, page by page of the process's code: the address comes from
  `DCMD_PROC_PTINFO`, the whole page (4 KiB) is first compared with what the process sees, then the write, `msync`
  with a D-cache flush and an I-cache invalidate. At the moment of the write the process is stopped (with "continue on close").
- The process is found through `/proc`: a path that ends with `/conn/bin/Connectivity` matches (QNX gives the path without the leading `/`,
  on the car it may be the full path to the partition; `pidin` shows only the tail). The code check decides anyway.
- The file on the flash does not change: the code page is the process's memory. While the process is patched, its new instance would have seen the same
  page; when all instances have finished, a new start gets the factory code (checked in IDEmu).

## Checked (the library patch)
- IDEmu: a fake process with `selftest/libpt.so`, patch `t2` in the library: the base was found (as in `pidin mem`), the reply 1→2→1,
  also together with `t1`; the process is alive.

## Checked
- Mac, ASan/UBSan, a copy of Connectivity from the car: check/apply/revert, a repeated apply, rollback to the byte, a foreign byte
  in the function → nothing written, a half-patched place → UNKNOWN.
- IDEmu (QNX 6.5, 24-03), the fake process `selftest/patch_target` and `out/conn_patch_selftest`: the live process
  changed its reply 1→2 and back, 5 cycles in a row, the process keeps working; the search for Connectivity through `/proc` (there is none
  in the emulator → refusal); a foreign pid → refusal. QNX gives the program path without the leading `/` — taken into account.

## Not checked
- Connectivity itself is not run in the emulator, so c1/c2 are not checked on a live Connectivity.
- Cache: QEMU does not model caches. On the car (two Cortex-A15 cores) the I-cache invalidate depends on `msync(MS_INVALIDATE_ICACHE)`.
- Large pages: if the kernel mapped the Connectivity code with pages larger than 4 KiB, the utility takes this into account by the `size`
  from PTINFO, but it was not checked on such a case.

## Build and run
- `./build.sh` (Docker image `qnx65-armv7-toolchain:4.9`) → `out/conn_patch`, `out/conn_patch_selftest`, `out/patch_target`.
- On the car: `../car/car-chip-test.sh conn-check | conn-apply c1 [c2] | conn-revert c1 [c2]`.
- By hand: `conn_patch check`, `conn_patch apply c1 c2`, `conn_patch revert c1 c2`; `--file <copy>` — on the Mac.
