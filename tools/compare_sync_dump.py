#!/usr/bin/env python3
"""Compare per-frame bullet/enemy dumps: port BootLog lines vs original memory dumps."""
import re, sys, struct, glob, os
BSTRIDE, BPOS, BVEL, BSPD, BANG, BSTATE = 0xb70, 0x7fc, 0x808, 0x820, 0x82c, 0x870
ESTRIDE, EPOS, EECL = 0x3e98, 0x2d34, 0x2ca0
NB, NE, NI = 256, 96, 256
ISTRIDE, ICUR, IVEL, ITGT, ITIMER, ITYPE, IINUSE, ISTATE, IONSCREEN = 0x2e4, 0x2a4, 0x2b0, 0x2bc, 0x2d0, 0x2d4, 0x2d5, 0x2d7, 0x2d6
def load_port(path):
    B, E, I = {}, {}, {}
    pi = re.compile(r'ITEM_DUMP f=(\d+) i=(\d+) ty=(-?\d+) st=(-?\d+) on=(-?\d+) x=([0-9a-f]{8}) y=([0-9a-f]{8}) vx=([0-9a-f]{8}) vy=([0-9a-f]{8}) tx=([0-9a-f]{8}) ty=([0-9a-f]{8}) t=(-?\d+)')
    pb = re.compile(r'BULLET_DUMP f=(\d+) i=(\d+) st=(\d+) x=([0-9a-f]{8}) y=([0-9a-f]{8}) vx=([0-9a-f]{8}) vy=([0-9a-f]{8}) sp=([0-9a-f]{8}) an=([0-9a-f]{8})')
    pe = re.compile(r'ENEMY_DUMP f=(\d+) i=(\d+) x=([0-9a-f]{8}) y=([0-9a-f]{8})')
    for line in open(path, encoding='utf-8', errors='replace'):
        m = pb.search(line)
        if m:
            f, i = int(m.group(1)), int(m.group(2))
            B.setdefault(f, {})[i] = (int(m.group(3)),) + tuple(int(m.group(k), 16) for k in range(4, 10))
            continue
        m = pe.search(line)
        if m:
            f, i = int(m.group(1)), int(m.group(2))
            E.setdefault(f, {})[i] = (int(m.group(3), 16), int(m.group(4), 16))
            continue
        m = pi.search(line)
        if m:
            f, i = int(m.group(1)), int(m.group(2))
            I.setdefault(f, {})[i] = (int(m.group(3)), int(m.group(4)), int(m.group(5))) + tuple(int(m.group(k), 16) for k in range(6, 12)) + (int(m.group(12)),)
    return B, E, I
def load_orig(d):
    B, E, I = {}, {}, {}
    for fn in glob.glob(os.path.join(d, 'dump_0_*.bin')):
        f = int(os.path.basename(fn)[7:-4])
        raw = open(fn, 'rb').read()
        bb, ee = {}, {}
        for i in range(NB if len(raw) > 512 * ISTRIDE else 0):
            o = i * BSTRIDE
            st = raw[o + BSTATE]
            if st == 0: continue
            x, y = struct.unpack_from('<II', raw, o + BPOS)
            vx, vy = struct.unpack_from('<II', raw, o + BVEL)
            sp, = struct.unpack_from('<I', raw, o + BSPD); an, = struct.unpack_from('<I', raw, o + BANG)
            bb[i] = (st, x, y, vx, vy, sp, an)
        base = NB * BSTRIDE
        for i in range(NE if len(raw) > 512 * ISTRIDE else 0):
            o = base + i * ESTRIDE
            ecl, = struct.unpack_from('<I', raw, o + EECL)
            if ecl == 0: continue
            x, y = struct.unpack_from('<II', raw, o + EPOS)
            ee[i] = (x, y)
        ii = {}
        base2 = NB * BSTRIDE + NE * ESTRIDE if len(raw) > 512 * ISTRIDE else 0
        for i in range(512 if base2 == 0 else NI):
            o = base2 + i * ISTRIDE
            if o + ISTRIDE > len(raw): break
            if raw[o + IINUSE] == 0: continue
            cx, cy = struct.unpack_from('<II', raw, o + ICUR); vx, vy = struct.unpack_from('<II', raw, o + IVEL); tx, ty = struct.unpack_from('<II', raw, o + ITGT)
            t, = struct.unpack_from('<i', raw, o + ITIMER)
            ii[i] = (struct.unpack_from('<b', raw, o + ITYPE)[0], struct.unpack_from('<b', raw, o + ISTATE)[0], struct.unpack_from('<b', raw, o + IONSCREEN)[0], cx, cy, vx, vy, tx, ty, t)
        B[f], E[f], I[f] = bb, ee, ii
    return B, E, I
def fl(u): return struct.unpack('<f', struct.pack('<I', u))[0]
def main():
    PB, PE, PI = load_port(sys.argv[1]); OB, OE, OI = load_orig(sys.argv[2])
    frames = sorted(set(PB) & set(OB))
    print('port frames', len(PB), 'orig frames', len(OB), 'common', len(frames), frames[:3], frames[-3:])
    names = ('state', 'x', 'y', 'vx', 'vy', 'speed', 'angle')
    for f in frames:
        pb, ob = PB[f], OB[f]
        if set(pb) != set(ob):
            print(f'f={f}: active bullet set differs: port-only={sorted(set(pb)-set(ob))} orig-only={sorted(set(ob)-set(pb))}')
        for i in sorted(set(pb) & set(ob)):
            d = [names[k] for k in range(7) if pb[i][k] != ob[i][k]]
            if d:
                print(f'f={f} bullet {i}: differs {d}')
                print('   port', [pb[i][0]] + ['%.6f' % fl(v) for v in pb[i][1:]])
                print('   orig', [ob[i][0]] + ['%.6f' % fl(v) for v in ob[i][1:]])
        pi_, oi = PI.get(f, {}), OI.get(f, {})
        if set(pi_) != set(oi):
            print(f'f={f}: active item set differs: port-only={sorted(set(pi_)-set(oi))} orig-only={sorted(set(oi)-set(pi_))}')
        inames = ('type', 'state', 'onscreen', 'x', 'y', 'vx', 'vy', 'tx', 'ty', 'timer')
        for i in sorted(set(pi_) & set(oi)):
            d = [inames[k] for k in range(10) if pi_[i][k] != oi[i][k]]
            if d:
                print(f'f={f} item {i}: differs {d}')
                print('   port', list(pi_[i][:3]) + ['%.6f' % fl(v) for v in pi_[i][3:9]] + [pi_[i][9]])
                print('   orig', list(oi[i][:3]) + ['%.6f' % fl(v) for v in oi[i][3:9]] + [oi[i][9]])
        pe, oe = PE.get(f, {}), OE.get(f, {})
        if set(pe) != set(oe):
            print(f'f={f}: active enemy set differs: port-only={sorted(set(pe)-set(oe))} orig-only={sorted(set(oe)-set(pe))}')
        for i in sorted(set(pe) & set(oe)):
            if pe[i] != oe[i]:
                print(f'f={f} enemy {i}: port=({fl(pe[i][0]):.6f},{fl(pe[i][1]):.6f}) orig=({fl(oe[i][0]):.6f},{fl(oe[i][1]):.6f})')
main()
