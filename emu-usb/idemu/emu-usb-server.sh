#!/bin/bash
# Start the IDEmu server with USB on and the Mac's iPhone passed through to the emulated head unit.
#
#   sudo emulators/emu-usb-server.sh          (Ctrl-C stops it)
#
# Why root: macOS gives the iPhone's USB interfaces to QEMU (libusb) only when QEMU runs as root
# (wired-carplay/emu-usb/README.md, test of 2026-11-26). IDEMU_USB=1 then adds the OMAP5 EHCI to the board
# (IDEmu/src/qemu/omap5-uevm.c), packs idemu/usb/start.sh on the shim so the guest starts the firmware's own io-usb,
# and puts a usb-host device for vendor 0x05ac on the EHCI (IDEMU_USB_HOST="vendorid=...,productid=..." picks another).
# While a unit runs, macOS does not see the iPhone; it gets it back when the unit stops.
# The server is the normal one (http://127.0.0.1:8765, the same API and SSH bridge). On exit the files it wrote are
# given back to the user who ran sudo, so later runs without sudo still work.
set -u
[ "$(id -u)" = 0 ] || { echo "Run it with sudo: sudo $0" >&2; exit 1; }
[ -n "${SUDO_UID:-}" ] || { echo "Start it through sudo (SUDO_UID is missing)" >&2; exit 1; }
HERE=$(cd "$(dirname "$0")" && pwd)
I=$HERE/IDEmu
give_back() {
    chown -R "$SUDO_UID:$SUDO_GID" "$I/environments" "$I/idemu" "$I/cache" "$I/mapcache" 2>/dev/null
    [ -f "$I/settings.json" ] && chown "$SUDO_UID:$SUDO_GID" "$I/settings.json"
    echo "emu-usb-server: files given back to uid $SUDO_UID"
}
trap give_back EXIT
cd "$I" || exit 1
export IDEMU_USB=1 PYTHONDONTWRITEBYTECODE=1
echo "emu-usb-server: IDEmu with USB on, iPhone passthrough ${IDEMU_USB_HOST:-vendorid=0x05ac}"
/usr/bin/python3 -m idemu gui --no-browser
