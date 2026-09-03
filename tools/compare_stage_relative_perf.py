#!/usr/bin/env python3
"""Strict paired comparison for TH08 PSP stage-relative performance logs.

Positive ``improvement_ms`` means that the candidate used less wall-clock time
per frame than the baseline.  Workload mismatches are reported, never
normalized away; such a comparison is emitted with ``strict_workload_match``
false and exits with status 1.

Without ``--stage`` the historical behavior compares every parsed stage.  An
explicit stage is a fixed-route gate: unrelated later demos are ignored, while
the selected stage must contain exactly twenty 300-frame windows at
stage-frames 301 through 6001.
"""

from __future__ import annotations

import argparse
import json
import math
import random
import statistics
import sys
from collections import defaultdict
from pathlib import Path
from typing import Any, Iterable


EXPECTED_RENDER_FRAMES = 300
FIXED_STAGE_SAMPLE_FRAMES = tuple(301 + 300 * index for index in range(20))
DEFAULT_BOOTSTRAP_ITERATIONS = 20_000
DEFAULT_BOOTSTRAP_SEED = 0x54483038  # ASCII "TH08"
APPENDED_STAGE_SAMPLE_BOUNDARY = "STAGE_RELATIVE_PERF_SAMPLE"
# These fields measure the implementation mechanism under test, not the
# gameplay workload.  OFF/ON is expected to differ here even when every Bullet
# and rendered vertex is identical, so they must never make strict workload
# identity fail.  Keep the names explicit: a new diagnostic is reviewed before
# it can be excluded.
MANAGER_DIAGNOSTIC_FIELDS = frozenset(
    {
        "bullet_enum_frames",
        "bullet_enum_slot_probes",
        "bullet_enum_word_probes",
        "bullet_enum_visited",
        "bullet_enum_fallback_frames",
    }
)

# Wall-clock FPS/replay diagnostic text is sampled outside the deterministic
# game workload.  A different number of displayed digits may therefore change
# the raw submitted-vertex counter even for a perfectly matched replay.  The
# owner counters below make that exception provable instead of turning it into
# a numeric tolerance.
RAW_VERTEX_FIELDS = frozenset(
    {"render_vertices_total", "render_vertices_peak"}
)
FPS_OVERLAY_VERTEX_FIELDS = frozenset(
    {
        "render_fps_overlay_vertices_total",
        "render_fps_overlay_vertices_peak",
    }
)
GAME_VERTEX_FIELDS = frozenset(
    {"render_game_vertices_total", "render_game_vertices_peak"}
)
FPS_OVERLAY_VERTEX_ACCOUNTING_FIELDS = (
    RAW_VERTEX_FIELDS | FPS_OVERLAY_VERTEX_FIELDS | GAME_VERTEX_FIELDS
)
LOGICAL_CONTEXT_FIELDS = frozenset(
    {"replay", "demo", "is_replay", "is_demo"}
)


class ComparisonError(ValueError):
    """Input logs cannot form a valid stage-relative paired comparison."""


def parse_scalar(text: str) -> int | float | str:
    try:
        if text.startswith(("0x", "-0x")):
            return int(text, 16)
        if any(character in text for character in ".eE"):
            value = float(text)
            if not math.isfinite(value):
                raise ValueError
            return value
        return int(text)
    except ValueError:
        return text


