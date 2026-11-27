# xref.py <elf> <hex-value>...  : find ARM `ldr rX,[pc,#imm]` loading a pool word equal to value, and `bl` calls to value
import sys, struct
d = open(sys.argv[1], 'rb').read(); base = 0x100000
vals = [int(v, 16) for v in sys.argv[2:]]
pools = {v: set(i + base for i in range(0, len(d) - 3, 4) if struct.unpack_from('<I', d, i)[0] == v) for v in vals}
for i in range(0, min(len(d), 0x2e000), 4):
    w = struct.unpack_from('<I', d, i)[0]; pc = i + base
    if (w & 0x0f7f0000) == 0x051f0000:            # ldr rd, [pc, #+/-imm]
        imm = w & 0xfff; tgt = pc + 8 + (imm if w & 0x00800000 else -imm)
        for v in vals:
            if tgt in pools[v]: print('ldr  %#x -> pool %#x = %#x' % (pc, tgt, v))
    if (w & 0x0f000000) == 0x0b000000:            # bl
        off = w & 0xffffff; off = off - (1 << 24) if off & 0x800000 else off
        t = pc + 8 + off * 4
        for v in vals:
            if t == v: print('bl   %#x -> %#x' % (pc, v))
