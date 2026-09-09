#!/usr/bin/env python3
"""Compare per-frame effect state: port EFFECT_DUMP lines vs original dumps (items region + effects region)."""
import re, sys, struct, glob, os
ISTRIDE, NI = 0x2e4, 512
ESTRIDE, NEF = 0x360, 512
E_ACTIVE, E_ID, E_TIMER, E_SCRIPT, E_ANM, E_POS, E_REL = 0x350, 0x351, 0x340, 0x21A, 0x216, 0x2a4, 0x352
def fl(u): return struct.unpack('<f', struct.pack('<I', u))[0]
def load_port(path):
    P = {}
    pat = re.compile(r'EFFECT_DUMP f=(\d+) i=(\d+) id=(-?\d+) t=(-?\d+) script=(-?\d+) anm=(-?\d+) rel=(-?\d+) x=([0-9a-f]{8}) y=([0-9a-f]{8})')
    for line in open(path, encoding='utf-8', errors='replace'):
        m = pat.search(line)
        if m:
            f, i = int(m.group(1)), int(m.group(2))
            P.setdefault(f, {})[i] = (int(m.group(3)), int(m.group(4)), int(m.group(5)), int(m.group(6)), int(m.group(7)), round(fl(int(m.group(8), 16)), 3), round(fl(int(m.group(9), 16)), 3))
    return P
def load_orig(d):
    O = {}
    for fn in glob.glob(os.path.join(d, 'dump_0_*.bin')):
        raw = open(fn, 'rb').read()
        if len(raw) < NI * ISTRIDE + NEF * ESTRIDE: continue
        f = int(os.path.basename(fn)[7:-4]); base = NI * ISTRIDE; ee = {}
        for i in range(NEF):
            o = base + i * ESTRIDE
            if raw[o + E_ACTIVE] == 0: continue
            ee[i] = (struct.unpack_from('<b', raw, o + E_ID)[0], struct.unpack_from('<i', raw, o + E_TIMER)[0], struct.unpack_from('<h', raw, o + E_SCRIPT)[0], struct.unpack_from('<h', raw, o + E_ANM)[0], struct.unpack_from('<b', raw, o + E_REL)[0], round(struct.unpack_from('<f', raw, o + E_POS)[0], 3), round(struct.unpack_from('<f', raw, o + E_POS + 4)[0], 3))
        O[f] = ee
    return O
def main():
    P, O = load_port(sys.argv[1]), load_orig(sys.argv[2])
    frames = sorted(set(P) & set(O))
    print('common frames', frames)
    from collections import Counter
    for f in frames:
        pc = Counter(v[0] for v in P[f].values()); oc = Counter(v[0] for v in O[f].values())
        if pc != oc:
            print(f'f={f}: effect id counts differ: port={dict(sorted(pc.items()))} orig={dict(sorted(oc.items()))}')
        else:
            print(f'f={f}: counts equal ({sum(pc.values())} active)')
        # slot-by-slot differences
        for i in sorted(set(P[f]) | set(O[f])):
            if P[f].get(i) != O[f].get(i):
                print(f'   slot {i}: port={P[f].get(i)} orig={O[f].get(i)}')
main()