def parse_stage_relative_samples(path: Path) -> list[dict[str, Any]]:
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError as error:
        raise ComparisonError(f"{path}: cannot read log: {error}") from error

    samples: list[dict[str, Any]] = []
    for line_number, raw_line in enumerate(lines, 1):
        # LogSnapshot uses a fixed 4096-byte formatting buffer.  A full SAMPLE
        # can therefore lose its terminating newline, after which the separate
        # stage-relative note is appended directly to the truncated tail, for
        # example ``audio_bgm_bySTAGE_RELATIVE_PERF_SAMPLE stage=5 ...``.
        # The marker is an independent record boundary, not another set of
        # SAMPLE fields.  Keep the complete prefix (including all manager and
        # render counters) and discard the appended record before tokenizing;
        # otherwise its second ``stage=`` is reported as a duplicate.
        boundary = raw_line.find(APPENDED_STAGE_SAMPLE_BOUNDARY)
        sample_text = raw_line[:boundary] if boundary >= 0 else raw_line
        fields = sample_text.split()
        if not fields or fields[0] != "SAMPLE":
            continue
        record: dict[str, Any] = {"kind": "SAMPLE", "line": line_number}
        for field in fields[1:]:
            if "=" not in field:
                continue
            name, value = field.split("=", 1)
            if name in record:
                raise ComparisonError(
                    f"{path}:{line_number}: duplicate field {name!r}"
                )
            record[name] = parse_scalar(value)
        if record.get("phase") == "stage_relative_periodic":
            samples.append(record)

    if not samples:
        raise ComparisonError(
            f"{path}: no SAMPLE phase=stage_relative_periodic records"
        )
    return samples


def _require_int(record: dict[str, Any], field: str, path: Path) -> int:
    value = record.get(field)
    if not isinstance(value, int) or isinstance(value, bool):
        raise ComparisonError(
            f"{path}:{record['line']}: {field} must be an integer, got {value!r}"
        )
    return value


def _require_fps(record: dict[str, Any], path: Path) -> float:
    value = record.get("fps")
    if not isinstance(value, (int, float)) or isinstance(value, bool):
        raise ComparisonError(
            f"{path}:{record['line']}: fps must be numeric, got {value!r}"
        )
    fps = float(value)
    if not math.isfinite(fps) or fps <= 0.0:
        raise ComparisonError(
            f"{path}:{record['line']}: fps must be finite and positive, got {value!r}"
        )
    return fps


def validate_samples(path: Path, samples: list[dict[str, Any]]) -> list[tuple[int, int]]:
    keys: list[tuple[int, int]] = []
    seen: dict[tuple[int, int], int] = {}
    stage_frames: dict[int, list[tuple[int, int]]] = defaultdict(list)

    for record in samples:
        stage = _require_int(record, "stage", path)
        stage_frame = _require_int(record, "stage_frame", path)
        render_frames = _require_int(record, "render_frames", path)
        _require_fps(record, path)
        if stage_frame <= 0:
            raise ComparisonError(
                f"{path}:{record['line']}: stage_frame must be positive, got {stage_frame}"
            )
        if render_frames != EXPECTED_RENDER_FRAMES:
            raise ComparisonError(
                f"{path}:{record['line']}: render_frames={render_frames}, "
                f"expected {EXPECTED_RENDER_FRAMES}"
            )
        key = (stage, stage_frame)
        if key in seen:
            raise ComparisonError(
                f"{path}:{record['line']}: duplicate sample stage={stage} "
                f"stage_frame={stage_frame}; first seen at line {seen[key]}"
            )
        seen[key] = int(record["line"])
        keys.append(key)
        stage_frames[stage].append((stage_frame, int(record["line"])))

    # A gap means that one or more nominal 300-stage-frame windows is absent,
    # even if both A and B logs happen to omit the same window.
    for stage, observations in stage_frames.items():
        for (previous, _), (current, line) in zip(observations, observations[1:]):
            if current - previous != EXPECTED_RENDER_FRAMES:
                raise ComparisonError(
                    f"{path}:{line}: stage={stage} sampling boundary gap "
                    f"{previous}->{current}; expected +{EXPECTED_RENDER_FRAMES}"
                )
    return keys


