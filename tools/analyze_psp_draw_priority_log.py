#!/usr/bin/env python3
"""Summarize low-rate TH08 PSP DRAW_PRIO records.

The runtime samples exactly one of each sixteen presented frames.  Raw timer
totals remain authoritative; projections merely scale the sampled mean to the
number of presented frames in the same PERF_ATTR window so the next owner to
profile is obvious.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys
from dataclasses import dataclass


RECORD_PREFIX = "DRAW_PRIO V1 "
PERF_PREFIX = "PERF_ATTR V1 "
PRIORITY_NAMES = {
    0: "Supervisor",
    2: "Supervisor loading VMs",
    3: "MusicRoom/TitleScreen",
    4: "Ending",
    5: "GameManager",
    6: "Background high",
    7: "Background low (incl. Effect background)",
    8: "Enemy high",
    9: "Player high",
    10: "Player low",
    11: "Enemy low",
    12: "EffectManager",
    13: "BulletManager",
    14: "Ascii high",
    15: "Spellcard",
    16: "FPS counter",
    17: "GUI",
    18: "ResultScreen",
    20: "Ascii low",
    21: "ScreenEffect",
    22: "Other priority",
}


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

    def pair(self, name: str) -> tuple[int, int]:
        values = self.fields[name].split("/")
        if len(values) != 2:
            raise ValueError(
                f"line {self.line_number}: {name} is not total/calls"
            )
        return int(values[0], 0), int(values[1], 0)


def tokenize_records(text: str, prefix: str) -> list[Record]:
    records: list[Record] = []
    for line_number, line in enumerate(text.splitlines(), 1):
        marker = line.find(prefix)
        if marker < 0:
            continue
        fields: dict[str, str] = {}
        for token in line[marker + len(prefix) :].split():
            if "=" in token:
                key, value = token.split("=", 1)
                fields[key] = value
        records.append(Record(line_number, fields))
    return records


def parse_records(text: str) -> list[Record]:
    records = tokenize_records(text, RECORD_PREFIX)
    required = {
        "st",
        "sf",
        "presented",
        "cadence_mode",
        "sampled",
        "chain",
        "cb",
        "residual",
        "effect_bg",
        "p7x",
        "timer_reads",
        "cr",
        "ov",
        "uf",
        "po",
    } | {f"p{priority}" for priority in range(22)}
    for record in records:
        missing = sorted(required - record.fields.keys())
        if missing:
            raise ValueError(
                f"line {record.line_number}: missing DRAW_PRIO fields: "
                + ", ".join(missing)
            )
        record.triple("chain")
        record.pair("cb")
        record.triple("effect_bg")
        record.triple("po")
        for priority in range(22):
            record.triple(f"p{priority}")
    return records


def parse_perf_records(text: str) -> dict[tuple[str, str, str], Record]:
    result: dict[tuple[str, str, str], Record] = {}
    for record in tokenize_records(text, PERF_PREFIX):
        if {"st", "sf", "cadence_mode"} <= record.fields.keys():
            key = (
                record.fields["st"],
                record.fields["sf"],
                record.fields["cadence_mode"],
            )
            result[key] = record
    return result


def identity_line(text: str) -> str:
    def find(pattern: str) -> str:
        match = re.search(pattern, text)
        return match.group(1) if match else "?"

    return (
        "log_identity "
        f"build={find(r'BUILD id=([^\s]+)')} "
        f"cpu={find(r'clock_cpu=(\d+)')} "
        f"bus={find(r'clock_bus=(\d+)')}"
    )


def bins(record: Record) -> list[tuple[int, int, int, int]]:
    values = []
    for priority in range(22):
        total, maximum, calls = record.triple(f"p{priority}")
        values.append((priority, total, maximum, calls))
    total, maximum, calls = record.triple("po")
    values.append((22, total, maximum, calls))
    return values


def projected(total_us: int, sampled: int, presented: int) -> float:
    return total_us * presented / sampled if sampled else 0.0


def summarize_record(record: Record, perf: Record | None) -> str:
    presented = record.integer("presented")
    sampled = record.integer("sampled")
    chain_total, chain_max, chain_calls = record.triple("chain")
    effect_total, effect_max, effect_calls = record.triple("effect_bg")
    callback_total, callback_calls = record.pair("cb")
    rows = sorted(bins(record), key=lambda row: row[1], reverse=True)
    lines = [
        f"DRAW_PRIO line {record.line_number}: stage={record.fields['st']} "
        f"sf={record.fields['sf']} cadence={record.fields['cadence_mode']}",
        f"sampled={sampled}/{presented} chain_raw={chain_total / 1000.0:.3f}ms "
        f"chain_max={chain_max / 1000.0:.3f}ms chain_calls={chain_calls} "
        f"callbacks_raw={callback_total / 1000.0:.3f}ms "
        f"callback_calls={callback_calls} "
        f"dispatch_residual_raw={record.integer('residual') / 1000.0:.3f}ms",
        f"effect_background_raw={effect_total / 1000.0:.3f}ms "
        f"max={effect_max / 1000.0:.3f}ms calls={effect_calls} "
        f"priority7_exclusive_raw={record.integer('p7x') / 1000.0:.3f}ms",
        "ranked sampled callback bins (projection is an estimate):",
    ]
    for priority, total, maximum, calls in rows:
        if calls == 0 and total == 0:
            continue
        average = total / calls if calls else 0.0
        estimate = projected(total, sampled, presented)
        lines.append(
            f"  p{priority:<2} {PRIORITY_NAMES.get(priority, 'Unknown'):<41} "
            f"raw={total / 1000.0:9.3f}ms "
            f"est={estimate / 1000.0:9.3f}ms "
            f"max={maximum / 1000.0:7.3f}ms "
            f"avg={average / 1000.0:7.3f}ms calls={calls}"
        )

    if perf is not None and "drawc" in perf.fields and "do" in perf.fields:
        parent_draw = int(perf.fields["drawc"].split("/", 1)[0], 0)
        parent_other = int(perf.fields["do"].split("/", 1)[0], 0)
        estimate = projected(chain_total, sampled, presented)
        lines.append(
            "projection_crosscheck "
            f"chain_est={estimate / 1000.0:.3f}ms "
            f"parent_draw_chain={parent_draw / 1000.0:.3f}ms "
            f"parent_draw_other={parent_other / 1000.0:.3f}ms"
        )
    return "\n".join(lines)


def summarize_aggregate(records: list[Record]) -> str:
    presented = sum(record.integer("presented") for record in records)
    sampled = sum(record.integer("sampled") for record in records)
    aggregate: list[tuple[int, int, int, int]] = []
    for priority in range(23):
        rows = [dict((row[0], row[1:]) for row in bins(record))[priority]
                for record in records]
        total = sum(row[0] for row in rows)
        maximum = max((row[1] for row in rows), default=0)
        calls = sum(row[2] for row in rows)
        aggregate.append((priority, total, maximum, calls))
    aggregate.sort(key=lambda row: row[1], reverse=True)

    lines = [
        f"aggregate windows={len(records)} sampled={sampled}/{presented}",
        "ranked aggregate callback bins (projection is an estimate):",
    ]
    for priority, total, maximum, calls in aggregate:
        if calls == 0 and total == 0:
            continue
        lines.append(
            f"  p{priority:<2} {PRIORITY_NAMES.get(priority, 'Unknown'):<41} "
            f"raw={total / 1000.0:10.3f}ms "
            f"est={projected(total, sampled, presented) / 1000.0:10.3f}ms "
            f"max={maximum / 1000.0:7.3f}ms calls={calls}"
        )
    return "\n".join(lines)


def invalid_record_lines(records: list[Record]) -> list[int]:
    invalid = []
    for record in records:
        sampled = record.integer("sampled")
        presented = record.integer("presented")
        chain_calls = record.triple("chain")[2]
        if (
            record.integer("uf") != 0
            or record.integer("cr") != 0
            or record.integer("ov") != 0
            or sampled <= 0
            or sampled > presented
            or chain_calls != sampled
        ):
            invalid.append(record.line_number)
    return invalid


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("log", type=pathlib.Path)
    args = parser.parse_args()
    text = args.log.read_text(encoding="utf-8", errors="replace")
    try:
        records = parse_records(text)
    except ValueError as error:
        print(error, file=sys.stderr)
        return 1
    if not records:
        print("no DRAW_PRIO V1 records", file=sys.stderr)
        return 2

    perf_records = parse_perf_records(text)
    print(identity_line(text))
    for index, record in enumerate(records):
        if index:
            print()
        key = (
            record.fields["st"],
            record.fields["sf"],
            record.fields["cadence_mode"],
        )
        print(summarize_record(record, perf_records.get(key)))
    print()
    print(summarize_aggregate(records))

    invalid = invalid_record_lines(records)
    if invalid:
        print(
            "invalid draw-priority records at lines: "
            + ", ".join(map(str, invalid)),
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
