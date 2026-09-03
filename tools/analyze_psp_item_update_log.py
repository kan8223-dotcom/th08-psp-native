#!/usr/bin/env python3
"""Summarize TH08 PSP ITEM_UPD records (the Item update sub-profile).

Each ``ITEM_UPD V1`` line sits on the same 600-stage-tick boundary as its
parent ``PERF_ATTR V1`` window.  Whole-population counters (``items=``,
``c_*``, ``s_*``) cover every visited item.  Section timers (``whole``,
``auto``, ``coll``, ``script``) cover only the rotating 1-of-32 list-position
sample, so their per-item means are the authoritative figures and every
per-tick projection printed here is an estimate for ranking only.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import sys
from dataclasses import dataclass

RECORD_PREFIX = "ITEM_UPD V1 "
PERF_PREFIX = "PERF_ATTR V1 "
CONFIG_PREFIX = "PERF_ATTR_CONFIG V1 "
SECTIONS = ("whole", "auto", "coll", "script")
COUNTERS = ("c_visit", "c_auto", "c_coll", "c_pick", "c_off", "c_script")
STATES = ("s_default", "s_auto", "s_spread", "s_rising", "s_apex", "s_other")


@dataclass(frozen=True)
class Record:
    line_number: int
    fields: dict[str, str]

    def integer(self, name: str) -> int:
        return int(self.fields[name], 0)

    def triple(self, name: str) -> tuple[int, int, int]:
        values = self.fields[name].split("/")
        if len(values) != 3:
            raise ValueError(
                f"line {self.line_number}: {name} is not total/max/calls"
            )
        return tuple(int(value, 0) for value in values)  # type: ignore[return-value]

    def window_key(self) -> tuple[int, str]:
        return (self.integer("st"), self.fields["sf"])


def parse_fields(line_number: int, body: str) -> Record:
    fields: dict[str, str] = {}
    for token in body.split():
        if "=" not in token:
            continue
        key, value = token.split("=", 1)
        fields[key] = value
    return Record(line_number, fields)


def parse_log(text: str) -> tuple[list[Record], dict[tuple[int, str], Record], int | None]:
    records: list[Record] = []
    perf: dict[tuple[int, str], Record] = {}
    calibration_us: int | None = None
    for index, raw in enumerate(text.splitlines(), start=1):
        line = raw.strip()
        if line.startswith(CONFIG_PREFIX):
            config = parse_fields(index, line[len(CONFIG_PREFIX):])
            if "cal_us" in config.fields:
                calibration_us = config.integer("cal_us")
        elif line.startswith(PERF_PREFIX):
            record = parse_fields(index, line[len(PERF_PREFIX):])
            perf[record.window_key()] = record
        elif line.startswith(RECORD_PREFIX):
            records.append(parse_fields(index, line[len(RECORD_PREFIX):]))
    return records, perf, calibration_us


def _ratio(numerator: float, denominator: float) -> float:
    return numerator / denominator if denominator else 0.0


def summarize_window(record: Record, perf: Record | None,
                     calibration_us: int | None) -> dict:
    ticks = record.integer("ticks")
    items_sum, items_max, item_ticks = record.triple("items")
    sampled = record.integer("sampled")
    sections = {name: record.triple(name) for name in SECTIONS}
    whole_total = sections["whole"][0]
    nested = sum(sections[name][0] for name in ("auto", "coll", "script"))
    residual = record.integer("resid") if "resid" in record.fields else max(
        whole_total - nested, 0)
    items_avg = _ratio(items_sum, ticks)
    whole_avg_us = _ratio(whole_total, sampled)
    summary = {
        "line": record.line_number,
        "stage": record.integer("st"),
        "sf": record.fields["sf"],
        "ticks": ticks,
        "item_ticks": item_ticks,
        "items_avg": items_avg,
        "items_max": items_max,
        "sampled": sampled,
        "timer_reads": record.integer("timer_reads"),
        "cr": record.integer("cr"),
        "ov": record.integer("ov"),
        "uf": record.fields.get("uf", "0x00"),
        "whole_avg_us": whole_avg_us,
        "whole_max_us": sections["whole"][1],
        "sections": {},
        "residual_share": _ratio(residual, whole_total),
        "residual_us_per_item": _ratio(residual, sampled),
        "projected_ms_per_tick": whole_avg_us * items_avg / 1000.0,
        "counts_per_tick": {
            name: _ratio(record.integer(name), ticks) for name in COUNTERS
        },
        "state_share": {},
        "iu_ms_per_tick": None,
        "iu_max_ms": None,
        "overhead_ms_per_tick": None,
    }
    for name in ("auto", "coll", "script"):
        total, maximum, calls = sections[name]
        summary["sections"][name] = {
            "total_us": total,
            "max_us": maximum,
            "calls": calls,
            "avg_us_per_call": _ratio(total, calls),
            "share_of_whole": _ratio(total, whole_total),
        }
    visited = record.integer("c_visit")
    for name in STATES:
        summary["state_share"][name] = _ratio(record.integer(name), visited)
    if perf is not None and "iu" in perf.fields:
        iu_total, iu_max, _iu_calls = perf.triple("iu")
        summary["iu_ms_per_tick"] = _ratio(iu_total, ticks) / 1000.0
        summary["iu_max_ms"] = iu_max / 1000.0
    if calibration_us is not None:
        overhead_us = record.integer("timer_reads") * calibration_us / 1024.0
        summary["overhead_ms_per_tick"] = _ratio(overhead_us, ticks) / 1000.0
    return summary


def summarize(text: str) -> list[dict]:
    records, perf, calibration_us = parse_log(text)
    return [
        summarize_window(record, perf.get(record.window_key()), calibration_us)
        for record in records
    ]


def _fmt_ms(value: float | None) -> str:
    return "   n/a" if value is None else f"{value:6.3f}"


def render(summaries: list[dict]) -> str:
    lines: list[str] = []
    if not summaries:
        return "no ITEM_UPD V1 records found\n"
    lines.append(
        "window        ticks items/tick  max  sampled  iu ms/tick  proj ms/tick"
        "  tax ms/tick  whole us  auto%  coll%  script%  resid%"
    )
    for s in summaries:
        sec = s["sections"]
        lines.append(
            f"st{s['stage']} sf={s['sf']:<11} {s['ticks']:5d} {s['items_avg']:9.1f}"
            f" {s['items_max']:4d} {s['sampled']:8d}  {_fmt_ms(s['iu_ms_per_tick'])}"
            f"      {s['projected_ms_per_tick']:6.3f}       {_fmt_ms(s['overhead_ms_per_tick'])}"
            f"  {s['whole_avg_us']:8.2f}  {100 * sec['auto']['share_of_whole']:5.1f}"
            f"  {100 * sec['coll']['share_of_whole']:5.1f}  {100 * sec['script']['share_of_whole']:6.1f}"
            f"  {100 * s['residual_share']:5.1f}"
        )
        if s["cr"] or s["ov"] or s["uf"] not in ("0x00", "0"):
            lines.append(
                f"  !! line {s['line']}: cr={s['cr']} ov={s['ov']} uf={s['uf']}"
            )
    lines.append("")
    lines.append("per-call means over the sample (us): auto / coll / script, and counts per tick")
    for s in summaries:
        sec = s["sections"]
        c = s["counts_per_tick"]
        st = s["state_share"]
        lines.append(
            f"st{s['stage']} sf={s['sf']:<11} auto={sec['auto']['avg_us_per_call']:6.2f}"
            f" coll={sec['coll']['avg_us_per_call']:6.2f}"
            f" script={sec['script']['avg_us_per_call']:6.2f}"
            f" resid/item={s['residual_us_per_item']:6.2f}"
            f" | per tick: auto={c['c_auto']:6.1f} coll={c['c_coll']:6.1f}"
            f" pick={c['c_pick']:5.2f} off={c['c_off']:5.2f} script={c['c_script']:6.1f}"
            f" | state default={100 * st['s_default']:4.1f}% auto={100 * st['s_auto']:4.1f}%"
            f" spread={100 * st['s_spread']:4.1f}% rising={100 * st['s_rising']:4.1f}%"
            f" apex={100 * st['s_apex']:4.1f}%"
        )
    total_whole = sum(s["whole_avg_us"] * s["sampled"] for s in summaries)
    if total_whole:
        lines.append("")
        agg = {name: 0.0 for name in ("auto", "coll", "script")}
        for s in summaries:
            for name in agg:
                agg[name] += s["sections"][name]["total_us"]
        resid = total_whole - sum(agg.values())
        lines.append(
            "all windows, sampled-time shares: "
            + " ".join(f"{name}={100 * agg[name] / total_whole:5.1f}%" for name in agg)
            + f" residual={100 * resid / total_whole:5.1f}%"
        )
    lines.append("")
    lines.append(
        "projection = whole mean per sampled item x mean items per tick; "
        "compare against iu ms/tick (parent PERF_ATTR) and subtract tax "
        "(timer_reads x cal_us/1024) before ranking."
    )
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("log", type=pathlib.Path)
    parser.add_argument("--json", action="store_true",
                        help="emit the per-window summaries as JSON")
    args = parser.parse_args(argv)
    text = args.log.read_text(encoding="utf-8", errors="replace")
    summaries = summarize(text)
    if args.json:
        json.dump(summaries, sys.stdout, indent=2)
        sys.stdout.write("\n")
    else:
        sys.stdout.write(render(summaries))
    return 0 if summaries else 1


if __name__ == "__main__":
    raise SystemExit(main())