def select_fixed_stage_samples(
    path: Path,
    samples: list[dict[str, Any]],
    stage: int,
) -> list[dict[str, Any]]:
    """Select one stage and require its complete fixed 20-window route.

    Records belonging to later demo stages are intentionally ignored.  The
    selected stage itself remains fail-closed: it must contain exactly
    stage-frame 301 through 6001 in 300-frame steps, in that order.
    """

    if not isinstance(stage, int) or isinstance(stage, bool):
        raise ComparisonError(f"stage filter must be an integer, got {stage!r}")
    selected = [
        record for record in samples if _require_int(record, "stage", path) == stage
    ]
    if not selected:
        raise ComparisonError(f"{path}: no stage-relative samples for stage={stage}")
    keys = validate_samples(path, selected)
    expected = [(stage, stage_frame) for stage_frame in FIXED_STAGE_SAMPLE_FRAMES]
    if keys != expected:
        raise ComparisonError(
            f"{path}: stage={stage} must contain exactly fixed keys {expected}, "
            f"got {keys}"
        )
    return selected


def fps_overlay_vertex_reconciliations(
    keys: list[tuple[int, int]],
    baseline: list[dict[str, Any]],
    candidate: list[dict[str, Any]],
    baseline_path: Path,
    candidate_path: Path,
) -> list[dict[str, Any]]:
    """Prove that a raw vertex delta is owned only by the FPS overlay.

    Totals are additive over the same interval, so each run must satisfy
    ``raw_total == game_total + overlay_total``.  Peaks are independent maxima
    and may occur on different frames; only the mathematically valid max/sum
    bounds are imposed.  Across A/B, game total and peak remain exact and the
    raw-total delta must equal the overlay-total delta with no tolerance.

    Required fields are read with :func:`_require_int`, making a partial owner
    schema a hard input error rather than silently falling back to raw counts.
    """

    def values(record: dict[str, Any], path: Path) -> dict[str, int]:
        return {
            "raw_total": _require_int(record, "render_vertices_total", path),
            "raw_peak": _require_int(record, "render_vertices_peak", path),
            "overlay_total": _require_int(
                record, "render_fps_overlay_vertices_total", path
            ),
            "overlay_peak": _require_int(
                record, "render_fps_overlay_vertices_peak", path
            ),
            "game_total": _require_int(
                record, "render_game_vertices_total", path
            ),
            "game_peak": _require_int(
                record, "render_game_vertices_peak", path
            ),
            "frames": _require_int(record, "render_frames", path),
        }

    def integrity(current: dict[str, int]) -> bool:
        raw_total = current["raw_total"]
        raw_peak = current["raw_peak"]
        overlay_total = current["overlay_total"]
        overlay_peak = current["overlay_peak"]
        game_total = current["game_total"]
        game_peak = current["game_peak"]
        frames = current["frames"]
        nonnegative = all(value >= 0 for value in current.values())
        # The diagnostic owner renders canonical six-vertex glyphs.  This is
        # an ownership sanity check, not a general multiple-of-six allowance.
        overlay_quantized = overlay_total % 6 == 0 and overlay_peak % 6 == 0
        totals_reconcile = raw_total == game_total + overlay_total
        peaks_bounded = (
            max(game_peak, overlay_peak) <= raw_peak
            and raw_peak <= game_peak + overlay_peak
        )
        interval_bounds = (
            raw_total <= raw_peak * frames
            and overlay_total <= overlay_peak * frames
            and game_total <= game_peak * frames
            and ((raw_total == 0) == (raw_peak == 0))
            and ((overlay_total == 0) == (overlay_peak == 0))
            and ((game_total == 0) == (game_peak == 0))
        )
        return (
            nonnegative
            and overlay_quantized
            and totals_reconcile
            and peaks_bounded
            and interval_bounds
        )

    reconciliations: list[dict[str, Any]] = []
    for key, baseline_record, candidate_record in zip(keys, baseline, candidate):
        baseline_values = values(baseline_record, baseline_path)
        candidate_values = values(candidate_record, candidate_path)
        raw_delta = candidate_values["raw_total"] - baseline_values["raw_total"]
        overlay_delta = (
            candidate_values["overlay_total"] - baseline_values["overlay_total"]
        )
        game_exact = (
            baseline_values["game_total"] == candidate_values["game_total"]
            and baseline_values["game_peak"] == candidate_values["game_peak"]
        )
        reconciliations.append(
            {
                "stage": key[0],
                "stage_frame": key[1],
                "baseline": baseline_values,
                "candidate": candidate_values,
                "baseline_integrity": integrity(baseline_values),
                "candidate_integrity": integrity(candidate_values),
                "raw_total_delta": raw_delta,
                "overlay_total_delta": overlay_delta,
                "raw_delta_matches_overlay_delta": raw_delta == overlay_delta,
                "game_vertices_exact": game_exact,
            }
        )
    for item in reconciliations:
        item["match"] = (
            item["baseline_integrity"]
            and item["candidate_integrity"]
            and item["raw_delta_matches_overlay_delta"]
            and item["game_vertices_exact"]
        )
    return reconciliations


