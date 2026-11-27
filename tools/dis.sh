#!/bin/sh
# dis.sh <file relative to the firmware extract> <start> <stop>  -- GNU objdump from the QNX 6.5 toolchain image
# FW_EXTRACT: folder with the extracted firmware files (default: ../../emulators/fw-extract/carplay beside this repo)
cd "${FW_EXTRACT:-$(dirname "$0")/../../emulators/fw-extract/carplay}" || exit 1
docker run --rm --platform linux/amd64 -v "$PWD":/w:ro qnx65-armv7-toolchain:4.9 /opt/qnx650/host/linux/x86/usr/bin/ntoarmv7-objdump -d --start-address=$2 --stop-address=$3 "/w/$1" 2>&1 | grep -v -E "^$|file format|^Disassembly"
