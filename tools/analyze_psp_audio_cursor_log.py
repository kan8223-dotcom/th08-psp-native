#!/usr/bin/env python3
"""Summarize TH08 PSP AUDIO_CURSOR V1 records (SC mixer fixed-point cursor).

One record per parent PERF_ATTR window.  In ``audit`` mode the canonical
binary64 cursor drives the mix and the fixed-point cursor is shadow-stepped per
output frame; ``mismatch`` counts frames whose source index differed,
``wrap_mismatch``/``cursor_mismatch``/``stop_mismatch`` count per-Mix
disagreements of the wrap flag, the post-mix cursor value and the stop
decision.  In ``product`` mode only the eligibility and rate histogram fields
are meaningful.  The gate for promoting the product switch is every mismatch
field equal to zero across every window.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import sys

RECORD_PREFIX = "AUDIO_CURSOR V1 "
COUNTERS = ("mixes", "eligible", "inelig_rate", "inelig_cursor", "frames", "mismatch",
            "wrap_mismatch", "cursor_mismatch", "stop_mismatch", "r44100", "r22050",
            "r11025", "rother", "max_frames")
MISMATCH_FIELDS = ("mismatch", "wrap_mismatch", "cursor_mismatch", "stop_mismatch")


def parse_fields(body: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    for token in body.split():
        if "=" in token:
            key, value = token.split("=", 1)
            fields[key] = value
    return fields


def summarize(text: str) -> list[dict]:
    out = []
    for number, raw in enumerate(text.splitlines(), start=1):
        line = raw.strip()
        if not line.startswith(RECORD_PREFIX):
            continue
        f = parse_fields(line[len(RECORD_PREFIX):])
        try:
            first, last = (int(x) for x in f["sf"].split("-"))
            record = {"line": number, "stage": int(f["st"]), "sf_first": first, "sf_last": last,
                      "mode": f["mode"]}
            for key in COUNTERS:
                record[key] = int(f[key])
        except (KeyError, ValueError) as error:
            raise ValueError(f"line {number}: malformed AUDIO_CURSOR record ({error})") from error
        if record["mode"] not in ("audit", "product"):
            raise ValueError(f"line {number}: unknown mode {record['mode']!r}")
        ticks = max(1, last - first)
        record["mixes_per_tick"] = record["mixes"] / ticks
        record["frames_per_tick"] = record["frames"] / ticks
        record["eligible_pct"] = 100.0 * record["eligible"] / record["mixes"] if record["mixes"] else 0.0
        record["mismatch_total"] = sum(record[k] for k in MISMATCH_FIELDS)
        out.append(record)
    return out


def gate(records: list[dict]) -> tuple[str, str]:
    """Return (verdict, reason) for promoting the product switch."""
    if not records:
        return "NO_DATA", "no AUDIO_CURSOR records"
    audit = [r for r in records if r["mode"] == "audit"]
    if not audit:
        return "PRODUCT_ONLY", "records are product-mode; no shadow comparison present"
    bad = [r for r in audit if r["mismatch_total"]]
    if bad:
        return "FAIL", f"{len(bad)} window(s) with mismatches (first at line {bad[0]['line']})"
    frames = sum(r["frames"] for r in audit)
    if frames == 0:
        return "EMPTY", "audit windows compared zero frames"
    return "PASS", f"{len(audit)} audit window(s), {frames} frames compared, zero mismatches"


def render(records: list[dict]) -> str:
    lines = []
    lines.append("window   stage  sf            mode     mixes/tick  frames/tick  elig%   mismatch  wrap  cursor  stop  r44100/r22050/r11025/other  max_frames")
    for i, r in enumerate(records, start=1):
        lines.append(
            f"{i:<8d} {r['stage']:<6d} {r['sf_first']}-{r['sf_last']:<8d} {r['mode']:<8s} "
            f"{r['mixes_per_tick']:<11.2f} {r['frames_per_tick']:<12.1f} {r['eligible_pct']:<7.1f} "
            f"{r['mismatch']:<9d} {r['wrap_mismatch']:<5d} {r['cursor_mismatch']:<7d} {r['stop_mismatch']:<5d} "
            f"{r['r44100']}/{r['r22050']}/{r['r11025']}/{r['rother']:<14d} {r['max_frames']}")
    if records:
        total_frames = sum(r["frames"] for r in records)
        total_mixes = sum(r["mixes"] for r in records)
        total_elig = sum(r["eligible"] for r in records)
        lines.append(f"total: windows={len(records)} mixes={total_mixes} eligible={total_elig} "
                     f"inelig_rate={sum(r['inelig_rate'] for r in records)} "
                     f"inelig_cursor={sum(r['inelig_cursor'] for r in records)} frames={total_frames} "
                     f"mismatch={sum(r['mismatch_total'] for r in records)}")
    verdict, reason = gate(records)
    lines.append(f"gate: {verdict} ({reason})")
    return "\n".join(lines)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("log", type=pathlib.Path)
    parser.add_argument("--json", action="store_true", help="emit JSON instead of the table")
    args = parser.parse_args(argv)
    text = args.log.read_text(encoding="utf-8", errors="replace")
    records = summarize(text)
    if args.json:
        verdict, reason = gate(records)
        print(json.dumps({"records": records, "gate": verdict, "reason": reason}, indent=2))
    else:
        print(render(records))
    return 0 if gate(records)[0] in ("PASS", "PRODUCT_ONLY") else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
