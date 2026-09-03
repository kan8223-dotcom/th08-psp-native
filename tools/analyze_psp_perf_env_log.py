#!/usr/bin/env python3
"""Summarize TH08 PSP PERF_ENV V1 records (execution environment per window).

For each parent PERF_ATTR window: effective CPU/bus clock at window start and
end, a fresh 1024-read timer calibration (compare with the boot ``cal_us`` in
PERF_ATTR_CONFIG), the main thread's kernel run clocks over the window (run/wall
= share of the wall the main thread actually executed; the rest went to other
threads such as audio), preemption counts, and the sceGeListSync waits reached
outside the swap context (PSPGL display-list rollover inside calc/draw
chains), attributed to the innermost PERF_ATTR phase.

Later records add ``mo4..mo7`` (outer loop pump, gated Render calls, the
post-present segment, the render-cadence tick) and ``gq`` (GE queue observer:
list submissions, certain-busy and unknown intervals between submissions, the
swap's GE wait and the first-submission-to-swap span per frame).  GE busy per
frame lies in [busy + swap wait, span].
"""

from __future__ import annotations

import argparse
import pathlib
import sys

RECORD_PREFIX = "PERF_ENV V1 "
CONFIG_PREFIX = "PERF_ATTR_CONFIG V1 "
PHASES = {
    0: "CalcChain", 1: "DrawFrame", 2: "DrawChain", 3: "PlayerUpdate", 4: "PlayerDraw",
    5: "EnemyUpdate", 6: "EnemyDraw", 7: "EffectUpdate", 8: "EffectDrawMain",
    9: "EffectDrawBullet", 10: "EffectDrawBackground", 11: "BulletUpdateInclusive",
    12: "BulletDrawInclusive", 13: "ItemUpdate", 14: "ItemDraw", 15: "PresentOuter",
    16: "PresentPreSwap", 17: "PresentSwap", 18: "PresentPostSwap", 19: "GeWaitSwap",
    20: "VblankWaitSwap", 21: "VblankWaitCadence", 22: "(no scope)",
}


