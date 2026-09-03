#!/usr/bin/env python3
"""Summarize TH08 PSP ITEM_ATAN2 records (Item autocollect atan2 fast path).

``mode=audit`` lines come from the shadow build: every autocollect angle ran
both the canonical Player::AngleToPoint and the double-float fast path, and
``mismatch`` counts accepted fast-path results whose binary32 bits differ from
the canonical result.  ``mode=product`` lines only count acceptances and
fallback reasons.  Sampled timers cover every 32nd call.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import sys
from dataclasses import dataclass

RECORD_PREFIX = "ITEM_ATAN2 V1 "
CONFIG_PREFIX = "PERF_ATTR_CONFIG V1 "
REASONS = ("r_zero", "r_nonfinite", "r_range", "r_tiny", "r_boundary")


@dataclass(frozen=True)
class Record:
    line_number: int
    fields: dict[str, str]

    def integer(self, name: str) -> int:
        return int(self.fields[name], 0)

    def triple(self, name: str) -> tuple[int, int, int]:
        values = self.fields[name].split("/")
        if len(values) != 3:
            raise ValueError(f"line {self.line_number}: {name} is not total/max/calls")
        return tuple(int(v, 0) for v in values)  # type: ignore[return-value]


def parse_fields(line_number: int, body: str) -> Record:
    fields: dict[str, str] = {}
    for token in body.split():
        if "=" in token:
            key, value = token.split("=", 1)
            fields[key] = value
    return Record(line_number, fields)


def parse_log(text: str, prefix: str = RECORD_PREFIX) -> tuple[list[Record], int | None]:
    records: list[Record] = []
    calibration_us: int | None = None
    for index, raw in enumerate(text.splitlines(), start=1):
        line = raw.strip()
        if line.startswith(CONFIG_PREFIX):
            config = parse_fields(index, line[len(CONFIG_PREFIX):])
            if "cal_us" in config.fields:
                calibration_us = config.integer("cal_us")
        elif line.startswith(prefix):
            records.append(parse_fields(index, line[len(prefix):]))
    return records, calibration_us


def _ratio(a: float, b: float) -> float:
    return a / b if b else 0.0


def summarize_record(record: Record, calibration_us: int | None) -> dict:
    calls = record.integer("calls")
    accepted = record.integer("accepted")
    mismatch = record.integer("mismatch")
    reasons = {name: record.integer(name) for name in REASONS}
    summary = {
        "line": record.line_number,
        "stage": record.integer("st"),
        "sf": record.fields["sf"],
        "mode": record.fields.get("mode", "?"),
        "calls": calls,
        "accepted": accepted,
        "accept_rate": _ratio(accepted, calls),
        "mismatch": mismatch,
        "reasons": reasons,
        "fallback_rate": _ratio(calls - accepted, calls),
        "delta_repeat_rate": _ratio(record.integer("delta_repeat"), calls) if "delta_repeat" in record.fields else None,
        "angle_repeat_rate": _ratio(record.integer("angle_repeat"), calls) if "angle_repeat" in record.fields else None,
        "sampled": record.integer("sampled") if "sampled" in record.fields else 0,
        "timing": {},
        "mismatch_records": [],
        "timer_overhead_us_per_read": (calibration_us / 1024.0) if calibration_us else None,
    }
    for name in ("t_canon", "t_fast", "t_vel"):
        if name in record.fields:
            total, maximum, count = record.triple(name)
            summary["timing"][name] = {
                "total_us": total,
                "max_us": maximum,
                "calls": count,
                "avg_us": _ratio(total, count),
            }
    recorded = record.integer("mm_recorded") if "mm_recorded" in record.fields else 0
    for i in range(min(recorded, 4)):
        key = f"mm{i}"
        if key in record.fields:
            summary["mismatch_records"].append(record.fields[key])
    return summary


def summarize(text: str, prefix: str = RECORD_PREFIX) -> list[dict]:
    records, calibration_us = parse_log(text, prefix)
    return [summarize_record(r, calibration_us) for r in records]


def render(summaries: list[dict]) -> str:
    if not summaries:
        return "no fast-path records found\n"
    lines = ["window        mode     calls accepted  accept%  mismatch  zero nonfin range  tiny bound  delta_rep% angle_rep%   canon us   fast us   vel us  speedup"]
    total_calls = total_acc = total_mm = 0
    for s in summaries:
        r = s["reasons"]
        t = s["timing"]
        canon = t.get("t_canon", {}).get("avg_us", 0.0)
        fast = t.get("t_fast", {}).get("avg_us", 0.0)
        vel = t.get("t_vel", {}).get("avg_us", 0.0)
        speed = _ratio(canon, fast) if fast else 0.0
        dr = s["delta_repeat_rate"]; ar = s["angle_repeat_rate"]
        lines.append(
            f"st{s['stage']} sf={s['sf']:<11} {s['mode']:<7} {s['calls']:6d} {s['accepted']:8d}  {100*s['accept_rate']:6.2f}  {s['mismatch']:8d}"
            f"  {r['r_zero']:4d} {r['r_nonfinite']:6d} {r['r_range']:5d} {r['r_tiny']:5d} {r['r_boundary']:5d}"
            f"   {(100*dr if dr is not None else 0):8.2f}   {(100*ar if ar is not None else 0):8.2f}"
            f"   {canon:8.2f}  {fast:8.2f} {vel:8.2f}  {speed:6.1f}x"
        )
        if s["mismatch"]:
            lines.append(f"  !! line {s['line']}: mismatch records (y/x/fast/canon bits): {' '.join(s['mismatch_records'])}")
        total_calls += s["calls"]; total_acc += s["accepted"]; total_mm += s["mismatch"]
    lines.append("")
    lines.append(f"all windows: calls={total_calls} accepted={total_acc} ({100*_ratio(total_acc,total_calls):.2f}%) mismatch={total_mm}")
    lines.append("gate: mismatch must be 0 in audit mode; timing is sampled (1/32 calls) and includes one timer read per section.")
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("log", type=pathlib.Path)
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--prefix", default="ITEM_ATAN2",
                        help="record name to summarize (ITEM_ATAN2 or ITEM_SINCOS)")
    args = parser.parse_args(argv)
    summaries = summarize(args.log.read_text(encoding="utf-8", errors="replace"),
                          args.prefix.rstrip() + " V1 ")
    if args.json:
        json.dump(summaries, sys.stdout, indent=2); sys.stdout.write("\n")
    else:
        sys.stdout.write(render(summaries))
    if not summaries:
        return 1
    return 0 if all(s["mismatch"] == 0 for s in summaries) else 2


if __name__ == "__main__":
    raise SystemExit(main())