def _manager_field(name: str) -> bool:
    return (
        name.startswith(("enemy_", "bullet_", "laser_", "item_"))
        and name not in MANAGER_DIAGNOSTIC_FIELDS
    )


def _render_workload_field(name: str) -> bool:
    if not name.startswith("render_"):
        return False
    if name.startswith("render_arena_"):
        return False
    return name not in ("render_perf_valid", "render_frames")


def _numeric(value: Any) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def _sum_preserving_int(values: Iterable[int | float]) -> int | float:
    materialized = list(values)
    if all(isinstance(value, int) and not isinstance(value, bool) for value in materialized):
        return sum(materialized)
    return math.fsum(float(value) for value in materialized)


def compare_field(
    field: str,
    category: str,
    keys: list[tuple[int, int]],
    baseline: list[dict[str, Any]],
    candidate: list[dict[str, Any]],
) -> dict[str, Any]:
    mismatches: list[dict[str, Any]] = []
    baseline_values = [record.get(field) for record in baseline]
    candidate_values = [record.get(field) for record in candidate]

    for key, baseline_value, candidate_value in zip(
        keys, baseline_values, candidate_values
    ):
        if baseline_value != candidate_value:
            mismatches.append(
                {
                    "stage": key[0],
                    "stage_frame": key[1],
                    "baseline": baseline_value,
                    "candidate": candidate_value,
                }
            )

    summary: dict[str, Any] = {
        "category": category,
        "field": field,
        "match": not mismatches,
        "mismatched_pairs": len(mismatches),
        "mismatches": mismatches,
    }
    if all(_numeric(value) for value in baseline_values + candidate_values):
        baseline_sum = _sum_preserving_int(baseline_values)
        candidate_sum = _sum_preserving_int(candidate_values)
        deltas = [
            float(candidate_value) - float(baseline_value)
            for baseline_value, candidate_value in zip(
                baseline_values, candidate_values
            )
        ]
        delta_sum = candidate_sum - baseline_sum
        if isinstance(baseline_sum, int) and isinstance(candidate_sum, int):
            delta_sum = int(delta_sum)
        summary.update(
            {
                "baseline_sum": baseline_sum,
                "candidate_sum": candidate_sum,
                "candidate_minus_baseline": delta_sum,
                "max_abs_pair_delta": max((abs(delta) for delta in deltas), default=0.0),
            }
        )
    else:
        summary["numeric"] = False
    return summary


def percentile(sorted_values: list[float], quantile: float) -> float:
    if not sorted_values:
        raise ComparisonError("cannot calculate a percentile of no values")
    position = (len(sorted_values) - 1) * quantile
    lower = int(math.floor(position))
    upper = int(math.ceil(position))
    if lower == upper:
        return sorted_values[lower]
    fraction = position - lower
    return sorted_values[lower] * (1.0 - fraction) + sorted_values[upper] * fraction


