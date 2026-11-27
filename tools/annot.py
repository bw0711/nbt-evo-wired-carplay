#!/usr/bin/env python3
"""annot.py <elf> <start-hex> <stop-hex> [--calls]

ARM (A32) disassembly of a QNX shared object or executable with the names a plain objdump leaves out:
  - calls through the PLT get the imported symbol (from .rel.plt / R_ARM_JUMP_SLOT),
  - calls into the file get the exported name (from .dynsym), if any,
  - PIC string loads (`ldr rG,[pc,#x]; add rG,pc,rG` = GOT base, then `ldr rY,[pc,#y]; add rX,rG,rY`) and plain
    `ldr rX,[pc,#x]; add rX,pc,rX` get the string they point to.
--calls prints only lines with a call or a resolved string (a quick map of a long function).
Needs capstone (pip install capstone; run with python3 -I).
"""
import struct, sys
import capstone

def u32(b, o): return struct.unpack_from("<I", b, o)[0]

class Elf:
    def __init__(self, path):
        self.d = d = open(path, "rb").read()
        shoff = u32(d, 0x20); shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 0x2e)
        self.sh = []
        for i in range(shnum):
            name, typ, flags, addr, off, size, link, info, align, entsize = struct.unpack_from("<10I", d, shoff + i * shentsize)
            self.sh.append(dict(name=name, type=typ, addr=addr, off=off, size=size, link=link, entsize=entsize))
        strtab = self.sh[shstrndx]
        for s in self.sh:
            e = d.index(b"\0", strtab["off"] + s["name"])
            s["n"] = d[strtab["off"] + s["name"]:e].decode()
        self.by = {s["n"]: s for s in self.sh}
        # dynamic symbols
        self.dsyms = []
        ds, dstr = self.by.get(".dynsym"), self.by.get(".dynstr")
        if ds:
            for i in range(ds["size"] // 16):
                nm, val, sz, info, oth, shn = struct.unpack_from("<IIIBBH", d, ds["off"] + 16 * i)
                e = d.index(b"\0", dstr["off"] + nm)
                self.dsyms.append((d[dstr["off"] + nm:e].decode(), val, sz, shn))
        self.addr2name = {v: n for n, v, sz, shn in self.dsyms if v and shn}
        # PLT: GOT slot -> symbol
        slot2sym = {}
        rp = self.by.get(".rel.plt")
        if rp:
            for i in range(rp["size"] // 8):
                off, info = struct.unpack_from("<II", d, rp["off"] + 8 * i)
                slot2sym[off] = self.dsyms[info >> 8][0]
        self.plt = {}
        p = self.by.get(".plt")
        if p:
            md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
            ins = list(md.disasm(d[p["off"]:p["off"] + p["size"]], p["addr"]))
            for k in range(len(ins) - 2):
                a, b, c = ins[k], ins[k + 1], ins[k + 2]
                if a.mnemonic == "add" and a.op_str.startswith("ip, pc") and b.mnemonic == "add" and c.mnemonic == "ldr":
                    try:
                        v = a.address + 8 + int(a.op_str.split("#")[1], 0)
                        v += int(b.op_str.split("#")[1], 0)
                        v += int(c.op_str.split("#")[1].rstrip("]!"), 0)
                    except (IndexError, ValueError):
                        continue
                    if v in slot2sym:
                        self.plt[a.address] = slot2sym[v]

    def off(self, va):
        for s in self.sh:
            if s["type"] != 8 and s["addr"] and s["addr"] <= va < s["addr"] + s["size"]:
                return s["off"] + va - s["addr"]
        return None

    def cstr(self, va):
        o = self.off(va)
        if o is None:
            return None
        e = self.d.find(b"\0", o, o + 300)
        if e < 0:
            return None
        s = self.d[o:e]
        if len(s) < 2 or any(c < 9 or (13 < c < 32) or c > 126 for c in s):
            return None
        return s.decode()

    def word(self, va):
        o = self.off(va)
        return u32(self.d, o) if o is not None else None


def main():
    e = Elf(sys.argv[1])
    a, b = int(sys.argv[2], 16), int(sys.argv[3], 16)
    only = "--calls" in sys.argv
    md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
    md.skipdata = True            # literal pools sit inside functions; do not stop at them
    o = e.off(a)
    lits = {}     # reg -> literal value loaded from the pool
    got = {}      # reg -> GOT base
    for arg in sys.argv[4:]:      # --got=r4: the register already holds the GOT base (set before the range)
        if arg.startswith("--got="):
            got[arg[6:]] = e.by[".got"]["addr"]
    for i in md.disasm(e.d[o:o + (b - a)], a):
        note = ""
        ops = [x.strip() for x in i.op_str.split(",")]
        if i.mnemonic.startswith("ldr") and "[pc, #" in i.op_str:
            w = e.word(i.address + 8 + int(i.op_str.split("#")[1].rstrip("]"), 0))
            if w is not None:
                lits[ops[0]] = w
                if i.mnemonic != "ldr" or w > 0xffff:
                    note = "=0x%x%s" % (w, " '%s'" % struct.pack(">I", w).decode("latin1") if all(32 <= c < 127 for c in struct.pack(">I", w)) else "")
        elif i.mnemonic == "add" and len(ops) == 3 and ops[1] == "pc" and ops[2] in lits:
            v = (i.address + 8 + lits[ops[2]]) & 0xffffffff
            s = e.cstr(v)
            if s:
                note = '"%s"' % s
            else:
                got[ops[0]] = v
        elif i.mnemonic == "add" and len(ops) == 3 and ((ops[1] in got and ops[2] in lits) or (ops[2] in got and ops[1] in lits)):
            g, l = (ops[1], ops[2]) if ops[1] in got else (ops[2], ops[1])
            v = (got[g] + lits[l]) & 0xffffffff
            s = e.cstr(v)
            note = '"%s"' % s if s else "-> 0x%x" % v
        elif i.mnemonic in ("bl", "blx", "b") and i.op_str.startswith("#"):
            t = int(i.op_str[1:], 0)
            if t in e.plt:
                note = e.plt[t] + "()"
            elif t in e.addr2name:
                note = e.addr2name[t] + "()"
        if only and not note:
            continue
        print("%x: %-8s %-30s %s" % (i.address, i.mnemonic, i.op_str, note))

main()
