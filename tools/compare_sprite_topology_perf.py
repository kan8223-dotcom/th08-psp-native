#!/usr/bin/env python3
"""Strict stage-relative A/B comparator for PSP sprite-topology fast paths.

This comparator is deliberately narrower than ``compare_stage_relative_perf``.
It permits only the physical counters which can change when an already-proved
quad is submitted as two GE_SPRITES vertices instead of the control topology.
Every gameplay-manager counter and every other render-workload counter must
still match pair by pair.

The two supported mechanisms have independent, mechanically checked telemetry:

* ``item``: ITEM_TIME canonical 6V -> 2V sprite pairs;
* ``bullet``: accepted Bullet 4V/indexed quads -> mixed 2V/4V submission.

The total submitted-vertex delta is not merely allowed: it must equal the sum
of the successful per-mechanism savings for every 300-frame window.  Draw,
state, and clean upload-attempt counters are reported as implementation deltas
because their exact values depend on compatible-run boundaries.  No wildcard
or prefix-based exclusion is accepted.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import statistics
import sys
from collections import OrderedDict
from pathlib import Path
from typing import Any, Iterable

import compare_stage_relative_perf as stage_perf


SCHEMA = "th08_sprite_topology_perf_comparison_v1"

# This is the complete allowlist.  Actual texture transfers, matrices, logical
# item/effect/bullet counts, VFPU work, and every future render counter remain
# strict.  A new field needs an explicit source-level justification here.
PHYSICAL_IMPLEMENTATION_FIELDS = frozenset(
    {
        "render_draws_total",
        "render_draws_peak",
        "render_vertices_total",
        "render_vertices_peak",
        "render_state_requested_total",
        "render_state_requested_peak",
        "render_state_emitted_total",
        "render_state_emitted_peak",
        "render_upload_attempt_total",
        "render_upload_attempt_peak",
    }
)

CONTEXT_FIELDS = frozenset({"replay", "demo"})
RECORD_MARKERS = (
    "ITEM_TIME_DRAW_PAIR ",
    "BULLET_MIXED_QUADS ",
    "STAGE_RELATIVE_PERF_SAMPLE ",
    "STAGE_RELATIVE_PERF_BASELINE ",
)

ITEM_PREFIX = "ITEM_TIME_DRAW_PAIR"
BULLET_PREFIX = "BULLET_MIXED_QUADS"

ITEM_ADDITIVE_FIELDS = frozenset(
    {
        "passes",
        "candidates",
        "canonical_draws",
        "visible",
        "culled",
        "eligible_pairs",
        "compatible_runs",
        "submitted_runs",
        "submitted_pairs",
        "endpoint_matches",
        "endpoint_mismatches",
        "canonical_fallbacks",
        "owner_fallbacks",
        "load_fallbacks",
        "script_fallbacks",
        "sprite_fallbacks",
        "visibility_fallbacks",
        "rotation_fallbacks",
        "scale_fallbacks",
        "nonfinite_fallbacks",
        "texture_fallbacks",
        "state_fallbacks",
        "axis_fallbacks",
        "endpoint_fallbacks",
        "capacity_fallbacks",
        "backend_fallbacks",
        "vertices_saved",
        "frontend_bytes_saved",
        "backend_bytes_saved",
        "cache_hits",
        "cache_revalidations",
        "cache_generation_changes",
        "cache_validation_failures",
    }
)

ITEM_REQUIRED_FIELDS = ITEM_ADDITIVE_FIELDS | frozenset(
    {
        "max_run",
        "peak_candidates",
        "peak_visible",
        "peak_eligible",
        "peak_runs",
        "peak_stage_frame",
    }
)

ITEM_ZERO_FAILURE_FIELDS = frozenset(
    {
        "canonical_draws",
        "endpoint_mismatches",
        "canonical_fallbacks",
        "owner_fallbacks",
        "load_fallbacks",
        "script_fallbacks",
        "sprite_fallbacks",
        "visibility_fallbacks",
        "rotation_fallbacks",
        "scale_fallbacks",
        "nonfinite_fallbacks",
        "texture_fallbacks",
        "state_fallbacks",
        "axis_fallbacks",
        "endpoint_fallbacks",
        "capacity_fallbacks",
        "backend_fallbacks",
        "cache_generation_changes",
        "cache_validation_failures",
    }
)

BULLET_REQUIRED_FIELDS = frozenset(
    {
        "passes",
        "owner_conflict_passes",
        "state_runs",
        "batches",
        "candidates",
        "eligible_prefix_quads",
        "general_quads",
        "sticky_general_quads",
        "nonfinite_fallbacks",
        "axis_fallbacks",
        "area_or_mirror_fallbacks",
        "z_or_w_fallbacks",
        "uv_fallbacks",
        "diffuse_fallbacks",
        "submitted_batches",
        "submitted_pair_quads",
        "submitted_general_quads",
        "backend_fallback_batches",
        "fail_closed_batches",
        "missing_run_batches",
        "invalid_range_batches",
        "canonical_recovery_draw_failures",
        "canonical_recovery_quads",
        "potential_frontend_vertices_saved",
        "potential_ge_vertices_saved",
        "submitted_frontend_vertices_saved",
        "submitted_ge_vertices_saved",
        "max_pair_prefix",
        "max_general_suffix",
    }
)

BULLET_ZERO_FAILURE_FIELDS = frozenset(
    {
        "owner_conflict_passes",
        "backend_fallback_batches",
        "fail_closed_batches",
        "missing_run_batches",
        "invalid_range_batches",
        "canonical_recovery_draw_failures",
        "canonical_recovery_quads",
    }
)

BULLET_CLASSIFIER_GENERAL_FIELDS = frozenset(
    {
        "sticky_general_quads",
        "nonfinite_fallbacks",
        "axis_fallbacks",
        "area_or_mirror_fallbacks",
        "z_or_w_fallbacks",
        "uv_fallbacks",
        "diffuse_fallbacks",
    }
)


class RuleBook:
    """Collect named reconciliation failures without suppressing detail."""

    def __init__(self) -> None:
        self._rules: OrderedDict[str, dict[str, Any]] = OrderedDict()

    def check(
        self,
        name: str,
        description: str,
        condition: bool,
        *,
        key: tuple[int, int] | None = None,
        observed: Any = None,
        expected: Any = None,
    ) -> None:
        rule = self._rules.setdefault(
            name, {"rule": name, "description": description, "violations": []}
        )
        if rule["description"] != description:
            raise AssertionError(f"rule {name!r} has inconsistent descriptions")
        if condition:
            return
        violation: dict[str, Any] = {"observed": observed, "expected": expected}
        if key is not None:
            violation["stage"], violation["stage_frame"] = key
        rule["violations"].append(violation)

    def finish(self) -> list[dict[str, Any]]:
        result: list[dict[str, Any]] = []
        for value in self._rules.values():
            item = dict(value)
            item["passed"] = not item["violations"]
            item["violation_count"] = len(item["violations"])
            result.append(item)
        return result


def _record_slice(raw_line: str, marker: str) -> str | None:
    start = raw_line.find(marker)
    if start < 0:
        return None
    end = len(raw_line)
    search_from = start + len(marker)
    for other in RECORD_MARKERS:
        position = raw_line.find(other, search_from)
        if position >= 0:
            end = min(end, position)
    return raw_line[start:end]


def parse_auxiliary_records(path: Path, prefix: str) -> list[dict[str, Any]]:
    """Parse one diagnostic record, including after a truncated SAMPLE line."""

    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError as error:
        raise stage_perf.ComparisonError(f"{path}: cannot read log: {error}") from error

    marker = prefix + " "
    records: list[dict[str, Any]] = []
    for line_number, raw_line in enumerate(lines, 1):
        record_text = _record_slice(raw_line, marker)
        if record_text is None:
            continue
        fields = record_text.split()
        if not fields or fields[0] != prefix:
            continue
        record: dict[str, Any] = {"record": prefix, "line": line_number}
        for field in fields[1:]:
            if "=" not in field:
                continue
            name, value = field.split("=", 1)
            if name in record:
                raise stage_perf.ComparisonError(
                    f"{path}:{line_number}: duplicate {prefix} field {name!r}"
                )
            record[name] = stage_perf.parse_scalar(value)
        records.append(record)
    return records


def _require_int(record: dict[str, Any], field: str, path: Path) -> int:
    return stage_perf._require_int(record, field, path)


def _require_aux_fields(
    path: Path, records: Iterable[dict[str, Any]], fields: frozenset[str]
) -> None:
    for record in records:
        for field in fields:
            _require_int(record, field, path)


def _sample_auxiliary_by_key(
    path: Path,
    records: list[dict[str, Any]],
    expected_keys: list[tuple[int, int]],
    required_fields: frozenset[str],
) -> dict[tuple[int, int], dict[str, Any]]:
    samples: dict[tuple[int, int], dict[str, Any]] = {}
    for record in records:
        if record.get("kind") != "SAMPLE" or record.get("phase") != "stage_relative_periodic":
            continue
        if record.get("mode") != "product":
            raise stage_perf.ComparisonError(
                f"{path}:{record['line']}: {record['record']} mode must be product"
            )
        _require_aux_fields(path, (record,), required_fields)
        key = (_require_int(record, "stage", path), _require_int(record, "stage_frame", path))
        if key in samples:
            raise stage_perf.ComparisonError(
                f"{path}:{record['line']}: duplicate {record['record']} sample {key}"
            )
        samples[key] = record
    if list(samples) != expected_keys:
        raise stage_perf.ComparisonError(
            f"{path}: auxiliary {records[0]['record'] if records else 'record'} "
            f"keys {list(samples)} do not exactly match SAMPLE keys {expected_keys}"
        )
    return samples


def _find_item_zero_origin(
    path: Path,
    records: list[dict[str, Any]],
    stage: int,
    before_line: int,
) -> dict[str, int]:
    candidates = []
    for record in records:
        if record["line"] >= before_line or record.get("mode") != "product":
            continue
        if record.get("kind") != "MARK" or record.get("stage") != stage:
            continue
        if record.get("stage_frame") != 0:
            continue
        try:
            zero = all(_require_int(record, field, path) == 0 for field in ITEM_ADDITIVE_FIELDS)
        except stage_perf.ComparisonError:
            continue
        if zero:
            candidates.append(record)
    if not candidates:
        raise stage_perf.ComparisonError(
            f"{path}: no zero ITEM_TIME_DRAW_PAIR MARK origin before stage {stage} samples"
        )
    return {field: 0 for field in ITEM_ADDITIVE_FIELDS}


def _item_intervals(
    path: Path,
    records: list[dict[str, Any]],
    samples: dict[tuple[int, int], dict[str, Any]],
    keys: list[tuple[int, int]],
) -> dict[tuple[int, int], dict[str, int]]:
    result: dict[tuple[int, int], dict[str, int]] = {}
    previous_by_stage: dict[int, dict[str, Any]] = {}
    for key in keys:
        current = samples[key]
        previous = previous_by_stage.get(key[0])
        if previous is None:
            previous = _find_item_zero_origin(path, records, key[0], int(current["line"]))
        interval: dict[str, int] = {}
        for field in ITEM_ADDITIVE_FIELDS:
            current_value = _require_int(current, field, path)
            previous_value = int(previous[field])
            if current_value < previous_value:
                raise stage_perf.ComparisonError(
                    f"{path}:{current['line']}: ITEM_TIME_DRAW_PAIR {field} "
                    f"decreased {previous_value}->{current_value} within stage {key[0]}"
                )
            interval[field] = current_value - previous_value
        result[key] = interval
        previous_by_stage[key[0]] = current
    return result


def _check_item_interval(
    rules: RuleBook, key: tuple[int, int], values: dict[str, int], render_frames: int
) -> int:
    failures = {field: values[field] for field in sorted(ITEM_ZERO_FAILURE_FIELDS) if values[field] != 0}
    rules.check(
        "item_failures_zero",
        "ITEM_TIME product must have no canonical, validation, capacity, or backend fallback",
        not failures,
        key=key,
        observed=failures,
        expected="all zero",
    )
    rules.check(
        "item_pass_count",
        "ITEM_TIME draw pass count must equal the measured render-frame count",
        values["passes"] == render_frames,
        key=key,
        observed=values["passes"],
        expected=render_frames,
    )
    rules.check(
        "item_candidate_partition",
        "every candidate must be either visible/eligible or canonically culled",
        values["candidates"] == values["visible"] + values["culled"]
        and values["visible"] == values["eligible_pairs"],
        key=key,
        observed={
            "candidates": values["candidates"],
            "visible": values["visible"],
            "culled": values["culled"],
            "eligible_pairs": values["eligible_pairs"],
        },
        expected="candidates=visible+culled and visible=eligible_pairs",
    )
    rules.check(
        "item_submission_accounting",
        "every eligible pair and compatible run must reach the product backend",
        values["submitted_pairs"] == values["eligible_pairs"]
        and values["submitted_runs"] == values["compatible_runs"],
        key=key,
        observed={
            "eligible_pairs": values["eligible_pairs"],
            "submitted_pairs": values["submitted_pairs"],
            "compatible_runs": values["compatible_runs"],
            "submitted_runs": values["submitted_runs"],
        },
        expected="submitted_pairs=eligible_pairs; submitted_runs=compatible_runs",
    )
    saved = values["submitted_pairs"] * 4
    rules.check(
        "item_savings_accounting",
        "each submitted ITEM_TIME pair removes four 28-byte frontend and four 24-byte backend vertices",
        values["vertices_saved"] == saved
        and values["frontend_bytes_saved"] == saved * 28
        and values["backend_bytes_saved"] == saved * 24,
        key=key,
        observed={
            "vertices_saved": values["vertices_saved"],
            "frontend_bytes_saved": values["frontend_bytes_saved"],
            "backend_bytes_saved": values["backend_bytes_saved"],
        },
        expected={
            "vertices_saved": saved,
            "frontend_bytes_saved": saved * 28,
            "backend_bytes_saved": saved * 24,
        },
    )
    rules.check(
        "item_cache_accounting",
        "each candidate must use exactly one cache hit or revalidation",
        values["cache_hits"] + values["cache_revalidations"] == values["candidates"],
        key=key,
        observed={
            "hits": values["cache_hits"],
            "revalidations": values["cache_revalidations"],
            "candidates": values["candidates"],
        },
        expected="hits+revalidations=candidates",
    )
    return saved


def _check_bullet_interval(
    rules: RuleBook, key: tuple[int, int], values: dict[str, Any], render_frames: int
) -> int:
    failures = {
        field: values[field]
        for field in sorted(BULLET_ZERO_FAILURE_FIELDS)
        if values[field] != 0
    }
    rules.check(
        "bullet_failures_zero",
        "Bullet mixed product must have no owner conflict, backend fallback, recovery, or fail-close",
        not failures,
        key=key,
        observed=failures,
        expected="all zero",
    )
    rules.check(
        "bullet_pass_count",
        "Bullet mixed pass count must equal the measured render-frame count",
        values["passes"] == render_frames,
        key=key,
        observed=values["passes"],
        expected=render_frames,
    )
    classifier_general = sum(values[field] for field in BULLET_CLASSIFIER_GENERAL_FIELDS)
    rules.check(
        "bullet_candidate_partition",
        "every Bullet quad must be one eligible prefix quad or one classified/sticky general quad",
        values["candidates"]
        == values["eligible_prefix_quads"] + values["general_quads"]
        and values["general_quads"] == classifier_general,
        key=key,
        observed={
            "candidates": values["candidates"],
            "eligible_prefix_quads": values["eligible_prefix_quads"],
            "general_quads": values["general_quads"],
            "classifier_general_sum": classifier_general,
        },
        expected="candidates=eligible+general; general=sum(classifier buckets)",
    )
    rules.check(
        "bullet_submission_accounting",
        "every staged batch and quad must reach the mixed product backend",
        values["submitted_batches"] == values["batches"]
        and values["submitted_pair_quads"] == values["eligible_prefix_quads"]
        and values["submitted_general_quads"] == values["general_quads"],
        key=key,
        observed={
            "batches": values["batches"],
            "submitted_batches": values["submitted_batches"],
            "eligible": values["eligible_prefix_quads"],
            "submitted_pairs": values["submitted_pair_quads"],
            "general": values["general_quads"],
            "submitted_general": values["submitted_general_quads"],
        },
        expected="all staged batches/quads submitted",
    )
    potential_frontend = values["eligible_prefix_quads"] * 2
    potential_ge = values["eligible_prefix_quads"] * 4
    submitted_frontend = values["submitted_pair_quads"] * 2
    submitted_ge = values["submitted_pair_quads"] * 4
    rules.check(
        "bullet_savings_accounting",
        "mixed diagnostics must exactly encode 4V->2V frontend and 6-index->2V GE savings",
        values["potential_frontend_vertices_saved"] == potential_frontend
        and values["potential_ge_vertices_saved"] == potential_ge
        and values["submitted_frontend_vertices_saved"] == submitted_frontend
        and values["submitted_ge_vertices_saved"] == submitted_ge,
        key=key,
        observed={
            "potential_frontend": values["potential_frontend_vertices_saved"],
            "potential_ge": values["potential_ge_vertices_saved"],
            "submitted_frontend": values["submitted_frontend_vertices_saved"],
            "submitted_ge": values["submitted_ge_vertices_saved"],
        },
        expected={
            "potential_frontend": potential_frontend,
            "potential_ge": potential_ge,
            "submitted_frontend": submitted_frontend,
            "submitted_ge": submitted_ge,
        },
    )
    return submitted_ge


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as source:
            for block in iter(lambda: source.read(1024 * 1024), b""):
                digest.update(block)
    except OSError as error:
        raise stage_perf.ComparisonError(f"{path}: cannot hash identity artifact: {error}") from error
    return digest.hexdigest()


def _identity_results(identity_pairs: list[tuple[str, Path, Path]]) -> list[dict[str, Any]]:
    seen: set[str] = set()
    result = []
    for label, baseline, candidate in identity_pairs:
        if label in seen:
            raise stage_perf.ComparisonError(f"duplicate identity label {label!r}")
        seen.add(label)
        baseline_sha = _sha256(baseline)
        candidate_sha = _sha256(candidate)
        result.append(
            {
                "label": label,
                "baseline_path": str(baseline),
                "candidate_path": str(candidate),
                "baseline_sha256": baseline_sha,
                "candidate_sha256": candidate_sha,
                "match": baseline_sha == candidate_sha,
            }
        )
    return result


def _compare_selected_timing(
    baseline_path: Path,
    candidate_path: Path,
    keys: list[tuple[int, int]],
    baseline: list[dict[str, Any]],
    candidate: list[dict[str, Any]],
    *,
    bootstrap_iterations: int,
    bootstrap_seed: int,
    tie_epsilon_ms: float,
) -> tuple[bool, dict[str, Any], list[dict[str, Any]]]:
    if tie_epsilon_ms < 0.0 or not math.isfinite(tie_epsilon_ms):
        raise stage_perf.ComparisonError("tie epsilon must be finite and non-negative")
    all_fields = set().union(*(record.keys() for record in baseline + candidate))
    raw_fields = sorted(
        field
        for field in all_fields
        if stage_perf._manager_field(field) or stage_perf._render_workload_field(field)
    )
    raw_differences = [
        stage_perf.compare_field(field, "raw_workload", keys, baseline, candidate)
        for field in raw_fields
    ]
    general_strict = all(difference["match"] for difference in raw_differences)

    pairs: list[dict[str, Any]] = []
    improvements: list[float] = []
    baseline_fps_values: list[float] = []
    candidate_fps_values: list[float] = []
    wins = ties = losses = 0
    for key, baseline_record, candidate_record in zip(keys, baseline, candidate):
        baseline_fps = stage_perf._require_fps(baseline_record, baseline_path)
        candidate_fps = stage_perf._require_fps(candidate_record, candidate_path)
        baseline_fps_values.append(baseline_fps)
        candidate_fps_values.append(candidate_fps)
        baseline_ms = 1000.0 / baseline_fps
        candidate_ms = 1000.0 / candidate_fps
        improvement_ms = baseline_ms - candidate_ms
        improvements.append(improvement_ms)
        if improvement_ms > tie_epsilon_ms:
            verdict = "win"
            wins += 1
        elif improvement_ms < -tie_epsilon_ms:
            verdict = "loss"
            losses += 1
        else:
            verdict = "tie"
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
                "result": verdict,
            }
        )
    ci_low, ci_high = stage_perf.paired_bootstrap_ci(
        improvements, bootstrap_iterations, bootstrap_seed
    )
    baseline_ms_values = [1000.0 / value for value in baseline_fps_values]
    candidate_ms_values = [1000.0 / value for value in candidate_fps_values]
    timing = {
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
    }
    return general_strict, timing, pairs


def compare_logs(
    baseline_path: Path,
    candidate_path: Path,
    *,
    mechanisms: frozenset[str],
    bootstrap_iterations: int = stage_perf.DEFAULT_BOOTSTRAP_ITERATIONS,
    bootstrap_seed: int = stage_perf.DEFAULT_BOOTSTRAP_SEED,
    tie_epsilon_ms: float = 0.0,
    identity_pairs: list[tuple[str, Path, Path]] | None = None,
    windows: frozenset[tuple[int, int]] | None = None,
) -> dict[str, Any]:
    if not mechanisms or not mechanisms <= {"item", "bullet"}:
        raise stage_perf.ComparisonError("mechanisms must contain item and/or bullet")

    baseline_all = stage_perf.parse_stage_relative_samples(baseline_path)
    candidate_all = stage_perf.parse_stage_relative_samples(candidate_path)
    all_keys = stage_perf.validate_samples(baseline_path, baseline_all)
    candidate_keys = stage_perf.validate_samples(candidate_path, candidate_all)
    if all_keys != candidate_keys:
        raise stage_perf.ComparisonError(
            f"stage/stage_frame alignment failure: baseline={all_keys} candidate={candidate_keys}"
        )
    if windows is not None:
        unknown = sorted(windows - set(all_keys))
        if unknown:
            raise stage_perf.ComparisonError(f"requested windows are absent: {unknown}")
        keys = [key for key in all_keys if key in windows]
    else:
        keys = list(all_keys)
    if not keys:
        raise stage_perf.ComparisonError("no stage-relative windows selected")
    baseline_lookup = dict(zip(all_keys, baseline_all))
    candidate_lookup = dict(zip(all_keys, candidate_all))
    baseline = [baseline_lookup[key] for key in keys]
    candidate = [candidate_lookup[key] for key in keys]

    general_strict_workload_match, timing, pairs = _compare_selected_timing(
        baseline_path,
        candidate_path,
        keys,
        baseline,
        candidate,
        bootstrap_iterations=bootstrap_iterations,
        bootstrap_seed=bootstrap_seed,
        tie_epsilon_ms=tie_epsilon_ms,
    )

    for record in baseline:
        for field in PHYSICAL_IMPLEMENTATION_FIELDS | CONTEXT_FIELDS:
            _require_int(record, field, baseline_path)
    for record in candidate:
        for field in PHYSICAL_IMPLEMENTATION_FIELDS | CONTEXT_FIELDS:
            _require_int(record, field, candidate_path)

    all_fields = set().union(*(record.keys() for record in baseline + candidate))
    manager_fields = sorted(field for field in all_fields if stage_perf._manager_field(field))
    render_fields = sorted(field for field in all_fields if stage_perf._render_workload_field(field))
    logical_render_fields = [field for field in render_fields if field not in PHYSICAL_IMPLEMENTATION_FIELDS]
    logical_differences = [
        stage_perf.compare_field(field, "manager", keys, baseline, candidate)
        for field in manager_fields
    ] + [
        stage_perf.compare_field(field, "logical_render", keys, baseline, candidate)
        for field in logical_render_fields
    ] + [
        stage_perf.compare_field(field, "context", keys, baseline, candidate)
        for field in sorted(CONTEXT_FIELDS)
    ]
    logical_workload_match = all(difference["match"] for difference in logical_differences)

    rules = RuleBook()
    for key, baseline_record, candidate_record in zip(keys, baseline, candidate):
        rules.check(
            "replay_workload",
            "both runs must be replay-driven and have identical replay/demo context",
            baseline_record["replay"] == candidate_record["replay"] == 1
            and baseline_record["demo"] == candidate_record["demo"],
            key=key,
            observed={
                "baseline_replay": baseline_record["replay"],
                "candidate_replay": candidate_record["replay"],
                "baseline_demo": baseline_record["demo"],
                "candidate_demo": candidate_record["demo"],
            },
            expected="replay=1 on both; demo identical",
        )

    baseline_item = parse_auxiliary_records(baseline_path, ITEM_PREFIX)
    baseline_bullet = parse_auxiliary_records(baseline_path, BULLET_PREFIX)
    candidate_item = parse_auxiliary_records(candidate_path, ITEM_PREFIX)
    candidate_bullet = parse_auxiliary_records(candidate_path, BULLET_PREFIX)
    if baseline_item or baseline_bullet:
        raise stage_perf.ComparisonError(
            f"{baseline_path}: control must have ITEM_TIME_DRAW_PAIR and BULLET_MIXED_QUADS both OFF"
        )
    if "item" not in mechanisms and candidate_item:
        raise stage_perf.ComparisonError(f"{candidate_path}: unselected ITEM_TIME_DRAW_PAIR is enabled")
    if "bullet" not in mechanisms and candidate_bullet:
        raise stage_perf.ComparisonError(f"{candidate_path}: unselected BULLET_MIXED_QUADS is enabled")

    item_samples: dict[tuple[int, int], dict[str, Any]] = {}
    item_intervals: dict[tuple[int, int], dict[str, int]] = {}
    bullet_samples: dict[tuple[int, int], dict[str, Any]] = {}
    if "item" in mechanisms:
        if not candidate_item:
            raise stage_perf.ComparisonError(f"{candidate_path}: ITEM_TIME_DRAW_PAIR records missing")
        item_samples = _sample_auxiliary_by_key(
            candidate_path, candidate_item, all_keys, ITEM_REQUIRED_FIELDS
        )
        item_intervals = _item_intervals(
            candidate_path, candidate_item, item_samples, all_keys
        )
    if "bullet" in mechanisms:
        if not candidate_bullet:
            raise stage_perf.ComparisonError(f"{candidate_path}: BULLET_MIXED_QUADS records missing")
        bullet_samples = _sample_auxiliary_by_key(
            candidate_path, candidate_bullet, all_keys, BULLET_REQUIRED_FIELDS
        )

    physical_differences = [
        stage_perf.compare_field(field, "physical_implementation", keys, baseline, candidate)
        for field in sorted(PHYSICAL_IMPLEMENTATION_FIELDS)
    ]
    vertex_reconciliations = []
    exercised = {mechanism: 0 for mechanism in mechanisms}
    for key, baseline_record, candidate_record in zip(keys, baseline, candidate):
        expected_saved = 0
        if "item" in mechanisms:
            item_saved = _check_item_interval(
                rules, key, item_intervals[key], candidate_record["render_frames"]
            )
            expected_saved += item_saved
            exercised["item"] += item_intervals[key]["submitted_pairs"]
        if "bullet" in mechanisms:
            bullet_record = bullet_samples[key]
            bullet_saved = _check_bullet_interval(
                rules, key, bullet_record, candidate_record["render_frames"]
            )
            expected_saved += bullet_saved
            exercised["bullet"] += bullet_record["submitted_pair_quads"]

        observed_saved = baseline_record["render_vertices_total"] - candidate_record["render_vertices_total"]
        peak_nonincrease = candidate_record["render_vertices_peak"] <= baseline_record["render_vertices_peak"]
        match = observed_saved == expected_saved and peak_nonincrease
        vertex_reconciliations.append(
            {
                "stage": key[0],
                "stage_frame": key[1],
                "baseline_vertices_total": baseline_record["render_vertices_total"],
                "candidate_vertices_total": candidate_record["render_vertices_total"],
                "observed_vertices_saved": observed_saved,
                "expected_vertices_saved": expected_saved,
                "baseline_vertices_peak": baseline_record["render_vertices_peak"],
                "candidate_vertices_peak": candidate_record["render_vertices_peak"],
                "match": match,
            }
        )
        rules.check(
            "submitted_vertex_delta_exact",
            "the physical vertex reduction must equal successful ITEM_TIME plus Bullet GE savings",
            observed_saved == expected_saved,
            key=key,
            observed=observed_saved,
            expected=expected_saved,
        )
        rules.check(
            "submitted_vertex_peak_nonincrease",
            "a topology-only 2V path must not increase the maximum submitted vertices in a frame",
            peak_nonincrease,
            key=key,
            observed=candidate_record["render_vertices_peak"],
            expected=f"<= {baseline_record['render_vertices_peak']}",
        )

        # A product ITEM_TIME run replaces its canonical draw and can split
        # the surrounding compatible stream once on each edge.  Bullet mixed
        # submission keeps one RenderPerf draw per pre-existing state batch,
        # so it contributes no additional frontend draw boundary.
        item_run_total = (
            item_intervals[key]["submitted_runs"] if "item" in mechanisms else 0
        )
        item_run_peak = (
            _require_int(item_samples[key], "peak_runs", candidate_path)
            if "item" in mechanisms
            else 0
        )
        draw_delta = (
            candidate_record["render_draws_total"]
            - baseline_record["render_draws_total"]
        )
        draw_peak_delta = (
            candidate_record["render_draws_peak"]
            - baseline_record["render_draws_peak"]
        )
        draw_upper = item_run_total * 2
        draw_peak_upper = item_run_peak * 2
        rules.check(
            "draw_split_bound",
            "only the two edges of each submitted ITEM_TIME run may add draw boundaries",
            0 <= draw_delta <= draw_upper,
            key=key,
            observed=draw_delta,
            expected=f"0..{draw_upper}",
        )
        rules.check(
            "draw_peak_split_bound",
            "the interval draw peak may grow only within the cumulative ITEM_TIME per-frame run bound",
            0 <= draw_peak_delta <= draw_peak_upper,
            key=key,
            observed=draw_peak_delta,
            expected=f"0..{draw_peak_upper}",
        )

        requested_delta = (
            candidate_record["render_state_requested_total"]
            - baseline_record["render_state_requested_total"]
        )
        requested_peak_delta = (
            candidate_record["render_state_requested_peak"]
            - baseline_record["render_state_requested_peak"]
        )
        rules.check(
            "state_requested_total_exact",
            "each added textured draw has exactly two texture-stage setters and one FVF setter",
            requested_delta == draw_delta * 3,
            key=key,
            observed=requested_delta,
            expected=draw_delta * 3,
        )
        rules.check(
            "state_requested_peak_bound",
            "frontend state-request peak growth is bounded by three requests per possible split draw",
            0 <= requested_peak_delta <= draw_peak_upper * 3,
            key=key,
            observed=requested_peak_delta,
            expected=f"0..{draw_peak_upper * 3}",
        )

        bullet_submission_total = (
            _require_int(bullet_samples[key], "submitted_batches", candidate_path)
            if "bullet" in mechanisms
            else 0
        )
        replacement_submissions = item_run_total + bullet_submission_total
        emitted_delta = (
            candidate_record["render_state_emitted_total"]
            - baseline_record["render_state_emitted_total"]
        )
        emitted_peak_delta = (
            candidate_record["render_state_emitted_peak"]
            - baseline_record["render_state_emitted_peak"]
        )
        rules.check(
            "state_emitted_total_bound",
            "each replacement or split submission may change at most ten cached PSPGL/client-array calls",
            -replacement_submissions * 10
            <= emitted_delta
            <= (replacement_submissions + draw_delta) * 10,
            key=key,
            observed=emitted_delta,
            expected=(
                f"{-replacement_submissions * 10}.."
                f"{(replacement_submissions + draw_delta) * 10}"
            ),
        )
        rules.check(
            "state_emitted_peak_bound",
            "state-emission peak growth is bounded by the measured replacement submissions and possible splits",
            -replacement_submissions * 10
            <= emitted_peak_delta
            <= (replacement_submissions + draw_peak_upper) * 10,
            key=key,
            observed=emitted_peak_delta,
            expected=(
                f"{-replacement_submissions * 10}.."
                f"{(replacement_submissions + draw_peak_upper) * 10}"
            ),
        )

        upload_delta = (
            candidate_record["render_upload_attempt_total"]
            - baseline_record["render_upload_attempt_total"]
        )
        upload_peak_delta = (
            candidate_record["render_upload_attempt_peak"]
            - baseline_record["render_upload_attempt_peak"]
        )
        rules.check(
            "clean_upload_attempt_total_exact",
            "each added textured draw performs exactly one clean texture-upload entry attempt",
            upload_delta == draw_delta,
            key=key,
            observed=upload_delta,
            expected=draw_delta,
        )
        rules.check(
            "clean_upload_attempt_peak_bound",
            "clean upload-attempt peak growth is bounded by the possible split draws",
            0 <= upload_peak_delta <= draw_peak_upper,
            key=key,
            observed=upload_peak_delta,
            expected=f"0..{draw_peak_upper}",
        )

    for mechanism, count in sorted(exercised.items()):
        rules.check(
            f"{mechanism}_mechanism_exercised",
            f"the selected {mechanism} product path must submit at least one 2V quad",
            count > 0,
            observed=count,
            expected="> 0",
        )

    reconciliations = rules.finish()
    mechanism_reconciliation_match = all(rule["passed"] for rule in reconciliations)
    identity = _identity_results(identity_pairs or [])
    identity_gate = all(item["match"] for item in identity)
    identity_match: bool | None = identity_gate if identity else None
    comparison_valid = logical_workload_match and mechanism_reconciliation_match and identity_gate

    return {
        "schema": SCHEMA,
        "baseline_path": str(baseline_path),
        "candidate_path": str(candidate_path),
        "mechanisms": sorted(mechanisms),
        "sample_count": len(keys),
        "selected_windows": [
            {"stage": stage, "stage_frame": stage_frame}
            for stage, stage_frame in keys
        ],
        "general_strict_workload_match": general_strict_workload_match,
        "logical_workload_match": logical_workload_match,
        "mechanism_reconciliation_match": mechanism_reconciliation_match,
        "comparison_valid": comparison_valid,
        "physical_implementation_allowlist": sorted(PHYSICAL_IMPLEMENTATION_FIELDS),
        "logical_workload_fields": {
            "manager": manager_fields,
            "render": logical_render_fields,
            "context": sorted(CONTEXT_FIELDS),
        },
        "logical_workload_differences": logical_differences,
        "physical_implementation_differences": physical_differences,
        "vertex_reconciliations": vertex_reconciliations,
        "reconciliations": reconciliations,
        "mechanism_submitted_2v_quads": exercised,
        "artifact_identity_checked": bool(identity),
        "artifact_identity_match": identity_match,
        "artifact_identity": identity,
        "artifact_identity_note": (
            "all supplied artifacts are byte-identical"
            if identity
            else "not checked; telemetry cannot prove pixel or replay-byte identity"
        ),
        "timing": timing,
        "pairs": pairs,
    }


def render_text(result: dict[str, Any]) -> str:
    lines = [
        f"schema={result['schema']}",
        f"baseline={result['baseline_path']}",
        f"candidate={result['candidate_path']}",
        f"mechanisms={','.join(result['mechanisms'])} samples={result['sample_count']}",
        f"general_strict_workload_match={str(result['general_strict_workload_match']).lower()}",
        f"logical_workload_match={str(result['logical_workload_match']).lower()}",
        f"mechanism_reconciliation_match={str(result['mechanism_reconciliation_match']).lower()}",
        f"artifact_identity_checked={str(result['artifact_identity_checked']).lower()} "
        f"artifact_identity_match="
        f"{('NA' if result['artifact_identity_match'] is None else str(result['artifact_identity_match']).lower())} "
        f"note={result['artifact_identity_note']}",
        f"comparison_valid={str(result['comparison_valid']).lower()}",
    ]
    for difference in result["logical_workload_differences"]:
        if difference["match"]:
            continue
        lines.append(
            f"LOGICAL_MISMATCH category={difference['category']} field={difference['field']} "
            f"pairs={difference['mismatched_pairs']}"
        )
        for mismatch in difference["mismatches"]:
            lines.append(
                f"  stage={mismatch['stage']} stage_frame={mismatch['stage_frame']} "
                f"baseline={mismatch['baseline']!r} candidate={mismatch['candidate']!r}"
            )
    for difference in result["physical_implementation_differences"]:
        lines.append(
            f"PHYSICAL field={difference['field']} match={str(difference['match']).lower()} "
            f"baseline_sum={difference.get('baseline_sum', 'NA')} "
            f"candidate_sum={difference.get('candidate_sum', 'NA')} "
            f"candidate_minus_baseline={difference.get('candidate_minus_baseline', 'NA')}"
        )
    for rule in result["reconciliations"]:
        lines.append(
            f"RECONCILE rule={rule['rule']} passed={str(rule['passed']).lower()} "
            f"violations={rule['violation_count']} description={rule['description']}"
        )
        for violation in rule["violations"]:
            location = ""
            if "stage" in violation:
                location = f" stage={violation['stage']} stage_frame={violation['stage_frame']}"
            lines.append(
                f"  VIOLATION{location} observed={violation['observed']!r} "
                f"expected={violation['expected']!r}"
            )
    for item in result["artifact_identity"]:
        lines.append(
            f"IDENTITY label={item['label']} match={str(item['match']).lower()} "
            f"baseline_sha256={item['baseline_sha256']} candidate_sha256={item['candidate_sha256']}"
        )
    for pair in result["pairs"]:
        lines.append(
            f"PAIR stage={pair['stage']} stage_frame={pair['stage_frame']} "
            f"baseline_fps={pair['baseline_fps']:.6f} candidate_fps={pair['candidate_fps']:.6f} "
            f"improvement_ms={pair['improvement_ms']:+.9f} result={pair['result']}"
        )
    timing = result["timing"]
    lines.append(
        "TIMING "
        f"baseline_fps_mean={timing['baseline_fps_mean']:.6f} "
        f"candidate_fps_mean={timing['candidate_fps_mean']:.6f} "
        f"paired_improvement_ms_mean={timing['paired_improvement_ms_mean']:+.9f} "
        f"wins={timing['wins']} ties={timing['ties']} losses={timing['losses']}"
    )
    lines.append(
        "BOOTSTRAP95 "
        f"seed={timing['bootstrap_seed']} iterations={timing['bootstrap_iterations']} "
        f"low_ms={timing['bootstrap_95_low_ms']:+.9f} high_ms={timing['bootstrap_95_high_ms']:+.9f}"
    )
    return "\n".join(lines) + "\n"


def integer_argument(text: str) -> int:
    return int(text, 0)


def window_argument(text: str) -> tuple[int, int]:
    try:
        stage_text, frame_text = text.split(":", 1)
        stage = int(stage_text, 0)
        stage_frame = int(frame_text, 0)
    except ValueError as error:
        raise argparse.ArgumentTypeError("window must be STAGE:STAGE_FRAME") from error
    if stage < 0 or stage_frame <= 0:
        raise argparse.ArgumentTypeError("window stage must be >=0 and stage_frame >0")
    return stage, stage_frame


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Strictly reconcile ITEM_TIME/Bullet 2V topology stage-relative A/B logs"
    )
    parser.add_argument("baseline_log", type=Path)
    parser.add_argument("candidate_log", type=Path)
    parser.add_argument(
        "--mechanism",
        action="append",
        choices=("item", "bullet"),
        required=True,
        help="selected product mechanism; repeat for a combined candidate",
    )
    parser.add_argument(
        "--identity-pair",
        action="append",
        nargs=3,
        metavar=("LABEL", "BASELINE", "CANDIDATE"),
        default=[],
        help="require a replay/surface/other artifact pair to be byte-identical",
    )
    parser.add_argument(
        "--window",
        action="append",
        type=window_argument,
        default=[],
        metavar="STAGE:STAGE_FRAME",
        help=(
            "compare only this pre-registered 300-frame window end; repeat for "
            "an explicit set (default: every aligned window)"
        ),
    )
    parser.add_argument(
        "--bootstrap-iterations",
        type=int,
        default=stage_perf.DEFAULT_BOOTSTRAP_ITERATIONS,
    )
    parser.add_argument("--seed", type=integer_argument, default=stage_perf.DEFAULT_BOOTSTRAP_SEED)
    parser.add_argument("--tie-epsilon-ms", type=float, default=0.0)
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)

    identity_pairs = [
        (label, Path(baseline), Path(candidate))
        for label, baseline, candidate in args.identity_pair
    ]
    try:
        result = compare_logs(
            args.baseline_log,
            args.candidate_log,
            mechanisms=frozenset(args.mechanism),
            bootstrap_iterations=args.bootstrap_iterations,
            bootstrap_seed=args.seed,
            tie_epsilon_ms=args.tie_epsilon_ms,
            identity_pairs=identity_pairs,
            windows=frozenset(args.window) if args.window else None,
        )
    except stage_perf.ComparisonError as error:
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
    return 0 if result["comparison_valid"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