def paired_bootstrap_ci(
    paired_improvements_ms: list[float], iterations: int, seed: int
) -> tuple[float, float]:
    if iterations <= 0:
        raise ComparisonError("bootstrap iterations must be positive")
    if not paired_improvements_ms:
        raise ComparisonError("cannot bootstrap an empty paired sample")
    rng = random.Random(seed)
    count = len(paired_improvements_ms)
    means = []
    for _ in range(iterations):
        means.append(
            math.fsum(
                paired_improvements_ms[rng.randrange(count)] for _ in range(count)
            )
            / count
        )
    means.sort()
    return percentile(means, 0.025), percentile(means, 0.975)


def compare_logs(
    baseline_path: Path,
    candidate_path: Path,
    *,
    bootstrap_iterations: int = DEFAULT_BOOTSTRAP_ITERATIONS,
    bootstrap_seed: int = DEFAULT_BOOTSTRAP_SEED,
    tie_epsilon_ms: float = 0.0,
    allow_fps_overlay_drift: bool = False,
    stage: int | None = None,
) -> dict[str, Any]:
    if tie_epsilon_ms < 0.0 or not math.isfinite(tie_epsilon_ms):
        raise ComparisonError("tie epsilon must be finite and non-negative")

    baseline = parse_stage_relative_samples(baseline_path)
    candidate = parse_stage_relative_samples(candidate_path)
    if stage is not None:
        baseline = select_fixed_stage_samples(baseline_path, baseline, stage)
        candidate = select_fixed_stage_samples(candidate_path, candidate, stage)
    baseline_keys = validate_samples(baseline_path, baseline)
    candidate_keys = validate_samples(candidate_path, candidate)

    if baseline_keys != candidate_keys:
        baseline_set = set(baseline_keys)
        candidate_set = set(candidate_keys)
        missing_from_candidate = [key for key in baseline_keys if key not in candidate_set]
        extra_in_candidate = [key for key in candidate_keys if key not in baseline_set]
        order_only = not missing_from_candidate and not extra_in_candidate
        raise ComparisonError(
            "stage/stage_frame alignment failure: "
            f"missing_from_candidate={missing_from_candidate} "
            f"extra_in_candidate={extra_in_candidate} order_only={str(order_only).lower()}"
        )

    baseline_fields = set().union(*(record.keys() for record in baseline))
    candidate_fields = set().union(*(record.keys() for record in candidate))
    all_fields = baseline_fields | candidate_fields
    manager_fields = sorted(field for field in all_fields if _manager_field(field))
    render_fields = sorted(field for field in all_fields if _render_workload_field(field))
    diagnostic_fields = sorted(all_fields & MANAGER_DIAGNOSTIC_FIELDS)
    context_fields: list[str] = []
    fps_overlay_vertex_results: list[dict[str, Any]] = []
    render_telemetry_valid = True
    if allow_fps_overlay_drift:
        # Raw and diagnostic-owner fields are judged by exact accounting below;
        # game-owned vertices deliberately stay in the strict render set.
        render_fields = [
            field
            for field in render_fields
            if field not in RAW_VERTEX_FIELDS | FPS_OVERLAY_VERTEX_FIELDS
        ]
        context_fields = sorted(all_fields & LOGICAL_CONTEXT_FIELDS)
        fps_overlay_vertex_results = fps_overlay_vertex_reconciliations(
            baseline_keys,
            baseline,
            candidate,
            baseline_path,
            candidate_path,
        )
        for baseline_record, candidate_record in zip(baseline, candidate):
            baseline_valid = _require_int(
                baseline_record, "render_perf_valid", baseline_path
            )
            candidate_valid = _require_int(
                candidate_record, "render_perf_valid", candidate_path
            )
            render_telemetry_valid = (
                render_telemetry_valid
                and baseline_valid == 1
                and candidate_valid == 1
            )
    if not manager_fields:
        raise ComparisonError("no gameplay manager count fields found")
    if not render_fields:
        raise ComparisonError("no render workload fields found")

    workload_differences = [
        compare_field(field, "manager", baseline_keys, baseline, candidate)
        for field in manager_fields
    ] + [
        compare_field(field, "context", baseline_keys, baseline, candidate)
        for field in context_fields
    ] + [
        compare_field(field, "render", baseline_keys, baseline, candidate)
        for field in render_fields
    ]
    exact_workload_match = all(item["match"] for item in workload_differences)
    fps_overlay_vertex_reconciliation_match = all(
        item["match"] for item in fps_overlay_vertex_results
    )
    strict_workload_match = (
        exact_workload_match
        and fps_overlay_vertex_reconciliation_match
        and render_telemetry_valid
    )
    raw_vertex_differences = [
        compare_field(field, "raw_render", baseline_keys, baseline, candidate)
        for field in sorted(RAW_VERTEX_FIELDS)
    ]
    render_vertices_mismatch = any(
        not item["match"] for item in raw_vertex_differences
    )

    pairs: list[dict[str, Any]] = []
    improvements: list[float] = []
    wins = ties = losses = 0
    for key, baseline_record, candidate_record in zip(
        baseline_keys, baseline, candidate
    ):
        baseline_fps = _require_fps(baseline_record, baseline_path)
        candidate_fps = _require_fps(candidate_record, candidate_path)
        baseline_ms = 1000.0 / baseline_fps
        candidate_ms = 1000.0 / candidate_fps
        improvement_ms = baseline_ms - candidate_ms
        improvements.append(improvement_ms)
        if improvement_ms > tie_epsilon_ms:
            result = "win"
            wins += 1
        elif improvement_ms < -tie_epsilon_ms:
            result = "loss"
            losses += 1
        else:
            result = "tie"
            ties += 1
        pairs.append(
            {
                "stage": key[0],
                "stage_frame": key[1],
                "baseline_fps": baseline_fps,
                "candidate_fps": candidate_fps,
                "baseline_ms": baseline_ms,
                "candidate_ms": candidate_ms,
                "improvement_ms": improvement_ms,
                "result": result,
            }
        )

    ci_low, ci_high = paired_bootstrap_ci(
        improvements, bootstrap_iterations, bootstrap_seed
    )
    baseline_fps_values = [_require_fps(record, baseline_path) for record in baseline]
    candidate_fps_values = [_require_fps(record, candidate_path) for record in candidate]
    baseline_ms_values = [1000.0 / fps for fps in baseline_fps_values]
    candidate_ms_values = [1000.0 / fps for fps in candidate_fps_values]

    return {
        "schema": "th08_stage_relative_perf_comparison_v1",
        "baseline_path": str(baseline_path),
        "candidate_path": str(candidate_path),
        "aligned": True,
        "expected_render_frames_per_sample": EXPECTED_RENDER_FRAMES,
        "sample_count": len(pairs),
        "stage_filter": stage,
        "fixed_stage_route_required": stage is not None,
        "strict_workload_match": strict_workload_match,
        "exact_game_and_logical_workload_match": exact_workload_match,
        "allow_fps_overlay_drift": allow_fps_overlay_drift,
        "render_telemetry_valid": render_telemetry_valid,
        "fps_overlay_vertex_reconciliation_match": (
            fps_overlay_vertex_reconciliation_match
        ),
        "fps_overlay_vertex_reconciliations": fps_overlay_vertex_results,
        "render_vertices_mismatch": render_vertices_mismatch,
        "render_vertices_mismatch_note": (
            "allowed only because the exact owner counters prove that the raw "
            "delta is entirely the FPS overlay while game vertices stay exact"
            if render_vertices_mismatch
            and allow_fps_overlay_drift
            and fps_overlay_vertex_reconciliation_match
            else "not ignored; use --allow-fps-overlay-drift only with complete "
            "owner counters to prove the exception"
            if render_vertices_mismatch
            else "none"
        ),
        "workload_fields": {
            "manager": manager_fields,
            "context": context_fields,
            "render": render_fields,
            "diagnostic_excluded": diagnostic_fields,
            "fps_overlay_accounting": (
                sorted(FPS_OVERLAY_VERTEX_ACCOUNTING_FIELDS)
                if allow_fps_overlay_drift
                else []
            ),
        },
        "workload_differences": workload_differences,
        "raw_vertex_differences": raw_vertex_differences,
        "timing": {
            "improvement_definition": "baseline_ms_minus_candidate_ms",
            "positive_means_candidate_faster": True,
            "baseline_fps_min": min(baseline_fps_values),
            "baseline_fps_mean": statistics.fmean(baseline_fps_values),
            "baseline_fps_max": max(baseline_fps_values),
            "candidate_fps_min": min(candidate_fps_values),
            "candidate_fps_mean": statistics.fmean(candidate_fps_values),
            "candidate_fps_max": max(candidate_fps_values),
            "baseline_ms_mean": statistics.fmean(baseline_ms_values),
            "candidate_ms_mean": statistics.fmean(candidate_ms_values),
            "paired_improvement_ms_mean": statistics.fmean(improvements),
            "wins": wins,
            "ties": ties,
            "losses": losses,
            "tie_epsilon_ms": tie_epsilon_ms,
            "bootstrap_iterations": bootstrap_iterations,
            "bootstrap_seed": bootstrap_seed,
            "bootstrap_95_low_ms": ci_low,
            "bootstrap_95_high_ms": ci_high,
        },
        "pairs": pairs,
    }


