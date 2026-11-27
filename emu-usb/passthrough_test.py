#!/usr/bin/env python3
"""Can IDEmu's QEMU take a real iPhone from this Mac (usb-host over libusb)?

Starts IDEmu's qemu-system-arm on a bare `virt` machine (no guest, nothing booted) with an EHCI controller and a
usb-host device for the first Apple device (vendor 0x05ac) found by ioreg, waits a few seconds, asks QEMU over QMP
which host devices it sees (`info usbhost`) and what is attached to the emulated bus (`info usb`), prints QEMU's
own error output, and stops QEMU. The iPhone is held by QEMU only while it runs; macOS gets it back afterwards.

  python3 passthrough_test.py          as the normal user (macOS will most likely refuse: the iPhone is in use)
  sudo python3 passthrough_test.py     as root, which libusb needs to take a device away from macOS

Success: `info usb` lists a device with speed 480 (the iPhone). Failure: QEMU prints a libusb error
(ACCESS / BUSY / NOT_SUPPORTED) and `info usb` stays empty.
"""
import json, os, re, socket, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
QEMU = os.path.normpath(os.path.join(HERE, "../../emulators/IDEmu/bin/qemu-system-arm"))


def apple_devices():
    out = subprocess.run(["ioreg", "-p", "IOUSB", "-w0", "-l"], capture_output=True, text=True).stdout
    devs, cur = [], {}
    for line in out.splitlines():
        m = re.search(r'"(USB Product Name|idVendor|idProduct)" = (.+)$', line)
        if m:
            cur[m.group(1)] = m.group(2).strip().strip('"')
            if {"idVendor", "idProduct"} <= cur.keys():
                pid = int(cur["idProduct"])
                # iPhone / iPad product ids are 0x12xx; other Apple devices (keyboards, hubs) are left alone
                if cur["idVendor"] == "1452" and (pid & 0xff00) == 0x1200:
                    devs.append((pid, cur.get("USB Product Name", "?")))
                cur = {}
    return devs


def qmp(path, *commands):
    s = socket.socket(socket.AF_UNIX)
    for _ in range(50):
        try:
            s.connect(path)
            break
        except OSError:
            time.sleep(0.1)
    f = s.makefile("rw")
    f.readline()                                      # greeting
    f.write(json.dumps({"execute": "qmp_capabilities"}) + "\n"); f.flush(); f.readline()
    answers = []
    for c in commands:
        f.write(json.dumps({"execute": "human-monitor-command", "arguments": {"command-line": c}}) + "\n"); f.flush()
        while True:
            r = json.loads(f.readline())
            if "return" in r or "error" in r:
                answers.append((c, r.get("return", r.get("error"))))
                break
    try:
        f.write(json.dumps({"execute": "quit"}) + "\n"); f.flush()
    except OSError:
        pass
    return answers


def main():
    devs = apple_devices()
    if not devs:
        print("No Apple USB device on this Mac. Plug the iPhone in (unlocked) and run again.")
        return 2
    pid, name = devs[0]
    print("Apple device: %s, product id 0x%04x; running as %s" % (name, pid, "root" if os.geteuid() == 0 else "user"))
    tmp = tempfile.mkdtemp(prefix="qemu-usb-")
    sock = os.path.join(tmp, "qmp.sock")
    err = open(os.path.join(tmp, "stderr.txt"), "w+")
    cmd = [QEMU, "-M", "virt", "-cpu", "cortex-a15", "-m", "64", "-display", "none", "-nodefaults",
           "-device", "ich9-usb-ehci1,id=ehci",
           "-device", "usb-host,bus=ehci.0,vendorid=0x05ac,productid=0x%04x" % pid,
           "-qmp", "unix:%s,server=on,wait=off" % sock]
    q = subprocess.Popen(cmd, stdout=err, stderr=err)
    time.sleep(4)                                     # usb-host attaches from a timer once the device is found
    try:
        answers = qmp(sock, "info usbhost", "info usb")
    except Exception as e:                            # QEMU may have exited already
        answers = [("qmp", "failed: %s" % e)]
    try:
        q.wait(timeout=10)
    except subprocess.TimeoutExpired:
        q.kill()
    err.seek(0)
    for c, a in answers:
        print("== %s\n%s" % (c, a if isinstance(a, str) else json.dumps(a)))
    e = err.read().strip()
    print("== QEMU messages\n%s" % (e or "(none)"))
    attached = any(c == "info usb" and isinstance(a, str) and "Speed 480" in a for c, a in answers)
    held = e.count("libusb_detach_kernel_driver")     # interfaces macOS keeps; QEMU cannot claim them later
    if attached and not held:
        print("RESULT: PASSTHROUGH WORKS: the iPhone is on the emulated EHCI bus and no interface is held by macOS")
    elif attached:
        print("RESULT: PARTLY: the iPhone is on the emulated bus, but macOS keeps %d interface(s); a guest could not "
              "use them. Run again with sudo." % held)
    else:
        print("RESULT: not attached (see the messages above)")
    return 0 if attached and not held else 1


if __name__ == "__main__":
    sys.exit(main())
