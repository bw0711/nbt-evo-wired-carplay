# Notices

## This project

nbt-evo-wired-carplay, Copyright (C) 2026 the nbt-evo-wired-carplay contributors.

All files of this repository are licensed under the GNU General Public License version 3 only
(`SPDX-License-Identifier: GPL-3.0-only`, full text in [LICENSE](LICENSE)), unless a file or a folder's notice says
otherwise.

    This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public
    License as published by the Free Software Foundation, version 3.

    This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied
    warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
    details.

Additional permission under GNU GPL version 3 section 7, for the code written in this project: if you modify this
program, or any covered work, by linking or combining it with the software of the BMW NBT Evo head unit (its operating
system, its programs such as `Connectivity`, and the libraries they load), containing parts covered by the terms of
their own licenses, the licensors of this program grant you additional permission to convey the resulting work.
Corresponding Source for a non-source form of such a combination shall not include the source code for the parts of
the head unit's software. (The iAP2 adapter is loaded into the head unit's `Connectivity`; `conn_patch` changes
`Connectivity`'s code in memory.)

## nbt_carplay by Valentin Radu

`nbt-carplay-evo/` (the program `nbt_carplay`) and `probe-evo/` (the probe) are modified versions of `nbt_carplay`
and `nbt_carplay_probe`, Copyright (C) 2026 Valentin Radu <valentin.radu@valinet.ro>, licensed under GPL-3.0-only,
as published in JayJay3071/nbt-android-auto (<https://github.com/JayJay3071/nbt-android-auto>, folder
`src/carplay/`, commit `78b098e1cffc3da5324a2b73870776c87850a5b4`). The upstream notices, including the authors'
additional permissions, are kept unchanged in [nbt-carplay-evo/NOTICE-upstream.md](nbt-carplay-evo/NOTICE-upstream.md)
and [probe-evo/NOTICE-upstream.md](probe-evo/NOTICE-upstream.md). The changes made here are listed in
[README.md](README.md#how-it-works) and in each folder's README; `git log` has every change since this repository
started.

Valentin Radu's additional permission for binary releases under the PolyForm Noncommercial License (quoted in the
upstream notice) was granted to JayJay3071 for nbt-android-auto only. It does not extend to this project: binaries
built from this repository are under GPL-3.0-only, and whoever conveys them must offer the corresponding source.

Required Notice: Copyright (c) 2026 Valentin Radu <valentin.radu@valinet.ro>

## Third-party code fetched at build time (not in this repository)

- **Mbed TLS 3.6.7** (<https://github.com/Mbed-TLS/mbedtls>), Copyright The Mbed TLS Contributors, Apache License 2.0.
  Downloaded by `probe-evo/build.sh` and patched with upstream nbt-android-auto's `mbedtls_legacy_time.py` and
  `mbedtls_empty_name.py`; statically linked into `nbt_carplay` and the probe.
- **lwIP 2.2.1** (<https://savannah.nongnu.org/projects/lwip>), Copyright (c) 2001, 2002 Swedish Institute of Computer
  Science, BSD-3-Clause. Statically linked into `nbt_carplay`.
- **Monocypher 4.0.2** (<https://monocypher.org>), CC0-1.0 or BSD-2-Clause. Statically linked into `nbt_carplay`.

Their full license texts come with their sources (`scripts/fetch-deps.sh`); whoever distributes binaries must ship
them along.

## Not included

No software of BMW, Harman, Apple, NVIDIA or QNX is included in this repository. The patch table of `conn_patch` holds
head-unit code, so it is made on the builder's machine from copies of their own head unit (`car/car-chip-test.sh
conn-pull`, `conn-patch/gen_tab.py`) and is never committed. The research notes describe the head unit's software by
names, addresses and behaviour; they contain no listings of it.

## Trademarks

BMW and iDrive are trademarks of BMW AG. Apple, iPhone, CarPlay, AirPlay and Siri are trademarks of Apple Inc. This
project is not affiliated with or endorsed by BMW AG, Harman or Apple Inc.