def render_text(result: dict[str, Any]) -> str:
    timing = result["timing"]
    lines = [
        f"schema={result['schema']}",
        f"baseline={result['baseline_path']}",
        f"candidate={result['candidate_path']}",
        f"aligned={str(result['aligned']).lower()} samples={result['sample_count']} "
        f"render_frames_each={result['expected_render_frames_per_sample']}",
        f"stage_filter={result['stage_filter']!r} "
        f"fixed_stage_route_required={str(result['fixed_stage_route_required']).lower()}",
        f"strict_workload_match={str(result['strict_workload_match']).lower()}",
        "exact_game_and_logical_workload_match="
        f"{str(result['exact_game_and_logical_workload_match']).lower()}",
        f"allow_fps_overlay_drift={str(result['allow_fps_overlay_drift']).lower()} "
        "fps_overlay_vertex_reconciliation_match="
        f"{str(result['fps_overlay_vertex_reconciliation_match']).lower()} "
        f"render_telemetry_valid={str(result['render_telemetry_valid']).lower()}",
        f"render_vertices_mismatch={str(result['render_vertices_mismatch']).lower()} "
        f"note={result['render_vertices_mismatch_note']}",
    ]
    for difference in result["workload_differences"]:
        line = (
            f"WORKLOAD category={difference['category']} field={difference['field']} "
            f"match={str(difference['match']).lower()} "
            f"mismatched_pairs={difference['mismatched_pairs']}"
        )
        if "baseline_sum" in difference:
            line += (
                f" baseline_sum={difference['baseline_sum']}"
                f" candidate_sum={difference['candidate_sum']}"
                f" candidate_minus_baseline={difference['candidate_minus_baseline']}"
                f" max_abs_pair_delta={difference['max_abs_pair_delta']}"
            )
        lines.append(line)
        for mismatch in difference["mismatches"]:
            lines.append(
                "  MISMATCH "
                f"stage={mismatch['stage']} stage_frame={mismatch['stage_frame']} "
                f"baseline={mismatch['baseline']!r} candidate={mismatch['candidate']!r}"
            )

    for item in result["fps_overlay_vertex_reconciliations"]:
        lines.append(
            "FPS_OVERLAY_VERTEX_ACCOUNTING "
            f"stage={item['stage']} stage_frame={item['stage_frame']} "
            f"match={str(item['match']).lower()} "
            f"baseline_integrity={str(item['baseline_integrity']).lower()} "
            f"candidate_integrity={str(item['candidate_integrity']).lower()} "
            f"game_vertices_exact={str(item['game_vertices_exact']).lower()} "
            f"raw_total_delta={item['raw_total_delta']} "
            f"overlay_total_delta={item['overlay_total_delta']} "
            "raw_delta_matches_overlay_delta="
            f"{str(item['raw_delta_matches_overlay_delta']).lower()}"
        )

    for pair in result["pairs"]:
        lines.append(
            f"PAIR stage={pair['stage']} stage_frame={pair['stage_frame']} "
            f"baseline_fps={pair['baseline_fps']:.6f} "
            f"candidate_fps={pair['candidate_fps']:.6f} "
            f"baseline_ms={pair['baseline_ms']:.9f} "
            f"candidate_ms={pair['candidate_ms']:.9f} "
            f"improvement_ms={pair['improvement_ms']:+.9f} result={pair['result']}"
        )
    lines.append(
        "TIMING "
        f"baseline_fps_min={timing['baseline_fps_min']:.6f} "
        f"baseline_fps_mean={timing['baseline_fps_mean']:.6f} "
        f"baseline_fps_max={timing['baseline_fps_max']:.6f} "
        f"candidate_fps_min={timing['candidate_fps_min']:.6f} "
        f"candidate_fps_mean={timing['candidate_fps_mean']:.6f} "
        f"candidate_fps_max={timing['candidate_fps_max']:.6f} "
        f"baseline_ms_mean={timing['baseline_ms_mean']:.9f} "
        f"candidate_ms_mean={timing['candidate_ms_mean']:.9f} "
        f"paired_improvement_ms_mean={timing['paired_improvement_ms_mean']:+.9f} "
        f"wins={timing['wins']} ties={timing['ties']} losses={timing['losses']}"
    )
    lines.append(
        "BOOTSTRAP95 "
        f"seed={timing['bootstrap_seed']} iterations={timing['bootstrap_iterations']} "
        f"low_ms={timing['bootstrap_95_low_ms']:+.9f} "
        f"high_ms={timing['bootstrap_95_high_ms']:+.9f}"
    )
    return "\n".join(lines) + "\n"


