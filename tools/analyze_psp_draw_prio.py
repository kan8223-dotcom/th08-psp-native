#!/usr/bin/env python3
"""Per-window draw-chain CPU breakdown from DRAW_PRIO lines (ms per sampled frame)."""
import re, sys
FIELDS = ["chain", "effect_bg", "p6", "p7x", "p8", "p9", "p12", "p13", "p14", "p17", "p20"]
def parse(path):
    rows = []
    for line in open(path, encoding="utf-8", errors="replace"):
        if not line.startswith("DRAW_PRIO V1"):
            continue
        st = re.search(r" st=(-?\d+) sf=(\d+)-(\d+)", line)
        sampled = re.search(r" sampled=(\d+)", line)
        if not st or not sampled or int(sampled.group(1)) == 0:
            continue
        n = int(sampled.group(1)); row = {"st": int(st.group(1)), "sf": f"{st.group(2)}-{st.group(3)}", "n": n}
        for f in FIELDS:
            m = re.search(rf" {f}=(\d+)", line)
            row[f] = int(m.group(1)) / n / 1000.0 if m else 0.0
        rows.append(row)
    return rows
def show(path):
    rows = parse(path)
    print(f"== {path}")
    print("st sf            n  " + " ".join(f"{f:>9}" for f in FIELDS))
    for r in rows:
        print(f"{r['st']:>2} {r['sf']:<13} {r['n']:>2}  " + " ".join(f"{r[f]:>9.2f}" for f in FIELDS))
    if rows:
        print("   mean            " + " ".join(f"{sum(r[f] for r in rows)/len(rows):>9.2f}" for f in FIELDS))
for p in sys.argv[1:]:
    show(p)
