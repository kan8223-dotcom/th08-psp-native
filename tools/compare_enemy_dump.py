#!/usr/bin/env python3
"""Compare per-frame enemy state: port ENEMY_DUMP lines vs original dumps (items+effects+enemies regions)."""
import re, sys, struct, glob, os
ISTRIDE, NI = 0x2e4, 512
EFSTRIDE, NEF = 0x360, 512
ENSTRIDE, NEN = 0x53d0, 70  # original layout keeps the per-slot trail vertex array
E_POS, E_LIFE, E_IDX, E_BT_CUR, E_F1, E_F2, E_ATT, E_ECL = 0x2d34, 0x2dfc, 0x2e0c, 0x2e14 + 8, 0x3324, 0x3328, 0x53c0, 0x2ca0
def fl(u): return struct.unpack('<f', struct.pack('<I', u))[0]
def load_port(path):
    P = {}
    pat = re.compile(r'ENEMY_DUMP f=(\d+) i=(\d+) x=([0-9a-f]{8}) y=([0-9a-f]{8}) life=(-?\d+) f1=([0-9a-f]{8}) f2=([0-9a-f]{8}) att=(-?\d+) bt=(-?\d+) ecl=(\d)')
    for line in open(path, encoding='utf-8', errors='replace'):
        m = pat.search(line)
        if m:
            f, i = int(m.group(1)), int(m.group(2))
            if i >= NEN: continue
            P.setdefault(f, {})[i] = dict(x=round(fl(int(m.group(3), 16)), 2), y=round(fl(int(m.group(4), 16)), 2), life=int(m.group(5)), f1=int(m.group(6), 16), f2=int(m.group(7), 16), att=int(m.group(8)), bt=int(m.group(9)), ecl=int(m.group(10)))
    return P
def load_orig(d):
    O = {}
    base = NI * ISTRIDE + NEF * EFSTRIDE
    for fn in glob.glob(os.path.join(d, 'dump_0_*.bin')):
        raw = open(fn, 'rb').read()
        if len(raw) < base + 0x3e98: continue
        f = int(os.path.basename(fn)[7:-4]); ee = {}
        for i in range(NEN):
            o = base + i * ENSTRIDE
            if o + 0x3e98 > len(raw): break
            f1, = struct.unpack_from('<I', raw, o + E_F1)
            if (f1 & 1) == 0: continue
            ee[i] = dict(x=round(struct.unpack_from('<f', raw, o + E_POS)[0], 2), y=round(struct.unpack_from('<f', raw, o + E_POS + 4)[0], 2), life=struct.unpack_from('<i', raw, o + E_LIFE)[0], f1=f1, f2=struct.unpack_from('<I', raw, o + E_F2)[0], att=struct.unpack_from('<i', raw, o + E_ATT)[0], bt=struct.unpack_from('<i', raw, o + E_BT_CUR)[0], ecl=1 if struct.unpack_from('<I', raw, o + E_ECL)[0] else 0)
        O[f] = ee
    return O
def main():
    P, O = load_port(sys.argv[1]), load_orig(sys.argv[2])
    frames = sorted(set(P) & set(O))
    lo = int(sys.argv[3]) if len(sys.argv) > 3 else 0; hi = int(sys.argv[4]) if len(sys.argv) > 4 else 10**9
    print('common frames', len(frames), frames[:2], frames[-2:])
    for f in frames:
        if f < lo or f > hi: continue
        for i in sorted(set(P[f]) | set(O[f])):
            p, o = P[f].get(i), O[f].get(i)
            if p != o:
                diffs = [k for k in ('x','y','life','f1','f2','att','bt','ecl') if (p or {}).get(k) != (o or {}).get(k)]
                print(f'f={f} enemy {i}: diffs={diffs}\n   port={p}\n   orig={o}')
main()
