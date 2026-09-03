#!/usr/bin/env python3
"""Summarize TH08 PSP SOFTFLOAT V1 records (soft-float census).

Each record counts, per parent PERF_ATTR window, the libgcc binary64 helper
calls issued directly by game/port code (``df_direct``), the ones issued from
inside libm (``df_internal``), and the libm binary64/binary32 entry calls,
attributed to the innermost active PERF_ATTR phase (``ph<N>``), plus a global
operation-class breakdown.  Counts are exact; the cycle estimate uses a rough
Allegrex cost model and is for ranking only.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import sys

RECORD_PREFIX = "SOFTFLOAT V1 "
PHASES = {
    0: "CalcChain", 1: "DrawFrame", 2: "DrawChain", 3: "PlayerUpdate", 4: "PlayerDraw",
    5: "EnemyUpdate", 6: "EnemyDraw", 7: "EffectUpdate", 8: "EffectDrawMain",
    9: "EffectDrawBullet", 10: "EffectDrawBackground", 11: "BulletUpdateInclusive",
    12: "BulletDrawInclusive", 13: "ItemUpdate", 14: "ItemDraw", 15: "PresentOuter",
    16: "PresentPreSwap", 17: "PresentSwap", 18: "PresentPostSwap", 19: "GeWaitSwap",
    20: "VblankWaitSwap", 21: "VblankWaitCadence", 22: "(no scope)",
}
OPS = ("add", "sub", "mul", "div", "cmp", "ext", "trunc", "fix", "flt", "sin", "cos", "atan2",
       "atan", "sqrt", "floor", "fmod", "other", "sinf", "cosf", "atan2f", "sqrtf", "floorf", "fmodf")
# Rough Allegrex cycle costs (newlib soft-float / libm), for ranking only.
CYCLE_MODEL = {"add": 90, "sub": 90, "mul": 110, "div": 300, "cmp": 40, "ext": 30, "trunc": 45,
               "fix": 40, "flt": 40, "sin": 5000, "cos": 5000, "atan2": 9000, "atan": 6000,
               "sqrt": 1500, "floor": 200, "fmod": 1500, "other": 6000, "sinf": 700, "cosf": 700,
               "atan2f": 1200, "sqrtf": 60, "floorf": 40, "fmodf": 300}


def parse_fields(body: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    for token in body.split():
        if "=" in token:
            key, value = token.split("=", 1)
            fields[key] = value
    return fields


def summarize(text: str, ticks: int = 600) -> list[dict]:
    out = []
    for number, raw in enumerate(text.splitlines(), start=1):
        line = raw.strip()
        if not line.startswith(RECORD_PREFIX):
            continue
        f = parse_fields(line[len(RECORD_PREFIX):])
        phases = {}
        for idx, name in PHASES.items():
            key = f"ph{idx}"
            if key not in f:
                continue
            parts = f[key].split("/")
            if len(parts) != 3:
                raise ValueError(f"line {number}: {key} is not df/libm/libmf")
            phases[name] = tuple(int(v) for v in parts)
        ops = {name: int(f.get(name, "0")) for name in OPS}
        est_cycles = sum(ops[name] * CYCLE_MODEL[name] for name in OPS)
        out.append({
            "line": number,
            "stage": int(f["st"]),
            "sf": f["sf"],
            "ticks": ticks,
            "df_direct": int(f["df_direct"]),
            "df_internal": int(f["df_internal"]),
            "libm": int(f["libm"]),
            "libmf": int(f["libmf"]),
            "phases": phases,
            "ops": ops,
            "est_cycles": est_cycles,
            "est_ms_per_tick_at_333": est_cycles / 333e6 * 1000.0 / ticks,
        })
    return out


def render(summaries: list[dict]) -> str:
    if not summaries:
        return "no SOFTFLOAT V1 records found\n"
    lines = ["window        df_direct/tick df_internal/tick libm/tick libmf/tick  est ms/tick@333  top phases by direct binary64 helper calls per tick (df/libm/libmf)"]
    tot_ops = {name: 0 for name in OPS}
    tot_ph: dict[str, list[int]] = {}
    for s in summaries:
        t = s["ticks"]
        ranked = sorted(s["phases"].items(), key=lambda kv: -(kv[1][0] + kv[1][1] * 40 + kv[1][2] * 6))[:4]
        top = ", ".join(f"{n} {v[0]/t:.0f}/{v[1]/t:.1f}/{v[2]/t:.1f}" for n, v in ranked if sum(v))
        lines.append(f"st{s['stage']} sf={s['sf']:<11} {s['df_direct']/t:12.0f} {s['df_internal']/t:14.0f} {s['libm']/t:9.1f} {s['libmf']/t:9.1f}   {s['est_ms_per_tick_at_333']:8.3f}   {top}")
        for name in OPS:
            tot_ops[name] += s["ops"][name]
        for n, v in s["phases"].items():
            acc = tot_ph.setdefault(n, [0, 0, 0])
            for i in range(3):
                acc[i] += v[i]
    total_ticks = sum(s["ticks"] for s in summaries)
    lines.append("")
    lines.append("all windows, per tick: " + " ".join(f"{name}={tot_ops[name]/total_ticks:.1f}" for name in OPS if tot_ops[name]))
    lines.append("all windows, per tick by phase (df_direct/libm/libmf): " + "; ".join(
        f"{n}={v[0]/total_ticks:.0f}/{v[1]/total_ticks:.1f}/{v[2]/total_ticks:.1f}" for n, v in sorted(tot_ph.items(), key=lambda kv: -sum(kv[1])) if sum(v)))
    est = sum(s["est_cycles"] for s in summaries) / 333e6 * 1000.0 / total_ticks
    lines.append(f"estimated soft-float cost at 333 MHz: {est:.3f} ms/tick (cycle model is approximate; use for ranking only)")
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("log", type=pathlib.Path)
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--ticks", type=int, default=600)
    args = parser.parse_args(argv)
    summaries = summarize(args.log.read_text(encoding="utf-8", errors="replace"), args.ticks)
    if args.json:
        json.dump(summaries, sys.stdout, indent=2); sys.stdout.write("\n")
    else:
        sys.stdout.write(render(summaries))
    return 0 if summaries else 1


if __name__ == "__main__":
    raise SystemExit(main())
