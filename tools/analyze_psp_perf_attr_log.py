#!/usr/bin/env python3
"""Summarize low-rate TH08 PSP PERF_ATTR records without double counting.

The diagnostic logs inclusive and nested phases on one line.  This tool keeps
the original fields visible and derives achieved simulation/render rates from
the measured wall interval; it deliberately does not add overlapping phases.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys
from dataclasses import dataclass


RECORD_PREFIX = "PERF_ATTR V1 "
PHASES = (
    ("Calc chain", "calc"),
    ("  Player update", "pu"),
    ("  Enemy update", "eu"),
    ("  Effect update", "fxu"),
    ("  Bullet update excl.", "bux"),
    ("  Item update", "iu"),
    ("  Calc other", "co"),
    ("Draw frame", "drawf"),
    ("  Draw chain", "drawc"),
    ("  Player draw", "pd"),
    ("  Enemy draw", "ed"),
    ("  Effect draw", "fxd"),
    ("  Bullet draw excl.", "bdx"),
    ("  Item draw", "id"),
    ("  Draw other", "do"),
    ("Present", "pres"),
    ("  GE wait", "ge"),
    ("  Swap VBlank", "vbs"),
    ("Cadence VBlank", "vbc"),
    ("Unattributed wall", "wo"),
)


@dataclass(frozen=True)
class Record:
    line_number: int
    fields: dict[str, str]

    def integer(self, name: str) -> int:
        value = self.fields[name]
        return int(value, 0)

    def total(self, name: str) -> int:
        return int(self.fields[name].split("/", 1)[0], 0)


@dataclass(frozen=True)
class LogIdentity:
    build: str
    cpu: str
    bus: str
    initial_mode: str


def parse_records(text: str) -> list[Record]:
    records: list[Record] = []
    for line_number, line in enumerate(text.splitlines(), 1):
        marker = line.find(RECORD_PREFIX)
        if marker < 0:
            continue
        fields: dict[str, str] = {}
        payload = line[marker + len(RECORD_PREFIX) :]
        for token in payload.split():
            if "=" not in token:
                continue
            key, value = token.split("=", 1)
            fields[key] = value
        required = {
            "st",
            "sf",
            "sim_frames",
            "rendered_frames",
            "cadence_mode",
            "wall",
            "calc",
            "drawf",
            "pres",
            "uf",
        }
        missing = sorted(required - fields.keys())
        if missing:
            raise ValueError(
                f"line {line_number}: missing PERF_ATTR fields: "
                + ", ".join(missing)
            )
        records.append(Record(line_number, fields))
    return records


def find_scalar(text: str, pattern: str) -> str:
    match = re.search(pattern, text)
    return match.group(1) if match else "?"


def parse_identity(text: str) -> LogIdentity:
    return LogIdentity(
        build=find_scalar(text, r"BUILD id=([^\s]+)"),
        cpu=find_scalar(text, r"clock_cpu=(\d+)"),
        bus=find_scalar(text, r"clock_bus=(\d+)"),
        initial_mode=find_scalar(
            text, r"RENDER_CADENCE init_mode=(\d+)"
        ),
    )


def identity_line(identity: LogIdentity) -> str:
    return (
        "log_identity "
        f"build={identity.build} cpu={identity.cpu} bus={identity.bus} "
        f"initial_mode={identity.initial_mode}"
    )


def percent(value: int, wall: int) -> float:
    return 100.0 * value / wall if wall else 0.0


def summarize(record: Record) -> str:
    wall = record.integer("wall")
    simulation_frames = record.integer("sim_frames")
    rendered_frames = record.integer("rendered_frames")
    simulation_fps = simulation_frames * 1_000_000.0 / wall if wall else 0.0
    render_fps = rendered_frames * 1_000_000.0 / wall if wall else 0.0
    per_simulation_ms = wall / simulation_frames / 1000.0

    lines = [
        f"PERF_ATTR line {record.line_number}: stage={record.fields['st']} "
        f"sf={record.fields['sf']} cadence={record.fields['cadence_mode']}",
        f"wall={wall / 1_000_000.0:.6f}s "
        f"sim={simulation_fps:.3f}fps render={render_fps:.3f}fps "
        f"wall/sim={per_simulation_ms:.3f}ms "
        f"rendered={rendered_frames}/{simulation_frames} "
        f"uf={record.fields['uf']}",
        "phase totals are overlapping; percentages are each versus wall:",
    ]
    for label, field in PHASES:
        if field not in record.fields:
            continue
        total = record.total(field)
        lines.append(
            f"  {label:<23} {total / 1000.0:10.3f} ms "
            f"{percent(total, wall):7.2f}%"
        )
    return "\n".join(lines)


def summarize_all(records: list[Record]) -> str:
    wall = sum(record.integer("wall") for record in records)
    simulation_frames = sum(record.integer("sim_frames") for record in records)
    rendered_frames = sum(
        record.integer("rendered_frames") for record in records
    )
    window_rates = [
        record.integer("sim_frames") * 1_000_000.0 / record.integer("wall")
        for record in records
        if record.integer("wall")
    ]
    simulation_fps = simulation_frames * 1_000_000.0 / wall
    render_fps = rendered_frames * 1_000_000.0 / wall
    lines = [
        f"aggregate windows={len(records)} sim_frames={simulation_frames} "
        f"rendered_frames={rendered_frames}",
        f"wall={wall / 1_000_000.0:.6f}s sim={simulation_fps:.3f}fps "
        f"render={render_fps:.3f}fps wall/sim="
        f"{wall / simulation_frames / 1000.0:.3f}ms "
        f"window_sim_min={min(window_rates):.3f} "
        f"window_sim_max={max(window_rates):.3f}",
        "phase totals are overlapping; percentages are each versus wall:",
    ]
    for label, field in PHASES:
        present = [record for record in records if field in record.fields]
        if len(present) != len(records):
            continue
        total = sum(record.total(field) for record in records)
        lines.append(
            f"  {label:<23} {total / 1000.0:10.3f} ms "
            f"{percent(total, wall):7.2f}%"
        )
    return "\n".join(lines)


def invalid_record_lines(records: list[Record]) -> list[int]:
    return [
        record.line_number
        for record in records
        if record.integer("uf") != 0
        or record.integer("sim_frames") != 600
        or record.integer("cr") != 0
    ]


def comparison_key(record: Record) -> tuple[str, ...]:
    return tuple(
        record.fields.get(field, "?")
        for field in (
            "st",
            "sf",
            "sim_frames",
            "rendered_frames",
            "cadence_mode",
            "replay",
            "demo",
        )
    )


def ratio_text(candidate: int, baseline: int) -> str:
    if baseline == 0:
        return "n/a"
    return f"{candidate / baseline:.6f}"


def summarize_comparison(
    baseline_records: list[Record],
    candidate_records: list[Record],
    baseline_identity: LogIdentity,
    candidate_identity: LogIdentity,
) -> str:
    if baseline_identity.build != candidate_identity.build:
        raise ValueError(
            "build mismatch: "
            f"{baseline_identity.build} != {candidate_identity.build}"
        )
    if baseline_identity.initial_mode != candidate_identity.initial_mode:
        raise ValueError(
            "initial cadence mismatch: "
            f"{baseline_identity.initial_mode} != "
            f"{candidate_identity.initial_mode}"
        )
    if len(baseline_records) != len(candidate_records):
        raise ValueError(
            "PERF_ATTR window-count mismatch: "
            f"{len(baseline_records)} != {len(candidate_records)}"
        )

    for index, (baseline, candidate) in enumerate(
        zip(baseline_records, candidate_records), 1
    ):
        if comparison_key(baseline) != comparison_key(candidate):
            raise ValueError(
                f"window {index} identity mismatch: "
                f"{comparison_key(baseline)} != {comparison_key(candidate)}"
            )

    ideal_ratio = None
    if baseline_identity.cpu.isdigit() and candidate_identity.cpu.isdigit():
        candidate_cpu = int(candidate_identity.cpu)
        if candidate_cpu:
            ideal_ratio = int(baseline_identity.cpu) / candidate_cpu

    lines = [
        "paired_comparison duration_ratio=candidate/baseline "
        "speedup=baseline/candidate",
        f"baseline {identity_line(baseline_identity)}",
        f"candidate {identity_line(candidate_identity)}",
    ]
    if ideal_ratio is not None:
        lines.append(
            "ideal_cpu_duration_ratio=" f"{ideal_ratio:.6f}"
        )

    lines.append("windows:")
    for baseline, candidate in zip(baseline_records, candidate_records):
        baseline_wall = baseline.integer("wall")
        candidate_wall = candidate.integer("wall")
        simulation_frames = baseline.integer("sim_frames")
        baseline_fps = simulation_frames * 1_000_000.0 / baseline_wall
        candidate_fps = simulation_frames * 1_000_000.0 / candidate_wall
        lines.append(
            f"  st={baseline.fields['st']} sf={baseline.fields['sf']} "
            f"baseline={baseline_fps:.3f}fps "
            f"candidate={candidate_fps:.3f}fps "
            f"wall_ratio={ratio_text(candidate_wall, baseline_wall)} "
            f"speedup={baseline_wall / candidate_wall:.6f}x"
        )

    baseline_wall = sum(record.integer("wall") for record in baseline_records)
    candidate_wall = sum(record.integer("wall") for record in candidate_records)
    simulation_frames = sum(
        record.integer("sim_frames") for record in baseline_records
    )
    lines.extend(
        (
            "aggregate:",
            f"  sim_frames={simulation_frames} "
            f"baseline_wall={baseline_wall / 1_000_000.0:.6f}s "
            f"candidate_wall={candidate_wall / 1_000_000.0:.6f}s",
            f"  baseline={simulation_frames * 1_000_000.0 / baseline_wall:.3f}fps "
            f"candidate={simulation_frames * 1_000_000.0 / candidate_wall:.3f}fps "
            f"wall_ratio={candidate_wall / baseline_wall:.6f} "
            f"speedup={baseline_wall / candidate_wall:.6f}x",
            "phase totals are overlapping; compare each phase independently:",
        )
    )
    for label, field in PHASES:
        if not all(field in record.fields for record in baseline_records):
            continue
        if not all(field in record.fields for record in candidate_records):
            continue
        baseline_total = sum(
            record.total(field) for record in baseline_records
        )
        candidate_total = sum(
            record.total(field) for record in candidate_records
        )
        speedup = (
            f"{baseline_total / candidate_total:.6f}x"
            if candidate_total
            else "n/a"
        )
        lines.append(
            f"  {label:<23} baseline={baseline_total / 1000.0:10.3f}ms "
            f"candidate={candidate_total / 1000.0:10.3f}ms "
            f"ratio={ratio_text(candidate_total, baseline_total)} "
            f"speedup={speedup}"
        )
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("log", type=pathlib.Path)
    parser.add_argument(
        "--compare",
        type=pathlib.Path,
        help="same-build/same-window candidate log (for example 443 MHz)",
    )
    args = parser.parse_args()
    text = args.log.read_text(encoding="utf-8", errors="replace")
    records = parse_records(text)
    if not records:
        print("no PERF_ATTR V1 records", file=sys.stderr)
        return 2

    identity = parse_identity(text)
    if args.compare is not None:
        compare_text = args.compare.read_text(
            encoding="utf-8", errors="replace"
        )
        compare_records = parse_records(compare_text)
        if not compare_records:
            print("no PERF_ATTR V1 records in comparison log", file=sys.stderr)
            return 2
        invalid = invalid_record_lines(records)
        compare_invalid = invalid_record_lines(compare_records)
        if invalid or compare_invalid:
            if invalid:
                print(
                    "invalid baseline attribution records at lines: "
                    + ", ".join(map(str, invalid)),
                    file=sys.stderr,
                )
            if compare_invalid:
                print(
                    "invalid candidate attribution records at lines: "
                    + ", ".join(map(str, compare_invalid)),
                    file=sys.stderr,
                )
            return 1
        try:
            print(
                summarize_comparison(
                    records,
                    compare_records,
                    identity,
                    parse_identity(compare_text),
                )
            )
        except ValueError as error:
            print(f"comparison rejected: {error}", file=sys.stderr)
            return 1
        return 0

    print(identity_line(identity))
    for index, record in enumerate(records):
        if index:
            print()
        print(summarize(record))
    print()
    print(summarize_all(records))

    invalid = invalid_record_lines(records)
    if invalid:
        print(
            "invalid attribution records at lines: "
            + ", ".join(map(str, invalid)),
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