def parse_fields(body: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    for token in body.split():
        if "=" in token:
            key, value = token.split("=", 1)
            fields[key] = value
    return fields


def summarize(text: str) -> tuple[int | None, list[dict]]:
    boot_cal = None
    out = []
    for number, raw in enumerate(text.splitlines(), start=1):
        line = raw.strip()
        if line.startswith(CONFIG_PREFIX):
            cfg = parse_fields(line[len(CONFIG_PREFIX):])
            if "cal_us" in cfg:
                boot_cal = int(cfg["cal_us"])
            continue
        if not line.startswith(RECORD_PREFIX):
            continue
        f = parse_fields(line[len(RECORD_PREFIX):])
        try:
            first, last = (int(x) for x in f["sf"].split("-"))
            record = {
                "line": number, "stage": int(f["st"]), "sf_first": first, "sf_last": last,
                "wall_us": int(f["wall"]),
                "clk0": tuple(int(x) for x in f["clk0"].split("/")),
                "clk1": tuple(int(x) for x in f["clk1"].split("/")),
                "recal_us": int(f["recal_us"]), "run_valid": int(f["run_valid"]) == 1,
                "run_us": int(f["run_us"]), "intr_preempt": int(f["intr_preempt"]),
                "thread_preempt": int(f["thread_preempt"]),
                "gw": tuple(int(x) for x in f["gw"].split("/")),
                "fw": tuple(int(x) for x in f["fw"].split("/")) if "fw" in f else (0, 0, 0),
                "mo": [tuple(int(x) for x in f[f"mo{i}"].split("/")) if f"mo{i}" in f else (0, 0, 0) for i in range(8)],
                "gq": tuple(int(x) for x in f["gq"].split("/")) if "gq" in f else (0,) * 8,
                "phases": {},
            }
            for idx, name in PHASES.items():
                total, mx, calls = (int(x) for x in f[f"gw{idx}"].split("/"))
                record["phases"][name] = (total, mx, calls)
        except (KeyError, ValueError) as error:
            raise ValueError(f"line {number}: malformed PERF_ENV record ({error})") from error
        ticks = max(1, last - first)
        record["ticks"] = ticks
        record["run_share"] = record["run_us"] / record["wall_us"] if record["wall_us"] and record["run_valid"] else None
        out.append(record)
    return boot_cal, out


def render(boot_cal: int | None, records: list[dict]) -> str:
    lines = [f"boot cal_us={boot_cal if boot_cal is not None else '?'} (1024 timer reads at boot)"]
    lines.append("window   stage  sf            clk0      clk1      recal_us  run/wall  intr_pre  thr_pre   gewait ms/tick (waits, max ms)  top phases")
    for i, r in enumerate(records, start=1):
        share = f"{r['run_share']:.3f}" if r["run_share"] is not None else "n/a"
        top = sorted(((n, v) for n, v in r["phases"].items() if v[2]), key=lambda kv: kv[1][0], reverse=True)[:3]
        top_text = ", ".join(f"{n} {v[0] / r['ticks'] / 1000.0:.3f}ms/{v[2]}" for n, v in top) or "-"
        lines.append(f"{i:<8d} {r['stage']:<6d} {r['sf_first']}-{r['sf_last']:<8d} "
                     f"{r['clk0'][0]}/{r['clk0'][1]:<5d} {r['clk1'][0]}/{r['clk1'][1]:<5d} {r['recal_us']:<9d} {share:<9s} "
                     f"{r['intr_preempt']:<9d} {r['thread_preempt']:<9d} "
                     f"{r['gw'][0] / r['ticks'] / 1000.0:6.3f} ({r['gw'][2]}, {r['gw'][1] / 1000.0:.2f})   {top_text}")
    if records:
        clocks = {r["clk0"] for r in records} | {r["clk1"] for r in records}
        lines.append("clock states seen: " + ", ".join(f"{c[0]}/{c[1]}" for c in sorted(clocks)))
        if boot_cal:
            lines.append("recal/boot cal ratio per window: " + " ".join(f"{r['recal_us'] / boot_cal:.2f}" for r in records))
        valid = [r for r in records if r["run_share"] is not None]
        if valid:
            lines.append(f"main-thread run share of wall: mean {sum(r['run_share'] for r in valid) / len(valid):.3f}, "
                         f"min {min(r['run_share'] for r in valid):.3f}")
        lines.append(f"unscoped GE waits, all windows: {sum(r['gw'][0] for r in records) / sum(r['ticks'] for r in records) / 1000.0:.3f} ms/tick, "
                     f"{sum(r['gw'][2] for r in records)} waits")
        names = ("SoundPlayer::ProcessQueues", "MemoryTelemetrySampleGameFrame", "loop head", "Sleep(0) yield",
                 "outer loop pump", "gated Render calls", "post-present", "cadence tick")
        ticks = sum(r["ticks"] for r in records)
        lines.append("main-loop work outside scopes, ms/tick: " + ", ".join(
            f"{names[i]} {sum(r['mo'][i][0] for r in records) / ticks / 1000.0:.3f} (max {max(r['mo'][i][1] for r in records) / 1000.0:.2f})" for i in range(4)))
        if any(r["mo"][i][2] for r in records for i in range(4, 8)):
            lines.append("main-loop segments (slots 4-7), ms/tick: " + ", ".join(
                f"{names[i]} {sum(r['mo'][i][0] for r in records) / ticks / 1000.0:.3f} (max {max(r['mo'][i][1] for r in records) / 1000.0:.2f}, {sum(r['mo'][i][2] for r in records)} calls)" for i in range(4, 8)))
        if any(r["gq"][7] for r in records):
            lines.append("GE queue per window (per frame ms): busy_lb..span_ub, submissions, busy-interval share")
            for i, r in enumerate(records, start=1):
                enq, busy_us, busy_n, idle_us, idle_n, swap_us, span_us, frames = r["gq"]
                if not frames:
                    lines.append(f"  {i:<3d} no frames")
                    continue
                lb = (busy_us + swap_us) / frames / 1000.0
                ub = span_us / frames / 1000.0
                share = busy_n / (busy_n + idle_n) if (busy_n + idle_n) else 0.0
                lines.append(f"  {i:<3d} sf={r['sf_first']}-{r['sf_last']:<8d} GE busy {lb:6.2f}..{ub:6.2f} ms/frame "
                             f"(swap wait {swap_us / frames / 1000.0:.2f}, unknown {idle_us / frames / 1000.0:.2f}), "
                             f"{enq / frames:.1f} lists/frame, busy intervals {share:.0%}")
            frames = sum(r["gq"][7] for r in records)
            if frames:
                lb = sum(r["gq"][1] + r["gq"][5] for r in records) / frames / 1000.0
                ub = sum(r["gq"][6] for r in records) / frames / 1000.0
                lines.append(f"GE busy per frame, all windows: {lb:.2f}..{ub:.2f} ms (frames {frames}, "
                             f"{sum(r['gq'][0] for r in records) / frames:.1f} lists/frame)")
        lines.append(f"flip-guard waits, all windows: {sum(r['fw'][0] for r in records) / sum(r['ticks'] for r in records) / 1000.0:.3f} ms/tick, "
                     f"{sum(r['fw'][2] for r in records)} waits, max {max(r['fw'][1] for r in records) / 1000.0:.2f} ms")
    return "\n".join(lines)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("log", type=pathlib.Path)
    args = parser.parse_args(argv)
    boot_cal, records = summarize(args.log.read_text(encoding="utf-8", errors="replace"))
    print(render(boot_cal, records))
    return 0 if records else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
