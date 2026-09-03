#!/usr/bin/env python3
"""Summarize deterministic SAMPLE/MARK records from TH08PSP_MEMORY.LOG."""

from __future__ import annotations

import argparse
import statistics
from collections import defaultdict
from pathlib import Path


def parse_value(value: str) -> int | float | str:
    try:
        if value.startswith(("0x", "-0x")):
            return int(value, 16)
        if any(character in value for character in ".eE"):
            return float(value)
        return int(value)
    except ValueError:
        return value


def parse_records(path: Path) -> list[dict[str, int | float | str]]:
    records: list[dict[str, int | float | str]] = []
    for line_number, raw_line in enumerate(
        path.read_text(encoding="utf-8", errors="replace").splitlines(), 1
    ):
        fields = raw_line.split()
        if not fields or fields[0] not in ("SAMPLE", "MARK"):
            continue
        record: dict[str, int | float | str] = {
            "kind": fields[0],
            "line": line_number,
        }
        for field in fields[1:]:
            if "=" not in field:
                continue
            name, value = field.split("=", 1)
            record[name] = parse_value(value)
        records.append(record)
    if not records:
        raise ValueError(f"{path}: no SAMPLE/MARK records")
    return records


def numeric(records: list[dict[str, object]], field: str) -> list[float]:
    return [
        float(record[field])
        for record in records
        if isinstance(record.get(field), (int, float))
    ]


def integer_extreme(
    records: list[dict[str, object]], field: str, operation: str
) -> tuple[int, dict[str, object]] | None:
    candidates = [
        record
        for record in records
        if isinstance(record.get(field), (int, float))
    ]
    if not candidates:
        return None
    key = lambda record: float(record[field])
    record = min(candidates, key=key) if operation == "min" else max(candidates, key=key)
    return int(float(record[field])), record


def location(record: dict[str, object]) -> str:
    return (
        f"frame={record.get('frame', 'NA')} stage={record.get('stage', 'NA')} "
        f"stage_frame={record.get('stage_frame', 'NA')} phase={record.get('phase', 'NA')}"
    )


def emit_extreme(
    output: list[str], records: list[dict[str, object]], field: str, operation: str
) -> None:
    result = integer_extreme(records, field, operation)
    if result is None:
        return
    value, record = result
    output.append(f"{field}_{operation}={value} {location(record)}")


def summarize(path: Path) -> str:
    records = parse_records(path)
    samples = [record for record in records if record["kind"] == "SAMPLE"]
    runtime_start = next(
        (
            index
            for index, record in enumerate(records)
            if record.get("stage_pool_allocated") == 1
            or record.get("phase") == "stage_pool_arena_prepared"
        ),
        0,
    )
    runtime_records = records[runtime_start:]
    output = [
        f"path={path}",
        f"records={len(records)} samples={len(samples)} marks={len(records) - len(samples)}",
        f"runtime_records={len(runtime_records)} start_line={runtime_records[0].get('line', 'NA')}",
    ]

    fps_values = numeric(samples, "fps")
    if fps_values:
        output.append(
            "fps_samples="
            f"{len(fps_values)} min={min(fps_values):.3f} "
            f"mean={statistics.fmean(fps_values):.3f} max={max(fps_values):.3f}"
        )

    for field, operation in (
        ("heap_used_peak", "max"),
        ("heap_free", "min"),
        ("heap_largest_free", "min"),
        ("kernel_free", "min"),
        ("kernel_largest", "min"),
        ("tracked_peak", "max"),
        ("render_arena_peak", "max"),
        ("render_arena_largest", "min"),
        ("anm_scratch_capacity", "max"),
        ("stage_pool_reserved", "max"),
        ("stage_pool_transient_peak", "max"),
        ("enemy_exact_sampled_peak", "max"),
        ("bullet_exact_sampled_peak", "max"),
        ("laser_exact_sampled_peak", "max"),
        ("item_exact_sampled_peak", "max"),
    ):
        emit_extreme(output, runtime_records, field, operation)

    emit_extreme(output, runtime_records, "anm_scratch_active", "max")
    emit_extreme(output, runtime_records, "anm_scratch_busy", "max")

    failure_fields = (
        "tracked_failures",
        "render_arena_failures",
        "render_arena_quarantines",
        "render_arena_scope_contentions",
        "render_arena_poisoned",
        "ecl_child_alloc_failures",
        "ecl_child_free_failures",
        "anm_scratch_poisoned",
        "stage_pool_transient_failures",
        "stage_pool_transient_quarantines",
        "heap_geometry_errors",
    )
    maxima = []
    for field in failure_fields:
        values = numeric(records, field)
        if values:
            maxima.append(f"{field}={int(max(values))}")
    output.append("failure_maxima " + " ".join(maxima))

    by_stage: dict[int, list[dict[str, object]]] = defaultdict(list)
    for record in samples:
        stage = record.get("stage")
        if isinstance(stage, int):
            by_stage[stage].append(record)
    for stage in sorted(by_stage):
        stage_records = by_stage[stage]
        stage_fps = numeric(stage_records, "fps")
        stage_free = numeric(stage_records, "heap_free")
        stage_largest = numeric(stage_records, "heap_largest_free")
        if not stage_fps:
            continue
        output.append(
            f"stage={stage} samples={len(stage_records)} "
            f"fps_min={min(stage_fps):.3f} fps_mean={statistics.fmean(stage_fps):.3f} "
            f"fps_max={max(stage_fps):.3f} "
            f"heap_free_min={int(min(stage_free)) if stage_free else -1} "
            f"heap_largest_free_min={int(min(stage_largest)) if stage_largest else -1}"
        )

    return "\n".join(output) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("memory_log", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = summarize(args.memory_log)
    if args.output is not None:
        args.output.write_text(result, encoding="utf-8")
    print(result, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