def integer_argument(text: str) -> int:
    return int(text, 0)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Strictly compare paired TH08 PSP stage-relative perf samples"
    )
    parser.add_argument("baseline_log", type=Path)
    parser.add_argument("candidate_log", type=Path)
    parser.add_argument(
        "--bootstrap-iterations", type=int, default=DEFAULT_BOOTSTRAP_ITERATIONS
    )
    parser.add_argument("--seed", type=integer_argument, default=DEFAULT_BOOTSTRAP_SEED)
    parser.add_argument("--tie-epsilon-ms", type=float, default=0.0)
    parser.add_argument(
        "--stage",
        type=int,
        help=(
            "compare only this stage and require exactly 20 samples at "
            "stage-frame 301..6001"
        ),
    )
    parser.add_argument(
        "--allow-fps-overlay-drift",
        action="store_true",
        help=(
            "allow raw vertex A/B drift only when complete owner counters prove "
            "raw=game+overlay per run and game-owned vertices remain exact"
        ),
    )
    parser.add_argument("--json", action="store_true", help="emit JSON instead of text")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)

    try:
        result = compare_logs(
            args.baseline_log,
            args.candidate_log,
            bootstrap_iterations=args.bootstrap_iterations,
            bootstrap_seed=args.seed,
            tie_epsilon_ms=args.tie_epsilon_ms,
            allow_fps_overlay_drift=args.allow_fps_overlay_drift,
            stage=args.stage,
        )
    except ComparisonError as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 2

    output = (
        json.dumps(result, ensure_ascii=False, indent=2, sort_keys=True) + "\n"
        if args.json
        else render_text(result)
    )
    if args.output is not None:
        try:
            args.output.write_text(output, encoding="utf-8")
        except OSError as error:
            print(f"ERROR: {args.output}: cannot write output: {error}", file=sys.stderr)
            return 2
    print(output, end="")
    return 0 if result["strict_workload_match"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
