# Contributing

Thanks for helping. This project runs code inside a car's head unit, next to the factory software that drives the
screen, the sound and the phone. A mistake can make the head unit restart, loop, or lose wireless CarPlay. The rules
below are there to prevent that, and to keep this repository legal to publish.

- [What fits](#what-fits)
- [Before you start](#before-you-start)
- [Head-unit facts you must know](#head-unit-facts-you-must-know)
- [Design rules](#design-rules)
- [Code rules](#code-rules)
- [Testing](#testing)
- [Never commit](#never-commit)
- [Reverse engineering](#reverse-engineering)
- [Commits and pull requests](#commits-and-pull-requests)
- [Reporting a result from a car](#reporting-a-result-from-a-car)
- [AI agents](#ai-agents)
- [Conduct](#conduct)
- [License of contributions](#license-of-contributions)

## What fits

Welcome:
- results from other head units, firmware versions, iPhones and iOS versions (see
  [Reporting a result from a car](#reporting-a-result-from-a-car)), including "it did not work";
- fixes, more host tests, emulator (IDEmu) coverage;
- ports of `conn_patch` to other firmware: new offsets and fingerprints, made the same way;
- the missing pieces listed in the README and the car results (for example the remaining iAP2 messages).

Not wanted:
- anything that makes wireless CarPlay, the factory iPod/USB behaviour or nbt-aa behave differently when the wired
  path is not in use;
- writes to the head unit outside what [README.md](README.md#what-it-changes-on-the-head-unit) lists, unless the
  pull request adds them to that table with a backup and an undo;
- code, binaries or data taken from BMW, Harman, Apple or QNX software (see [Never commit](#never-commit)).

## Before you start

- For anything bigger than a small fix, open an issue first and say what you want to change and how you will test it.
- Set up the build and tests as in [README.md](README.md#dependencies), and run `./scripts/test-host.sh` before
  changing anything, so you know your starting point is green.
- Read the README section of the part you touch (`nbt-carplay-evo/`, `iap2-adapter/`, `conn-patch/`,
  `car/autostart/`) and the research note it points to.

## Head-unit facts you must know

All of these were learned on a real NBT Evo (`NBTevo_N18032I`):

- **A core dump of any program restarts the head unit**, ours included, even one in `/dev/shmem`. Every program we run
  there must exit on fatal signals without a core (see the signal handling in `nbt_carplay.c`).
- **A crash inside `Connectivity` dumps core and so restarts the whole head unit.** The iAP2 adapter runs in that
  process. Keep the adapter tiny: no parsing, only passing frames on.
- `/tmp` is a 14 MB RAM disk. Filling it broke Siri. Keep pcaps and dumps off by default.
- `/var` is flash: write there only what must survive a restart, a few small writes per boot at most. Logs go to
  `/dev/shmem` (RAM), with a size limit.
- `/dev/shmem` cannot rename over an existing file: write in place, or remove first.
- A full `pidin fds` hangs, and the process can't be killed until the next restart. The head unit has no `awk`, `tr`,
  `head`, `netstat` or `tcpdump`.
- QNX 6.5 refuses writes to a running program's code through `/proc/<pid>/as` (EFAULT). `conn_patch` writes the
  physical page instead (`DCMD_PROC_PTINFO`, `mmap_device_memory`, `msync` with I-cache invalidation).
- The CPU does not fix up unaligned memory accesses: compile with `-mno-unaligned-access`.
- The factory iPod handlers respawn after each USB re-enumeration. The bridge has to deal with "Resource busy" for a
  few seconds.
- On macOS, link-local traffic (169.254/16) goes out over Wi-Fi unless you bind the ENET adapter: `ssh -B en7 …`.

## Design rules

1. **Factory first.** Prefer the head unit's own receiver, menu and drivers over doing the work ourselves.
2. **Wireless and everything else stay untouched.** With no iPhone on the cable, or with the iPhone's Wi-Fi on, the
   head unit must behave exactly as without this project.
3. **Fail closed.** If anything is unknown (another firmware, a byte that differs, an answer we did not expect), do
   nothing and log why. `conn_patch` and `gen_tab.py` show the pattern: whole-function fingerprints, then a write,
   then a read-back, with the factory bytes put back if the read-back differs.
4. **Every change on the head unit has a backup and an undo**, and is listed in the README table. The undo must work
   without the laptop where possible (kill switch files, boot-loop guards, crash limits).
5. **Flash writes are rare and small.** Never write on every session or every second.
6. **One stage per action** in `car/car-chip-test.sh`. Each stage says whether it is read-only, RAM or FLASH, checks
   SHA-256 after every copy, and has a `DRY=1` mode that writes nothing.

## Code rules

- C99 with GNU extensions (`-std=gnu99`), built with the GCC 4.9 toolchain (`qnx65-armv7-toolchain:4.9`), always with
  `-mno-unaligned-access`. No new warnings under `-Wall -Wextra`.
- Programs for the head unit may only import from `libc.so.3`, `libsocket.so.3` and `libusbdi.so.2` of QNX 6.5. Each
  `build.sh` checks this; keep the checks. No new runtime libraries on the head unit.
- New third-party code: fetched by `scripts/fetch-deps.sh` at a pinned commit (or a pinned archive with SHA-256),
  never copied into the tree, and with its license added to [NOTICE.md](NOTICE.md).
- Anything that parses data from the iPhone, the USB link, the network or the head unit's log must check every length.
  Add a host test under AddressSanitizer + UBSan with malformed input (see `test-host/bridge_replay.c`).
- Match the style of the file you edit. Comments and log lines are in English and say why, not only what.
- Log lines that can contain a Bluetooth address, a UDID or a device name must be redactable (`car-chip-test.sh`
  `redact_bt`).
- Keep `nbt-carplay-evo/` close to upstream where the change does not need otherwise, so upstream fixes can be merged.
  Name upstream changes you pull in, with their commit, in the commit message.

## Testing

A pull request needs:
1. `./scripts/test-host.sh` passing, plus a new host test for new behaviour.
2. `./scripts/build-all.sh` passing (the import checks are part of it). If a head-unit binary changed, give its new
   SHA-256 in the pull request.
3. For changes that depend on QNX (procfs, resource managers, process start, file systems), a run in IDEmu
   (`emu-usb/README.md`, `iap2-adapter/bench/`). Say what you ran and what you saw.
4. For changes that can only be proven in a car, say so plainly. The pull request then stays a draft until someone
   reports a car run, and the README status says what has and has not been seen on a car.

Never test on a car first. Never test on a head unit you cannot recover over SSH.

## Never commit

Pull requests that contain any of these are closed, and the history has to be rewritten:
- anything under `car/runs/`, pcaps, H.264 dumps, core files, or logs from a head unit or an iPhone that are not
  redacted;
- files pulled from a head unit or from firmware (`Connectivity`, libraries, configs such as `carplay.ini`, extracts
  of PSdZData or SWFL), and tables generated from them (`conn_patch_tab.h`);
- Apple MFi certificates or signatures, pairing records, AirPlay identities, private keys;
- Bluetooth or Wi-Fi MAC addresses, UDIDs, serial numbers, VINs, iPhone names, people's names, passwords, the head
  unit's SSH password;
- Apple SDK or MFi documentation, or code under a license that is not compatible with GPL-3.0-only.

`.gitignore` covers the usual places. Still check `git diff --cached` before every commit.

## Reverse engineering

Notes on how the head unit works are welcome and are a large part of this project. Write them as findings: function
names, addresses, what a branch decides, which config key does what, how you checked. Short mentions of one or two
instructions are fine. Do not paste disassembly listings, decompiled code, or files from the firmware. Say which
firmware build the addresses belong to.

## Commits and pull requests

- One topic per pull request, small enough to review. The message says why, not only what.
- Sign off every commit (`git commit -s`). This certifies the
  [Developer Certificate of Origin 1.1](https://developercertificate.org/): you wrote the change or have the right to
  submit it under this project's license.
- Update the docs in the same pull request: the README status and the "What it changes on the head unit" table if the
  head unit is affected, the component README, and a `car/RESULT-<date>.md` for a car visit.
- Everything in this repository is in English: documents, comments, log lines, commit messages.
- Fill in the pull request template: what changed, how it was tested (host / IDEmu / car), firmware, iPhone and iOS,
  and the new SHA-256 values.

## Reporting a result from a car

Open an issue with the "Car report" template. Useful even when nothing worked:
- head-unit firmware (`NBTevo_…`), car model and year, whether nbt-aa or other mods are installed;
- iPhone model and iOS version;
- the stages you ran and their summary lines (`car/runs/<date>/summary.txt`). **Redact them first**: Bluetooth
  addresses, iPhone name, UDID, VIN, the chip certificate. Never attach the whole `runs/` folder.
- what you saw on the screen, the cluster and the HUD, and what you heard;
- whether wireless CarPlay still behaves as before.

## AI agents

Agents may contribute, under the same rules. An agent working here follows
[README.md → Installing: for an agent](README.md#installing-for-an-agent) and this file. A person opens the pull
request and is responsible for it. Say in the pull request that an agent helped. An agent never types or stores a
head-unit password, and never runs a FLASH stage without the car owner's explicit yes for that stage.

## Conduct

Be kind and specific; assume good intent; review the change, not the person. Harassment of any kind is not accepted.
The [Contributor Covenant 2.1](https://www.contributor-covenant.org/version/2/1/code_of_conduct/) applies; report
problems to the maintainer through a GitHub issue or by private message on GitHub.

## License of contributions

Contributions are accepted under GPL-3.0-only, the license of this repository (inbound = outbound), together with
the additional permission in [NOTICE.md](NOTICE.md) for combining with the head unit's software.
