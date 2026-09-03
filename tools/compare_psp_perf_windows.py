#!/usr/bin/env python3
"""Cross-build A/B of PERF_ATTR V1 windows matched by stage and stage-frame span.

Unlike ``analyze_psp_perf_attr_log.py --compare`` (same build, clock A/B), this
accepts two different build ids and prints per-window, per-stage-tick
milliseconds for the main phase totals of both logs plus the delta (B - A).
Windows are matched on (stage, sf span, demo); unmatched windows are listed.
"""

from __future__ import annotations

import argparse
import pathlib
import sys

PREFIX = "PERF_ATTR V1 "
# field -> (label, index within a/b/c triple or None for scalar)
COLUMNS = (
    ("wall", "wall", None), ("calc", "calc", 0), ("drawf", "draw", 0), ("pres", "present", 0),
    ("vbs", "swapVB", None), ("vbc", "cadVB", None), ("ge", "geWait", None), ("wo", "unattr", None),
)


def parse(text: str) -> dict[tuple, dict]:
    windows = {}
    build = None
    for raw in text.splitlines():
        line = raw.strip()
        if line.startswith("BUILD id="):
            build = line[len("BUILD id="):].split()[0]
        if not line.startswith(PREFIX):
            continue
        fields = {}
        for token in line[len(PREFIX):].split():
            if "=" in token:
                k, v = token.split("=", 1)
                fields[k] = v
        first, last = (int(x) for x in fields["sf"].split("-"))
        key = (int(fields["st"]), first, last, int(fields.get("demo", -1)))
        record = {"ticks": max(1, last - first)}
        for name, _label, idx in COLUMNS:
            value = fields[name]
            record[name] = int(value.split("/")[idx] if idx is not None else value)
        record["busy"] = record["wall"] - record["wo"] - record["vbs"] - record["vbc"] - record["ge"]
        windows[key] = record
    return {"build": build, "windows": windows}


def render(a: dict, b: dict, stage: int | None) -> str:
    lines = [f"A = {a['build']}", f"B = {b['build']}",
             "per stage tick, ms (B-A in parentheses); busy = wall - unattributed - VBlank waits - GE wait"]
    header = f"{'window':<20s}" + "".join(f"{label:>22s}" for _n, label, _i in COLUMNS) + f"{'busy':>22s}"
    lines.append(header)
    keys = [k for k in a["windows"] if k in b["windows"] and (stage is None or k[0] == stage)]
    keys.sort()
    sums_a = {n: 0.0 for n, _l, _i in COLUMNS}; sums_a["busy"] = 0.0
    sums_b = dict(sums_a); ticks = 0
    for key in keys:
        ra, rb = a["windows"][key], b["windows"][key]
        cells = []
        for name in [n for n, _l, _i in COLUMNS] + ["busy"]:
            va = ra[name] / ra["ticks"] / 1000.0
            vb = rb[name] / rb["ticks"] / 1000.0
            cells.append(f"{va:8.3f}->{vb:6.3f}({vb - va:+6.3f})")
            sums_a[name] += ra[name]; sums_b[name] += rb[name]
        ticks += ra["ticks"]
        lines.append(f"st{key[0]} sf={key[1]}-{key[2]:<9d}" + "".join(f"{c:>22s}" for c in cells))
    if keys:
        cells = []
        for name in [n for n, _l, _i in COLUMNS] + ["busy"]:
            va = sums_a[name] / ticks / 1000.0; vb = sums_b[name] / ticks / 1000.0
            cells.append(f"{va:8.3f}->{vb:6.3f}({vb - va:+6.3f})")
        lines.append(f"{'mean of matched':<20s}" + "".join(f"{c:>22s}" for c in cells))
    missing_a = sorted(k for k in b["windows"] if k not in a["windows"])
    missing_b = sorted(k for k in a["windows"] if k not in b["windows"])
    lines.append(f"matched windows: {len(keys)}; only in A: {len(missing_b)}; only in B: {len(missing_a)}")
    return "\n".join(lines)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("log_a", type=pathlib.Path)
    parser.add_argument("log_b", type=pathlib.Path)
    parser.add_argument("--stage", type=int, default=None, help="restrict to one stage number")
    args = parser.parse_args(argv)
    a = parse(args.log_a.read_text(encoding="utf-8", errors="replace"))
    b = parse(args.log_b.read_text(encoding="utf-8", errors="replace"))
    print(render(a, b, args.stage))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
