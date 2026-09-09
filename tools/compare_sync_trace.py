#!/usr/bin/env python3
"""Compare the port's SYNC_TRACE log lines with the original game's CSV trace (extended fields)."""
import re, sys, csv
FIELDS = ('seed', 'x', 'y', 'score', 'lives', 'gen', 'orbs', 'totorbs', 'piv', 'gauge', 'power', 'graze')
def load_port(path):
    d = {}
    pat = re.compile(r'SYNC_TRACE st=(-?\d+) f=(\d+) s=([0-9a-f]{4}) x=([0-9a-f]{8}) y=([0-9a-f]{8}) sc=(\d+) lv=(-?\d+)(?: g=(\d+))?(?: o=(-?\d+) to=(-?\d+) piv=(-?\d+) ga=(-?\d+) pw=(-?\d+) gr=(-?\d+))?')
    for line in open(path, encoding='utf-8', errors='replace'):
        m = pat.search(line)
        if not m: continue
        st, f = int(m.group(1)), int(m.group(2))
        vals = [m.group(3), m.group(4), m.group(5), int(m.group(6)), int(m.group(7))] + [int(m.group(k)) if m.group(k) is not None else None for k in range(8, 15)]
        d.setdefault(st, {})[f] = dict(zip(FIELDS, vals))
    return d
def load_orig(path):
    d = {}
    for row in csv.DictReader(open(path, encoding='utf-8', errors='replace')):
        st, f = int(row['st']), int(row['f'])
        vals = [row['seed'], row['x'], row['y'], int(row['score']), int(row['lives'])] + [int(row[k]) if k in row and row[k] not in (None, '') else None for k in FIELDS[5:]]
        if vals[10] is not None and vals[10] > 100000:
            import struct as _st
            vals[10] = int(_st.unpack('<f', _st.pack('<I', vals[10] & 0xffffffff))[0])
        d.setdefault(st, {})[f] = dict(zip(FIELDS, vals))
    return d
def main():
    port, orig = load_port(sys.argv[1]), load_orig(sys.argv[2])
    stage = int(sys.argv[3]) if len(sys.argv) > 3 else 0
    P, O = port.get(stage, {}), orig.get(stage, {})
    common = sorted(f for f in set(P) & set(O) if f >= 1)
    print(f'stage {stage}: port frames={len(P)} orig frames={len(O)} common={len(common)}')
    if not common: return
    # persistent seed divergence
    for f in common:
        rest = [g for g in common if g >= f][:30]
        if len(rest) >= 30 and all(P[g]['seed'] != O[g]['seed'] for g in rest):
            print(f'PERSISTENT SEED DIVERGENCE from frame {f}')
            for g in [x for x in common if f - 6 <= x <= f + 3]:
                dg = P[g]['gen'] - P[g - 1]['gen'] if g - 1 in P and P[g]['gen'] is not None and P[g-1]['gen'] is not None else None
                do = O[g]['gen'] - O[g - 1]['gen'] if g - 1 in O and O[g]['gen'] is not None and O[g-1]['gen'] is not None else None
                diffs = [k for k in FIELDS if P[g][k] != O[g][k]]
                print(f'  f={g} dgen port={dg} orig={do} diffs={diffs}')
                print(f'     port={P[g]}')
                print(f'     orig={O[g]}')
            break
    else:
        print(f'no persistent seed divergence over frames {common[0]}..{common[-1]}')
    for k in FIELDS[3:]:
        bad = [f for f in common if P[f][k] is not None and O[f][k] is not None and P[f][k] != O[f][k]]
        if bad:
            run = [f for f in bad if all((g in P and g in O and P[g][k] != O[g][k]) for g in range(f, f + 5))]
            print(f'{k}: {len(bad)} mismatching frames, first={bad[:4]}, first persistent={run[:1]}')
main()
