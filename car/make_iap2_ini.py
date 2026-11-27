#!/usr/bin/env python3
"""make_iap2_ini.py <car's carplay.ini> <out> <new LibPath>    -> prints the LibPath it replaced
   make_iap2_ini.py --restore <carplay.ini> <out> <old LibPath> -> puts the old one back

Only the LibPath line of the [iAP2] section changes (the rest of the file, PCM lines included, stays byte for byte).
Fails (exit 1) if there is no [iAP2] LibPath, or (install) it already is the new one, or (restore) it is not ours.
"""
import sys

OURS = "nbtcp_iap2_adapter.so"


def edit(src, out, value, restore):
    data = open(src, "rb").read().decode("latin1")
    nl = "\r\n" if "\r\n" in data else "\n"
    lines = data.split(nl)
    sec, hit, old = None, None, None
    for i, l in enumerate(lines):
        s = l.strip()
        if s.startswith("[") and s.endswith("]"):
            sec = s
        elif sec == "[iAP2]" and s.startswith("LibPath="):
            hit, old = i, s[len("LibPath="):]
            break
    if hit is None:
        sys.exit("no [iAP2] LibPath")
    if not restore and old.endswith(OURS):
        sys.exit("LibPath is ours already: " + old)
    if restore and not old.endswith(OURS):
        sys.exit("LibPath is not ours (" + old + "): nothing to restore")
    lines[hit] = "LibPath=" + value
    open(out, "wb").write(nl.join(lines).encode("latin1"))
    print(old)


if sys.argv[1] == "--restore":
    edit(sys.argv[2], sys.argv[3], sys.argv[4], True)
else:
    edit(sys.argv[1], sys.argv[2], sys.argv[3], False)
