#!/usr/bin/env python3
"""Sum live heap bytes per owner from TH08PSP_MEMORY.LOG ALLOC/FREE/RESIZE events."""
import re, sys, collections
path = sys.argv[1]; upto_frame = int(sys.argv[2]) if len(sys.argv) > 2 else None
live = {}; owner_live = collections.Counter(); owner_peak = collections.Counter(); events = 0
for line in open(path, encoding="utf-8", errors="replace"):
    m = re.match(r"(ALLOC|FREE|RESIZE|FAIL) seq=(\d+) frame=(\d+) ptr=0x([0-9a-f]+) request=(\d+) usable=(\d+) owner=(.*)", line.rstrip("\n"))
    if not m: continue
    kind, _, frame, ptr, req, usable, owner = m.groups(); frame = int(frame); usable = int(usable); ptr = int(ptr, 16)
    if upto_frame is not None and frame > upto_frame: break
    events += 1
    if kind == "ALLOC" or kind == "RESIZE":
        if ptr in live:
            o, u = live.pop(ptr); owner_live[o] -= u
        live[ptr] = (owner, usable); owner_live[owner] += usable
        owner_peak[owner] = max(owner_peak[owner], owner_live[owner])
    elif kind == "FREE":
        if ptr in live:
            o, u = live.pop(ptr); owner_live[o] -= u
    elif kind == "FAIL":
        print(f"FAIL frame={frame} request={req} owner={owner}")
print(f"events={events} live_blocks={len(live)} live_bytes={sum(u for _, u in live.values())}")
print("top live owners:")
for o, b in owner_live.most_common(25):
    print(f"  {b:>10}  peak {owner_peak[o]:>10}  {o[:70]}")
