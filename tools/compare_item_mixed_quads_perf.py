#!/usr/bin/env python3
"""Strict Stage 5 OFF/ON gate for the PSP all-Item mixed-quad product.

The candidate may change only the proven presentation topology:

* canonical Item quads use six submitted vertices;
* eligible mixed-prefix quads use two vertices (four GE vertices saved);
* general suffix quads retain six GE indices and save no GE vertices;
* each accepted native mixed batch bypasses exactly nine public client-array
  state calls.

Everything else is exact.  The fixed route is twenty 300-frame Stage 5
windows.  Acceptance also requires byte-identical replay and surface artifacts,
zero frontend/backend fallback, zero shared-arena exhaustion, a one-million
sample paired bootstrap, and a strictly positive lower confidence bound.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from collections import OrderedDict
from pathlib import Path
from typing import Any, Iterable

import compare_stage_relative_perf as stage_perf


SCHEMA = "th08_item_mixed_quads_perf_comparison_v1"
ITEM_PREFIX = "ITEM_MIXED_QUADS"
BACKEND_PREFIX = "MIXED_GE_BACKEND_TELEMETRY"
BULLET_PREFIX = "BULLET_MIXED_QUADS"
EXPECTED_KEYS = tuple((5, 301 + 300 * index) for index in range(20))
EXPECTED_TEARDOWN_STAGE_FRAME = 6119
REQUIRED_BOOTSTRAP_ITERATIONS = 1_000_000

CONTEXT_FIELDS = frozenset({"replay", "demo"})
RECONCILED_RENDER_FIELDS = frozenset(
    {
        "render_vertices_total",
        "render_vertices_peak",
        "render_state_emitted_total",
        "render_state_emitted_peak",
    }
)

ITEM_REQUIRED_FIELDS = frozenset(
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

ITEM_ZERO_FAILURE_FIELDS = frozenset(
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

ITEM_CLASSIFIER_GENERAL_FIELDS = frozenset(
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

BACKEND_COUNTER_FIELDS = (
    "bullet_attempts",
    "bullet_submitted_batches",
    "bullet_submitted_quads",
    "bullet_fallbacks",
    "bullet_arena_exhaustions",
    "item_attempts",
    "item_submitted_batches",
    "item_submitted_quads",
    "item_fallbacks",
    "item_arena_exhaustions",
)
BACKEND_BULLET_FIELDS = BACKEND_COUNTER_FIELDS[:5]
BACKEND_ITEM_FIELDS = BACKEND_COUNTER_FIELDS[5:]
BACKEND_GAUGE_FIELDS = (
    "shared_arena_high_water_vertices",
    "shared_arena_capacity_vertices",
)


class RuleBook:
    """Collect all hard-gate failures so a NO-GO remains auditable."""

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
            raise AssertionError(f"inconsistent description for rule {name!r}")
        if condition:
            return
        violation: dict[str, Any] = {"observed": observed, "expected": expected}
        if key is not None:
            violation["stage"], violation["stage_frame"] = key
        rule["violations"].append(violation)

    def finish(self) -> list[dict[str, Any]]:
        result = []
        for rule in self._rules.values():
            item = dict(rule)
            item["passed"] = not item["violations"]
            item["violation_count"] = len(item["violations"])
            result.append(item)
        return result


def parse_prefixed_records(path: Path, prefix: str) -> list[dict[str, Any]]:
    """Parse a telemetry record even when appended to a truncated SAMPLE."""

    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError as error:
        raise stage_perf.ComparisonError(f"{path}: cannot read log: {error}") from error

    marker = prefix + " "
    records: list[dict[str, Any]] = []
    for line_number, raw_line in enumerate(lines, 1):
        start = raw_line.find(marker)
        if start < 0:
            continue
        fields = raw_line[start:].split()
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


def _require_fields(
    path: Path, records: Iterable[dict[str, Any]], fields: Iterable[str]
) -> None:
    for record in records:
        for field in fields:
            _require_int(record, field, path)


def _validate_fixed_route(
    baseline_path: Path,
    candidate_path: Path,
    baseline: list[dict[str, Any]],
    candidate: list[dict[str, Any]],
) -> list[tuple[int, int]]:
    expected = list(EXPECTED_KEYS)
    baseline_keys = stage_perf.validate_samples(baseline_path, baseline)
    candidate_keys = stage_perf.validate_samples(candidate_path, candidate)
    if baseline_keys != expected:
        raise stage_perf.ComparisonError(
            f"{baseline_path}: expected exact Stage 5 keys {expected}, got {baseline_keys}"
        )
    if candidate_keys != expected:
        raise stage_perf.ComparisonError(
            f"{candidate_path}: expected exact Stage 5 keys {expected}, got {candidate_keys}"
        )
    return expected


def _sample_records_by_key(
    path: Path,
    records: list[dict[str, Any]],
    keys: list[tuple[int, int]],
    required_fields: Iterable[str],
    *,
    require_product: bool = False,
) -> dict[tuple[int, int], dict[str, Any]]:
    samples: dict[tuple[int, int], dict[str, Any]] = {}
    for record in records:
        if record.get("kind") != "SAMPLE" or record.get("phase") != "stage_relative_periodic":
            continue
        if record.get("stage") != 5:
            continue
        if require_product and record.get("mode") != "product":
            raise stage_perf.ComparisonError(
                f"{path}:{record['line']}: {record['record']} mode must be product"
            )
        _require_fields(path, (record,), required_fields)
        key = (_require_int(record, "stage", path), _require_int(record, "stage_frame", path))
        if key in samples:
            raise stage_perf.ComparisonError(
                f"{path}:{record['line']}: duplicate {record['record']} sample {key}"
            )
        samples[key] = record
    if list(samples) != keys:
        raise stage_perf.ComparisonError(
            f"{path}: {records[0]['record'] if records else 'telemetry'} keys "
            f"{list(samples)} do not exactly match {keys}"
        )
    return samples


def _one_teardown(
    path: Path,
    records: list[dict[str, Any]],
    required_fields: Iterable[str],
    *,
    require_product: bool = False,
) -> dict[str, Any]:
    matches = [
        record
        for record in records
        if record.get("kind") == "MARK"
        and record.get("phase") == "stage_teardown_complete"
        and record.get("stage") == 5
        and record.get("stage_frame") == EXPECTED_TEARDOWN_STAGE_FRAME
    ]
    if len(matches) != 1:
        raise stage_perf.ComparisonError(
            f"{path}: expected one {records[0]['record'] if records else 'telemetry'} "
            f"Stage 5 frame {EXPECTED_TEARDOWN_STAGE_FRAME} teardown, got {len(matches)}"
        )
    record = matches[0]
    if require_product and record.get("mode") != "product":
        raise stage_perf.ComparisonError(
            f"{path}:{record['line']}: teardown mode must be product"
        )
    _require_fields(path, (record,), required_fields)
    return record


def _frontend_values(record: dict[str, Any], path: Path) -> dict[str, int]:
    values = {field: _require_int(record, field, path) for field in ITEM_REQUIRED_FIELDS}
    if any(value < 0 for value in values.values()):
        raise stage_perf.ComparisonError(
            f"{path}:{record['line']}: negative Item frontend counter"
        )
    return values


def _check_frontend_interval(
    rules: RuleBook,
    key: tuple[int, int],
    values: dict[str, int],
    expected_passes: int,
) -> dict[str, int]:
    failures = {
        field: values[field]
        for field in sorted(ITEM_ZERO_FAILURE_FIELDS)
        if values[field] != 0
    }
    rules.check(
        "item_frontend_failures_zero",
        "Item frontend owner conflicts, backend fallback, fail-close, and recovery must all be zero",
        not failures,
        key=key,
        observed=failures,
        expected="all zero",
    )
    rules.check(
        "item_pass_count",
        "the Item owner bracket must execute once per measured frame",
        values["passes"] == expected_passes,
        key=key,
        observed=values["passes"],
        expected=expected_passes,
    )
    classifier_general = sum(values[field] for field in ITEM_CLASSIFIER_GENERAL_FIELDS)
    rules.check(
        "item_candidate_partition",
        "every Item quad is one eligible prefix quad or one classified/sticky general quad",
        values["candidates"]
        == values["eligible_prefix_quads"] + values["general_quads"]
        and values["general_quads"] == classifier_general,
        key=key,
        observed={
            "candidates": values["candidates"],
            "eligible": values["eligible_prefix_quads"],
            "general": values["general_quads"],
            "classifier_general": classifier_general,
        },
        expected="candidates=eligible+general; general=sum(classifier buckets)",
    )
    rules.check(
        "item_submission_accounting",
        "every Item frontend batch and quad must reach the mixed backend",
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
        expected="all staged batches and quads submitted",
    )
    potential_frontend = (
        values["eligible_prefix_quads"] * 4 + values["general_quads"] * 2
    )
    potential_ge = values["eligible_prefix_quads"] * 4
    submitted_frontend = (
        values["submitted_pair_quads"] * 4
        + values["submitted_general_quads"] * 2
    )
    submitted_ge = values["submitted_pair_quads"] * 4
    rules.check(
        "item_savings_accounting",
        "Item savings must use canonical 6V authority: pair +4/+4, general +2/+0 frontend/GE",
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
    rules.check(
        "item_batch_shape",
        "state runs cannot exceed physical batches and maxima cannot exceed submitted partitions",
        values["state_runs"] <= values["batches"]
        and values["max_pair_prefix"] <= values["submitted_pair_quads"]
        and values["max_general_suffix"] <= values["submitted_general_quads"],
        key=key,
        observed={
            "state_runs": values["state_runs"],
            "batches": values["batches"],
            "max_pair_prefix": values["max_pair_prefix"],
            "submitted_pairs": values["submitted_pair_quads"],
            "max_general_suffix": values["max_general_suffix"],
            "submitted_general": values["submitted_general_quads"],
        },
        expected="state_runs<=batches; maxima<=submitted partitions",
    )
    return {
        "batches": values["submitted_batches"],
        "pairs": values["submitted_pair_quads"],
        "general": values["submitted_general_quads"],
        "quads": values["submitted_pair_quads"] + values["submitted_general_quads"],
        "frontend_saved": submitted_frontend,
        "ge_saved": submitted_ge,
    }


def _backend_metadata_valid(record: dict[str, Any]) -> bool:
    return (
        record.get("valid") == 1
        and record.get("counter_scope") == "device_lifetime"
        and record.get("cumulative") == 1
        and record.get("counter_bits") == "32_wrap"
    )


def _backend_values(record: dict[str, Any], path: Path) -> dict[str, int]:
    fields = BACKEND_COUNTER_FIELDS + BACKEND_GAUGE_FIELDS
    values = {field: _require_int(record, field, path) for field in fields}
    if any(value < 0 for value in values.values()):
        raise stage_perf.ComparisonError(
            f"{path}:{record['line']}: negative mixed-backend counter"
        )
    return values


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as source:
            for block in iter(lambda: source.read(1024 * 1024), b""):
                digest.update(block)
    except OSError as error:
        raise stage_perf.ComparisonError(f"{path}: cannot hash artifact: {error}") from error
    return digest.hexdigest()


def _artifact_results(
    replay_pair: tuple[Path, Path] | None,
    surface_pairs: list[tuple[str, Path, Path]] | None,
) -> list[dict[str, Any]]:
    if replay_pair is None:
        raise stage_perf.ComparisonError("a replay artifact pair is required")
    if not surface_pairs:
        raise stage_perf.ComparisonError("at least one surface artifact pair is required")
    pairs = [("replay", replay_pair[0], replay_pair[1]), *(surface_pairs or [])]
    seen: set[str] = set()
    result = []
    for label, baseline, candidate in pairs:
        if not label or label in seen:
            raise stage_perf.ComparisonError(f"duplicate/empty artifact label {label!r}")
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


def _field_differences(
    fields: Iterable[str],
    category: str,
    keys: list[tuple[int, int]],
    baseline: list[dict[str, Any]],
    candidate: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    return [
        stage_perf.compare_field(field, category, keys, baseline, candidate)
        for field in sorted(fields)
    ]


def compare_logs(
    baseline_path: Path,
    candidate_path: Path,
    *,
    replay_pair: tuple[Path, Path] | None,
    surface_pairs: list[tuple[str, Path, Path]] | None,
    bootstrap_iterations: int = REQUIRED_BOOTSTRAP_ITERATIONS,
    bootstrap_seed: int = stage_perf.DEFAULT_BOOTSTRAP_SEED,
    tie_epsilon_ms: float = 0.0,
) -> dict[str, Any]:
    baseline = stage_perf.parse_stage_relative_samples(baseline_path)
    candidate = stage_perf.parse_stage_relative_samples(candidate_path)
    baseline = stage_perf.select_fixed_stage_samples(
        baseline_path, baseline, 5
    )
    candidate = stage_perf.select_fixed_stage_samples(
        candidate_path, candidate, 5
    )
    keys = _validate_fixed_route(baseline_path, candidate_path, baseline, candidate)
    general = stage_perf.compare_logs(
        baseline_path,
        candidate_path,
        bootstrap_iterations=bootstrap_iterations,
        bootstrap_seed=bootstrap_seed,
        tie_epsilon_ms=tie_epsilon_ms,
        stage=5,
    )

    required_general = (
        CONTEXT_FIELDS
        | RECONCILED_RENDER_FIELDS
        | frozenset({"render_perf_valid", "render_frames"})
    )
    _require_fields(baseline_path, baseline, required_general)
    _require_fields(candidate_path, candidate, required_general)

    all_fields = set().union(*(record.keys() for record in baseline + candidate))
    manager_fields = {field for field in all_fields if stage_perf._manager_field(field)}
    manager_diagnostics = all_fields & stage_perf.MANAGER_DIAGNOSTIC_FIELDS
    exact_render_fields = {
        field
        for field in all_fields
        if stage_perf._render_workload_field(field)
        and field not in RECONCILED_RENDER_FIELDS
    }
    if not manager_fields:
        raise stage_perf.ComparisonError("no gameplay manager fields found")
    if not exact_render_fields:
        raise stage_perf.ComparisonError("no exact render workload fields found")
    workload_differences = (
        _field_differences(manager_fields, "manager", keys, baseline, candidate)
        + _field_differences(
            manager_diagnostics, "manager_diagnostic", keys, baseline, candidate
        )
        + _field_differences(CONTEXT_FIELDS, "context", keys, baseline, candidate)
        + _field_differences(
            exact_render_fields, "render_exact", keys, baseline, candidate
        )
    )
    exact_workload_match = all(item["match"] for item in workload_differences)

    baseline_frontend = parse_prefixed_records(baseline_path, ITEM_PREFIX)
    baseline_backend = parse_prefixed_records(baseline_path, BACKEND_PREFIX)
    baseline_bullet = parse_prefixed_records(baseline_path, BULLET_PREFIX)
    if baseline_frontend or baseline_backend or baseline_bullet:
        raise stage_perf.ComparisonError(
            f"{baseline_path}: OFF control contains mixed-quad telemetry"
        )
    candidate_bullet = parse_prefixed_records(candidate_path, BULLET_PREFIX)
    if candidate_bullet:
        raise stage_perf.ComparisonError(
            f"{candidate_path}: isolated Item product contains Bullet mixed telemetry"
        )
    candidate_frontend = parse_prefixed_records(candidate_path, ITEM_PREFIX)
    candidate_backend = parse_prefixed_records(candidate_path, BACKEND_PREFIX)
    if not candidate_frontend or not candidate_backend:
        raise stage_perf.ComparisonError(
            f"{candidate_path}: Item frontend/backend telemetry is missing"
        )

    frontend_samples = _sample_records_by_key(
        candidate_path,
        candidate_frontend,
        keys,
        ITEM_REQUIRED_FIELDS,
        require_product=True,
    )
    frontend_teardown = _one_teardown(
        candidate_path,
        candidate_frontend,
        ITEM_REQUIRED_FIELDS,
        require_product=True,
    )
    backend_samples = _sample_records_by_key(
        candidate_path,
        candidate_backend,
        keys,
        (*BACKEND_COUNTER_FIELDS, *BACKEND_GAUGE_FIELDS, "valid", "cumulative"),
    )
    backend_teardown = _one_teardown(
        candidate_path,
        candidate_backend,
        (*BACKEND_COUNTER_FIELDS, *BACKEND_GAUGE_FIELDS, "valid", "cumulative"),
    )
    first_sample_line = int(candidate[0]["line"])
    origins = [
        record
        for record in candidate_backend
        if int(record["line"]) < first_sample_line
        and record.get("kind") == "MARK"
        and record.get("stage") == 5
        and record.get("stage_frame") == 0
    ]
    if not origins:
        raise stage_perf.ComparisonError(
            f"{candidate_path}: no Stage 5 stage_frame=0 mixed-backend origin"
        )
    backend_origin = origins[-1]
    _require_fields(
        candidate_path,
        (backend_origin,),
        (*BACKEND_COUNTER_FIELDS, *BACKEND_GAUGE_FIELDS, "valid", "cumulative"),
    )

    rules = RuleBook()
    frontend_intervals: dict[tuple[int, int], dict[str, int]] = {}
    for key, baseline_record, candidate_record in zip(keys, baseline, candidate):
        rules.check(
            "render_telemetry_valid",
            "both render snapshots must be complete 300-frame intervals",
            baseline_record["render_perf_valid"] == 1
            and candidate_record["render_perf_valid"] == 1
            and baseline_record["render_frames"] == 300
            and candidate_record["render_frames"] == 300,
            key=key,
            observed={
                "baseline_valid": baseline_record["render_perf_valid"],
                "candidate_valid": candidate_record["render_perf_valid"],
                "baseline_frames": baseline_record["render_frames"],
                "candidate_frames": candidate_record["render_frames"],
            },
            expected="valid=1 and render_frames=300 on both",
        )
        rules.check(
            "replay_context",
            "both inputs must be replay-driven with identical demo context",
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
        front_record = frontend_samples[key]
        rules.check(
            "item_frontend_record_alignment",
            "Item frontend SAMPLE must describe the same frame and follow its general snapshot",
            front_record.get("frame") == candidate_record.get("frame")
            and int(front_record["line"]) in (
                int(candidate_record["line"]),
                int(candidate_record["line"]) + 1,
            ),
            key=key,
            observed={
                "frontend_line": front_record["line"],
                "sample_line": candidate_record["line"],
                "frontend_frame": front_record.get("frame"),
                "sample_frame": candidate_record.get("frame"),
            },
            expected="same frame; same physical line or immediately following",
        )
        values = _frontend_values(front_record, candidate_path)
        frontend_intervals[key] = _check_frontend_interval(
            rules, key, values, candidate_record["render_frames"]
        )

    tail_key = (5, EXPECTED_TEARDOWN_STAGE_FRAME)
    tail_values = _frontend_values(frontend_teardown, candidate_path)
    tail_interval = _check_frontend_interval(
        rules,
        tail_key,
        tail_values,
        EXPECTED_TEARDOWN_STAGE_FRAME - EXPECTED_KEYS[-1][1],
    )

    previous_backend = _backend_values(backend_origin, candidate_path)
    rules.check(
        "backend_origin_metadata",
        "the pre-window backend origin must be a valid cumulative device-lifetime snapshot",
        _backend_metadata_valid(backend_origin),
        observed={
            field: backend_origin.get(field)
            for field in ("valid", "counter_scope", "cumulative", "counter_bits")
        },
        expected={
            "valid": 1,
            "counter_scope": "device_lifetime",
            "cumulative": 1,
            "counter_bits": "32_wrap",
        },
    )

    backend_intervals = []
    backend_sequence = [
        *((key, backend_samples[key], frontend_intervals[key], candidate[index])
          for index, key in enumerate(keys)),
        (tail_key, backend_teardown, tail_interval, None),
    ]
    total_item_batches = 0
    total_item_pairs = 0
    for key, backend_record, front, general_record in backend_sequence:
        values = _backend_values(backend_record, candidate_path)
        rules.check(
            "backend_metadata",
            "every backend snapshot must be valid cumulative device-lifetime telemetry",
            _backend_metadata_valid(backend_record),
            key=key,
            observed={
                field: backend_record.get(field)
                for field in ("valid", "counter_scope", "cumulative", "counter_bits")
            },
            expected={
                "valid": 1,
                "counter_scope": "device_lifetime",
                "cumulative": 1,
                "counter_bits": "32_wrap",
            },
        )
        if general_record is not None:
            rules.check(
                "backend_record_alignment",
                "mixed backend SAMPLE must be immediately before the same-frame general snapshot",
                int(backend_record["line"]) + 1 == int(general_record["line"])
                and backend_record.get("frame") == general_record.get("frame"),
                key=key,
                observed={
                    "backend_line": backend_record["line"],
                    "sample_line": general_record["line"],
                    "backend_frame": backend_record.get("frame"),
                    "sample_frame": general_record.get("frame"),
                },
                expected="backend line immediately before SAMPLE with identical frame",
            )
        monotonic = all(
            values[field] >= previous_backend[field]
            for field in BACKEND_COUNTER_FIELDS
        )
        rules.check(
            "backend_counters_no_wrap",
            "the bounded Stage 5 route must not wrap cumulative backend counters",
            monotonic,
            key=key,
            observed={
                "previous": {field: previous_backend[field] for field in BACKEND_COUNTER_FIELDS},
                "current": {field: values[field] for field in BACKEND_COUNTER_FIELDS},
            },
            expected="all cumulative counters nondecreasing",
        )
        interval = {
            field: values[field] - previous_backend[field]
            for field in BACKEND_COUNTER_FIELDS
        }
        rules.check(
            "bullet_backend_owner_quiescent",
            "the isolated Item A/B must never touch Bullet mixed-backend counters",
            all(values[field] == 0 for field in BACKEND_BULLET_FIELDS),
            key=key,
            observed={field: values[field] for field in BACKEND_BULLET_FIELDS},
            expected="all zero",
        )
        rules.check(
            "item_backend_failures_zero",
            "Item backend fallback and shared-arena exhaustion are immediate NO-GO",
            values["item_fallbacks"] == 0
            and values["item_arena_exhaustions"] == 0,
            key=key,
            observed={
                "item_fallbacks": values["item_fallbacks"],
                "item_arena_exhaustions": values["item_arena_exhaustions"],
            },
            expected={"item_fallbacks": 0, "item_arena_exhaustions": 0},
        )
        rules.check(
            "item_backend_cumulative_accounting",
            "device-lifetime Item attempts must equal submits plus fallback",
            values["item_attempts"]
            == values["item_submitted_batches"] + values["item_fallbacks"],
            key=key,
            observed={field: values[field] for field in BACKEND_ITEM_FIELDS},
            expected="item_attempts=item_submitted_batches+item_fallbacks",
        )
        rules.check(
            "item_backend_matches_frontend",
            "each frontend batch/quad must map to one owner-specific backend submit",
            interval["item_attempts"] == front["batches"]
            and interval["item_submitted_batches"] == front["batches"]
            and interval["item_submitted_quads"] == front["quads"]
            and interval["item_fallbacks"] == 0
            and interval["item_arena_exhaustions"] == 0,
            key=key,
            observed={
                "backend_interval": {
                    field: interval[field] for field in BACKEND_ITEM_FIELDS
                },
                "frontend": front,
            },
            expected={
                "item_attempts": front["batches"],
                "item_submitted_batches": front["batches"],
                "item_submitted_quads": front["quads"],
                "item_fallbacks": 0,
                "item_arena_exhaustions": 0,
            },
        )
        capacity = values["shared_arena_capacity_vertices"]
        high_water = values["shared_arena_high_water_vertices"]
        rules.check(
            "shared_arena_bounds",
            "shared native arena capacity is stable and high-water never exceeds it",
            capacity > 0
            and capacity == previous_backend["shared_arena_capacity_vertices"]
            and previous_backend["shared_arena_high_water_vertices"]
            <= high_water
            <= capacity,
            key=key,
            observed={
                "previous_high_water": previous_backend[
                    "shared_arena_high_water_vertices"
                ],
                "high_water": high_water,
                "previous_capacity": previous_backend[
                    "shared_arena_capacity_vertices"
                ],
                "capacity": capacity,
            },
            expected="capacity stable and positive; previous<=high_water<=capacity",
        )
        backend_intervals.append(
            {
                "stage": key[0],
                "stage_frame": key[1],
                **{field: interval[field] for field in BACKEND_ITEM_FIELDS},
                "shared_arena_high_water_vertices": high_water,
                "shared_arena_capacity_vertices": capacity,
            }
        )
        total_item_batches += front["batches"]
        total_item_pairs += front["pairs"]
        previous_backend = values

    rules.check(
        "item_product_exercised",
        "the measured route must contain accepted Item batches and eligible 2V prefix quads",
        total_item_batches > 0 and total_item_pairs > 0,
        observed={"batches": total_item_batches, "pairs": total_item_pairs},
        expected="both >0",
    )

    render_reconciliations = []
    for key, baseline_record, candidate_record in zip(keys, baseline, candidate):
        front = frontend_intervals[key]
        vertices_saved = (
            baseline_record["render_vertices_total"]
            - candidate_record["render_vertices_total"]
        )
        vertex_peak_saved = (
            baseline_record["render_vertices_peak"]
            - candidate_record["render_vertices_peak"]
        )
        emitted_saved = (
            baseline_record["render_state_emitted_total"]
            - candidate_record["render_state_emitted_total"]
        )
        emitted_peak_saved = (
            baseline_record["render_state_emitted_peak"]
            - candidate_record["render_state_emitted_peak"]
        )
        rules.check(
            "render_vertex_total_exact",
            "the only GE vertex delta is four vertices per submitted Item prefix pair",
            vertices_saved == front["ge_saved"],
            key=key,
            observed=vertices_saved,
            expected=front["ge_saved"],
        )
        rules.check(
            "render_vertex_peak_bound",
            "Item topology may only reduce the per-frame GE vertex peak within interval savings",
            0 <= vertex_peak_saved <= front["ge_saved"],
            key=key,
            observed=vertex_peak_saved,
            expected=f"0..{front['ge_saved']}",
        )
        expected_emitted_saved = front["batches"] * 9
        rules.check(
            "render_state_emitted_total_exact",
            "each accepted Item native batch bypasses exactly nine client-array state calls",
            emitted_saved == expected_emitted_saved,
            key=key,
            observed=emitted_saved,
            expected=expected_emitted_saved,
        )
        rules.check(
            "render_state_emitted_peak_bound",
            "the state-emission peak may only fall within nine calls per interval Item batch",
            0 <= emitted_peak_saved <= expected_emitted_saved,
            key=key,
            observed=emitted_peak_saved,
            expected=f"0..{expected_emitted_saved}",
        )
        render_reconciliations.append(
            {
                "stage": key[0],
                "stage_frame": key[1],
                "observed_vertices_saved": vertices_saved,
                "expected_vertices_saved": front["ge_saved"],
                "observed_vertex_peak_saved": vertex_peak_saved,
                "observed_state_emitted_saved": emitted_saved,
                "expected_state_emitted_saved": expected_emitted_saved,
                "observed_state_emitted_peak_saved": emitted_peak_saved,
            }
        )

    artifacts = _artifact_results(replay_pair, surface_pairs)
    artifact_identity_match = all(item["match"] for item in artifacts)
    replay_identity_match = artifacts[0]["match"]
    surface_identity_match = all(item["match"] for item in artifacts[1:])
    rules.check(
        "replay_artifact_identity",
        "baseline and candidate replay artifacts must be byte-identical",
        replay_identity_match,
        observed=artifacts[0],
        expected="matching SHA-256",
    )
    rules.check(
        "surface_artifact_identity",
        "every supplied baseline/candidate surface must be byte-identical",
        surface_identity_match,
        observed=[item for item in artifacts[1:] if not item["match"]],
        expected="all matching SHA-256",
    )

    reconciliations = rules.finish()
    reconciliation_match = all(rule["passed"] for rule in reconciliations)
    comparison_valid = (
        exact_workload_match and reconciliation_match and artifact_identity_match
    )
    timing = general["timing"]
    performance_gate_passed = (
        timing["paired_improvement_ms_mean"] > 0.0
        and timing["bootstrap_95_low_ms"] > 0.0
    )
    bootstrap_iterations_valid = (
        timing["bootstrap_iterations"] == REQUIRED_BOOTSTRAP_ITERATIONS
    )
    acceptance_passed = (
        comparison_valid
        and bootstrap_iterations_valid
        and performance_gate_passed
    )
    failed_rules = [rule["rule"] for rule in reconciliations if not rule["passed"]]
    if not exact_workload_match:
        failed_rules.insert(0, "exact_workload_match")

    return {
        "schema": SCHEMA,
        "baseline_path": str(baseline_path),
        "candidate_path": str(candidate_path),
        "expected_route": {
            "stage": 5,
            "first_stage_frame": EXPECTED_KEYS[0][1],
            "last_stage_frame": EXPECTED_KEYS[-1][1],
            "step": 300,
            "sample_count": len(EXPECTED_KEYS),
            "teardown_stage_frame": EXPECTED_TEARDOWN_STAGE_FRAME,
        },
        "aligned": True,
        "sample_count": len(keys),
        "comparison_valid": comparison_valid,
        "exact_workload_match": exact_workload_match,
        "reconciliation_match": reconciliation_match,
        "artifact_identity_match": artifact_identity_match,
        "replay_identity_match": replay_identity_match,
        "surface_identity_match": surface_identity_match,
        "general_strict_workload_match": general["strict_workload_match"],
        "general_strict_note": (
            "the generic comparator cannot reconcile the intentional vertex and "
            "nine-state-call reductions; every other mismatch remains strict here"
        ),
        "bootstrap_iterations_required": REQUIRED_BOOTSTRAP_ITERATIONS,
        "bootstrap_iterations_valid": bootstrap_iterations_valid,
        "performance_gate_passed": performance_gate_passed,
        "acceptance_passed": acceptance_passed,
        "verdict": "GO" if acceptance_passed else "NO-GO",
        "hard_gate_failures": failed_rules,
        "workload_fields": {
            "manager": sorted(manager_fields),
            "manager_diagnostic": sorted(manager_diagnostics),
            "context": sorted(CONTEXT_FIELDS),
            "render_exact": sorted(exact_render_fields),
            "render_reconciled": sorted(RECONCILED_RENDER_FIELDS),
        },
        "workload_differences": workload_differences,
        "frontend_intervals": [
            {"stage": key[0], "stage_frame": key[1], **frontend_intervals[key]}
            for key in keys
        ],
        "frontend_teardown_interval": tail_interval,
        "backend_intervals": backend_intervals,
        "render_reconciliations": render_reconciliations,
        "artifacts": artifacts,
        "reconciliations": reconciliations,
        "timing": timing,
        "pairs": general["pairs"],
    }


def render_text(result: dict[str, Any]) -> str:
    timing = result["timing"]
    lines = [
        f"schema={result['schema']}",
        f"baseline={result['baseline_path']}",
        f"candidate={result['candidate_path']}",
        f"aligned={str(result['aligned']).lower()} samples={result['sample_count']} "
        "stage=5 stage_frames=301..6001 step=300",
        f"comparison_valid={str(result['comparison_valid']).lower()}",
        f"exact_workload_match={str(result['exact_workload_match']).lower()}",
        f"reconciliation_match={str(result['reconciliation_match']).lower()}",
        f"artifact_identity_match={str(result['artifact_identity_match']).lower()}",
        f"bootstrap_iterations_valid={str(result['bootstrap_iterations_valid']).lower()} "
        f"required={result['bootstrap_iterations_required']}",
        f"performance_gate_passed={str(result['performance_gate_passed']).lower()}",
        f"acceptance_passed={str(result['acceptance_passed']).lower()} "
        f"verdict={result['verdict']}",
    ]
    for difference in result["workload_differences"]:
        lines.append(
            f"WORKLOAD category={difference['category']} field={difference['field']} "
            f"match={str(difference['match']).lower()} "
            f"mismatched_pairs={difference['mismatched_pairs']}"
        )
    for artifact in result["artifacts"]:
        lines.append(
            f"ARTIFACT label={artifact['label']} match={str(artifact['match']).lower()} "
            f"baseline_sha256={artifact['baseline_sha256']} "
            f"candidate_sha256={artifact['candidate_sha256']}"
        )
    for rule in result["reconciliations"]:
        lines.append(
            f"RECONCILE rule={rule['rule']} passed={str(rule['passed']).lower()} "
            f"violations={rule['violation_count']} description={rule['description']}"
        )
    for pair in result["pairs"]:
        lines.append(
            f"PAIR stage={pair['stage']} stage_frame={pair['stage_frame']} "
            f"baseline_fps={pair['baseline_fps']:.6f} "
            f"candidate_fps={pair['candidate_fps']:.6f} "
            f"improvement_ms={pair['improvement_ms']:+.9f} result={pair['result']}"
        )
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
        f"low_ms={timing['bootstrap_95_low_ms']:+.9f} "
        f"high_ms={timing['bootstrap_95_high_ms']:+.9f}"
    )
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Strictly gate the TH08 PSP all-Item mixed-quad OFF/ON A/B"
    )
    parser.add_argument("baseline_log", type=Path)
    parser.add_argument("candidate_log", type=Path)
    parser.add_argument(
        "--replay-pair",
        nargs=2,
        type=Path,
        metavar=("BASELINE", "CANDIDATE"),
        required=True,
    )
    parser.add_argument(
        "--surface-pair",
        nargs=3,
        action="append",
        metavar=("LABEL", "BASELINE", "CANDIDATE"),
        required=True,
    )
    parser.add_argument(
        "--bootstrap-iterations",
        type=int,
        default=REQUIRED_BOOTSTRAP_ITERATIONS,
    )
    parser.add_argument(
        "--seed",
        type=stage_perf.integer_argument,
        default=stage_perf.DEFAULT_BOOTSTRAP_SEED,
    )
    parser.add_argument("--tie-epsilon-ms", type=float, default=0.0)
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)

    surface_pairs = [
        (label, Path(baseline), Path(candidate))
        for label, baseline, candidate in args.surface_pair
    ]
    try:
        result = compare_logs(
            args.baseline_log,
            args.candidate_log,
            replay_pair=(args.replay_pair[0], args.replay_pair[1]),
            surface_pairs=surface_pairs,
            bootstrap_iterations=args.bootstrap_iterations,
            bootstrap_seed=args.seed,
            tie_epsilon_ms=args.tie_epsilon_ms,
        )
    except stage_perf.ComparisonError as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 2

    output = (
        json.dumps(result, ensure_ascii=False, indent=2, sort_keys=True) + "\n"
        if args.json
        else render_text(result)
    )
    if args.output is None:
        sys.stdout.write(output)
    else:
        try:
            args.output.write_text(output, encoding="utf-8")
        except OSError as error:
            print(f"ERROR: {args.output}: cannot write output: {error}", file=sys.stderr)
            return 2
    return 0 if result["acceptance_passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
