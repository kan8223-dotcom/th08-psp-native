#!/usr/bin/env python3
"""Read mixed-cadence RAM-log spans; never treat stage progression as calc count."""
from __future__ import annotations

import argparse
import pathlib
import re
from dataclasses import dataclass

PREFIX = "PERF_SPAN V1 "
PHASES = ("calc", "drawf", "drawc", "pu", "pd", "eu", "ed", "fxu", "fxdm", "fxdb", "fxdbg",
          "bui", "bdi", "iu", "id", "pres", "pre", "sw", "post", "ge", "vbs", "vbc")


@dataclass(frozen=True)
class Span:
    line: int
    fields: dict[str, str]

    def number(self, key: str) -> int:
        return int(self.fields[key])

    def phase(self, key: str) -> tuple[int, int, int]:
        return tuple(map(int, self.fields[key].split("/")))


def parse_spans(text: str) -> list[Span]:
    spans = []
    numeric = ("stage_ticks", "calc_calls", "presents", "unknown_modes", "repeats", "switches",
               "replay", "demo_mode", "demo_index", "partial", "target_calc", "overshoot", "wall",
               "reads", "cal_us", "cr")
    for line_number, line in enumerate(text.splitlines(), 1):
        if not line.startswith(PREFIX):
            continue
        fields = dict(token.split("=", 1) for token in line[len(PREFIX):].split())
        span = Span(line_number, fields)
        try:
            if any(span.number(key) < 0 for key in numeric):
                raise ValueError("negative unsigned field")
            int(fields["st"])
            match = re.fullmatch(r"(\d+)-(\d+)", fields["sf"])
            if not match or int(match[2]) - int(match[1]) != span.number("stage_ticks"):
                raise ValueError("stage interval mismatch")
            modes = list(map(int, fields["modes"].split("/")))
            if len(modes) != 3 or min(modes) < 0 or sum(modes) + span.number("unknown_modes") != span.number("presents"):
                raise ValueError("present/mode count mismatch")
            for key in PHASES:
                phase = span.phase(key)
                if len(phase) != 3 or min(phase) < 0 or phase[1] > phase[0]:
                    raise ValueError("invalid phase " + key)
            if span.phase("calc")[2] != span.number("calc_calls"):
                raise ValueError("calc callback count mismatch")
            if span.number("partial") != int(fields["reason"] != "period"):
                raise ValueError("partial/reason mismatch")
            if span.number("overshoot") != max(0, span.number("calc_calls") - span.number("target_calc")):
                raise ValueError("overshoot mismatch")
            if not span.number("partial") and span.number("calc_calls") < span.number("target_calc"):
                raise ValueError("short period")
        except (KeyError, ValueError) as error:
            raise ValueError(f"line {line_number}: {error}") from error
        spans.append(span)
    return spans


def describe_span(span: Span) -> str:
    f = span.fields
    header = (f"line={span.line} stage={f['st']} sf={f['sf']} reason={f['reason']} "
              f"calc_calls={f['calc_calls']} stage_ticks={f['stage_ticks']} presents={f['presents']} "
              f"draws_60_30_20={f['modes']} repeated_stage_frames={f['repeats']}")
    wall, calls, draws = span.number("wall"), span.number("calc_calls"), span.number("presents")
    if span.number("cr") or wall == 0:
        return header + "\nINVALID clock interval; rates not calculated"
    lines = [header, f"calc_callback_hz={calls * 1e6 / wall:.3f} render_fps={draws * 1e6 / wall:.3f} "
             f"wall_ms={wall / 1000:.3f}"]
    total = lambda key: span.phase(key)[0]
    derived = {
        "bullet_update_excl": total("bui") - total("iu"),
        "bullet_draw_excl": total("bdi") - total("id") - total("fxdb"),
        "calc_other": total("calc") - sum(total(k) for k in ("pu", "eu", "fxu", "bui")),
        "draw_other": total("drawc") - sum(total(k) for k in ("pd", "ed", "fxdm", "fxdbg", "bdi")),
        "wall_other": wall - sum(total(k) for k in ("calc", "drawf", "pres", "vbc")),
    }
    if min(derived.values()) < 0:
        lines.append("WARNING overlapping-owner underflow; do not use derived totals")
    for key in ("calc", "pu", "eu", "fxu", "bui", "iu", "drawf", "drawc", "pd", "ed", "bdi", "id",
                "fxdm", "fxdb", "fxdbg", "pres", "ge", "vbs", "vbc"):
        denominator = draws if key in ("drawf", "drawc", "pd", "ed", "bdi", "id", "fxdm", "fxdb", "fxdbg", "pres") else calls
        per = f"{total(key) / denominator / 1000:.6f}" if denominator else "n/a"
        lines.append(f"  {key}: total_ms={total(key) / 1000:.3f} per_{'present' if denominator == draws and key not in ('calc','pu','eu','fxu','bui','iu','ge','vbs','vbc') else 'calc'}_ms={per}")
    if min(derived.values()) >= 0:
        lines.append("  derived_ms: " + " ".join(f"{k}={v / 1000:.3f}" for k, v in derived.items()))
    lines.append("Inclusive phase rows overlap; do not add them together.")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=pathlib.Path)
    args = parser.parse_args()
    try:
        spans = parse_spans(args.log.read_text(encoding="utf-8", errors="replace"))
    except ValueError as error:
        parser.error(str(error))
    if not spans:
        parser.error("no PERF_SPAN V1 records (older PERF_ATTR logs need the original analyzer)")
    for span in spans:
        print(describe_span(span))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
