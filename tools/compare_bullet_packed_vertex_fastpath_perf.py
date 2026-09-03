#!/usr/bin/env python3
"""Strict r056 OFF / r057 Bullet packed-vertex M1 Stage-5 gate.

M1 is allowed to remove only the intermediate 28-byte Bullet vertex staging
write.  It retains the canonical four-vertex/indexed topology, draw boundaries,
state boundary, texture uploads, matrices, VFPU work, and gameplay workload.
Consequently every manager and game-owned render-workload field is pairwise
exact.  Raw vertex totals may differ only by the separately measured,
successfully submitted wall-clock FPS/replay diagnostic overlay vertices; the
derived game vertex total and peak remain exact, and no numeric tolerance is
used.  The separate product telemetry proves that each accepted final quad was
packed once into the Present-fenced 24-byte arena without
owner/state/index/capacity, contract, client-array fallback, abandonment,
recovery-split, or canonical fallback.

The performance decision is deliberately pre-registered: twenty Stage-5
windows (301..6001, step 300), byte-identical supplied surfaces, a positive
paired mean improvement, and a strictly positive lower bound from exactly one
million paired-bootstrap resamples.  Replay-file correctness belongs to the
separate r054/r055 audit pair; replay/demo context is still exact here.
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


SCHEMA = "th08_bullet_packed_vertex_fastpath_perf_comparison_v2"
FASTPATH_PREFIX = "BULLET_PACKED_VERTEX_FASTPATH_TELEMETRY"
EXPECTED_KEYS = tuple((5, 301 + 300 * index) for index in range(20))
EXPECTED_TEARDOWN_STAGE_FRAME = 6119
REQUIRED_BOOTSTRAP_ITERATIONS = 1_000_000
EXPECTED_ARENA_CAPACITY_VERTICES = 1536 * 4
MAX_RUN_QUADS = 1536

CONTEXT_FIELDS = frozenset({"replay", "demo"})
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
VERTEX_ACCOUNTING_FIELDS = (
    RAW_VERTEX_FIELDS | FPS_OVERLAY_VERTEX_FIELDS | GAME_VERTEX_FIELDS
)
REQUIRED_EXACT_RENDER_FIELDS = frozenset(
    {
        "render_draws_total",
        "render_draws_peak",
        *GAME_VERTEX_FIELDS,
        "render_state_requested_total",
        "render_state_requested_peak",
        "render_state_emitted_total",
        "render_state_emitted_peak",
        "render_matrix_recompute_total",
        "render_matrix_recompute_peak",
        "render_vfpu_sincos_total",
        "render_vfpu_sincos_peak",
        "render_upload_attempt_total",
        "render_upload_attempt_peak",
        "render_actual_upload_total",
        "render_actual_upload_peak",
        "render_upload_bytes_total",
        "render_upload_bytes_peak",
    }
)

# Device-lifetime cumulative counters.  max_run/high-water/capacity are gauges
# and are intentionally kept out of interval subtraction.
COUNTER_FIELDS = (
    "begin_attempts",
    "accepted_batches",
    "canonical_fallback_batches",
    "append_attempts",
    "appended_quads",
    "packed_vertices",
    "uniform_diffuse_quads",
    "per_vertex_diffuse_quads",
    "submit_attempts",
    "submitted_runs",
    "submitted_quads",
    "native_submits",
    "native_submitted_quads",
    "client_fallback_submits",
    "client_fallback_quads",
    "owner_fallbacks",
    "state_fallbacks",
    "index_fallbacks",
    "capacity_fallbacks",
    "contract_fallbacks",
    "abandoned_runs",
    "abandoned_quads",
    "recovery_split_runs",
    "recovery_split_quads",
    "frontend_28b_bytes_avoided",
    "packed_24b_bytes",
)
GAUGE_FIELDS = (
    "max_run_quads",
    "arena_high_water_vertices",
    "arena_capacity_vertices",
)
STATIC_INT_FIELDS = (
    "manager_28B_staging_writes",
    "packed_generation_passes",
    "extra_flush",
)
FAILURE_FIELDS = (
    "canonical_fallback_batches",
    "client_fallback_submits",
    "client_fallback_quads",
    "owner_fallbacks",
    "state_fallbacks",
    "index_fallbacks",
    "capacity_fallbacks",
    "contract_fallbacks",
    "abandoned_runs",
    "abandoned_quads",
    "recovery_split_runs",
    "recovery_split_quads",
    "per_vertex_diffuse_quads",
)


class RuleBook:
    """Collect all hard-gate failures so a NO-GO preserves every cause."""

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


def parse_fastpath_records(path: Path) -> list[dict[str, Any]]:
    """Parse bounded M1 records, including one appended after another line."""

    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError as error:
        raise stage_perf.ComparisonError(f"{path}: cannot read log: {error}") from error

    marker = FASTPATH_PREFIX + " "
    records: list[dict[str, Any]] = []
    for line_number, raw_line in enumerate(lines, 1):
        start = raw_line.find(marker)
        if start < 0:
            continue
        fields = raw_line[start:].split()
        if not fields or fields[0] != FASTPATH_PREFIX:
            continue
        record: dict[str, Any] = {"record": FASTPATH_PREFIX, "line": line_number}
        for field in fields[1:]:
            if "=" not in field:
                continue
            name, value = field.split("=", 1)
            if name in record:
                raise stage_perf.ComparisonError(
                    f"{path}:{line_number}: duplicate {FASTPATH_PREFIX} field {name!r}"
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
) -> dict[tuple[int, int], dict[str, Any]]:
    samples: dict[tuple[int, int], dict[str, Any]] = {}
    for record in records:
        if record.get("kind") != "SAMPLE" or record.get("phase") != "stage_relative_periodic":
            continue
        if record.get("stage") != 5:
            continue
        key = (_require_int(record, "stage", path), _require_int(record, "stage_frame", path))
        if key in samples:
            raise stage_perf.ComparisonError(
                f"{path}:{record['line']}: duplicate M1 SAMPLE {key}"
            )
        samples[key] = record
    if list(samples) != keys:
        raise stage_perf.ComparisonError(
            f"{path}: M1 SAMPLE keys {list(samples)} do not exactly match {keys}"
        )
    return samples


def _teardown_record(path: Path, records: list[dict[str, Any]]) -> dict[str, Any]:
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
            f"{path}: expected one M1 Stage 5 frame "
            f"{EXPECTED_TEARDOWN_STAGE_FRAME} teardown, got {len(matches)}"
        )
    return matches[0]


def _metadata_valid(record: dict[str, Any]) -> bool:
    return (
        record.get("valid") == 1
        and record.get("counter_scope") == "device_lifetime"
        and record.get("cumulative") == 1
        and record.get("counter_bits") == "32_wrap"
    )


def _static_contract_valid(record: dict[str, Any]) -> bool:
    return (
        record.get("manager_28B_staging_writes") == 0
        and record.get("packed_generation_passes") == 1
        and record.get("extra_flush") == 0
        and record.get("topology") == "4v_indexed"
    )


def _values(record: dict[str, Any], path: Path) -> dict[str, int]:
    fields = (*COUNTER_FIELDS, *GAUGE_FIELDS, *STATIC_INT_FIELDS)
    values = {field: _require_int(record, field, path) for field in fields}
    if any(value < 0 for value in values.values()):
        raise stage_perf.ComparisonError(
            f"{path}:{record['line']}: negative M1 counter/gauge"
        )
    return values


def _check_snapshot_algebra(
    rules: RuleBook,
    key: tuple[int, int],
    values: dict[str, int],
) -> None:
    failures = {
        field: values[field] for field in FAILURE_FIELDS if values[field] != 0
    }
    rules.check(
        "fastpath_failures_zero",
        "canonical/client fallback, owner/state/index/capacity/contract failure, abandonment, split, and per-vertex diffuse must stay zero",
        not failures,
        key=key,
        observed=failures,
        expected="all zero",
    )
    rules.check(
        "fastpath_begin_accounting",
        "each begin attempt is accepted or canonically rejected before append",
        values["begin_attempts"]
        == values["accepted_batches"] + values["canonical_fallback_batches"],
        key=key,
        observed={
            field: values[field]
            for field in (
                "begin_attempts",
                "accepted_batches",
                "canonical_fallback_batches",
            )
        },
        expected="begin_attempts=accepted_batches+canonical_fallback_batches",
    )
    rules.check(
        "fastpath_append_accounting",
        "every append attempt produces one final packed quad and every packed quad is submitted or explicitly abandoned",
        values["append_attempts"] == values["appended_quads"]
        and values["appended_quads"]
        == values["submitted_quads"] + values["abandoned_quads"],
        key=key,
        observed={
            field: values[field]
            for field in (
                "append_attempts",
                "appended_quads",
                "submitted_quads",
                "abandoned_quads",
            )
        },
        expected="append=appended; appended=submitted+abandoned",
    )
    rules.check(
        "fastpath_packing_accounting",
        "each appended quad writes four 24-byte vertices and uses one diffuse classification",
        values["packed_vertices"] == values["appended_quads"] * 4
        and values["uniform_diffuse_quads"]
        + values["per_vertex_diffuse_quads"]
        == values["appended_quads"],
        key=key,
        observed={
            field: values[field]
            for field in (
                "appended_quads",
                "packed_vertices",
                "uniform_diffuse_quads",
                "per_vertex_diffuse_quads",
            )
        },
        expected="packed_vertices=4*appended; diffuse partition=appended",
    )
    rules.check(
        "fastpath_submit_accounting",
        "each submit attempt completes one run and native plus client routes partition all runs and quads",
        values["submit_attempts"] == values["submitted_runs"]
        and values["submitted_runs"]
        == values["native_submits"] + values["client_fallback_submits"]
        and values["submitted_quads"]
        == values["native_submitted_quads"] + values["client_fallback_quads"]
        and values["native_submitted_quads"] >= values["native_submits"]
        and values["client_fallback_quads"] >= values["client_fallback_submits"]
        and ((values["native_submits"] == 0) == (values["native_submitted_quads"] == 0))
        and ((values["client_fallback_submits"] == 0) == (values["client_fallback_quads"] == 0)),
        key=key,
        observed={
            field: values[field]
            for field in (
                "submit_attempts",
                "submitted_runs",
                "submitted_quads",
                "native_submits",
                "native_submitted_quads",
                "client_fallback_submits",
                "client_fallback_quads",
            )
        },
        expected="attempts=runs; native+client partition runs/quads; every run nonempty",
    )
    rules.check(
        "fastpath_byte_accounting",
        "telemetry must report exactly 4*28 avoided frontend bytes and 4*24 final bytes per quad",
        values["frontend_28b_bytes_avoided"]
        == values["appended_quads"] * 4 * 28
        and values["packed_24b_bytes"] == values["packed_vertices"] * 24,
        key=key,
        observed={
            "frontend_28b_bytes_avoided": values["frontend_28b_bytes_avoided"],
            "packed_24b_bytes": values["packed_24b_bytes"],
        },
        expected={
            "frontend_28b_bytes_avoided": values["appended_quads"] * 112,
            "packed_24b_bytes": values["packed_vertices"] * 24,
        },
    )
    capacity = values["arena_capacity_vertices"]
    high_water = values["arena_high_water_vertices"]
    max_run = values["max_run_quads"]
    rules.check(
        "fastpath_arena_bounds",
        "the complete 1536-quad arena is stable and max-run/high-water stay in its 4V bounds",
        capacity == EXPECTED_ARENA_CAPACITY_VERTICES
        and 0 <= max_run <= MAX_RUN_QUADS
        and 0 <= high_water <= capacity
        and high_water % 4 == 0
        and max_run * 4 <= high_water
        and ((values["submitted_runs"] == 0) == (max_run == 0))
        and ((values["submitted_quads"] == 0) == (high_water == 0)),
        key=key,
        observed={
            "max_run_quads": max_run,
            "arena_high_water_vertices": high_water,
            "arena_capacity_vertices": capacity,
            "submitted_runs": values["submitted_runs"],
            "submitted_quads": values["submitted_quads"],
        },
        expected="capacity=6144; max<=1536; 4*max<=HWM<=capacity; zero gauges iff unused",
    )


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


def _fps_overlay_vertex_reconciliations(
    keys: list[tuple[int, int]],
    baseline: list[dict[str, Any]],
    candidate: list[dict[str, Any]],
    baseline_path: Path,
    candidate_path: Path,
) -> list[dict[str, Any]]:
    """Prove that raw vertex noise is exactly the owned diagnostic overlay.

    Totals are additive, while peaks are maxima of per-frame values and must
    not be subtracted from one another.  Each log therefore proves the total
    identity and the legal max bounds; A/B then requires exact game total/peak
    and an exact raw-total delta == overlay-total delta.  There is deliberately
    no fixed or modulo-based allowance for an unexplained raw delta.
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
        quantized = overlay_total % 6 == 0 and overlay_peak % 6 == 0
        totals_reconcile = raw_total == overlay_total + game_total
        peaks_bounded = (
            overlay_peak <= raw_peak
            and game_peak <= raw_peak
            and raw_peak <= overlay_peak + game_peak
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
            and quantized
            and totals_reconcile
            and peaks_bounded
            and interval_bounds
        )

    reconciliations = []
    for key, baseline_record, candidate_record in zip(keys, baseline, candidate):
        baseline_values = values(baseline_record, baseline_path)
        candidate_values = values(candidate_record, candidate_path)
        baseline_integrity = integrity(baseline_values)
        candidate_integrity = integrity(candidate_values)
        raw_delta = candidate_values["raw_total"] - baseline_values["raw_total"]
        overlay_delta = (
            candidate_values["overlay_total"]
            - baseline_values["overlay_total"]
        )
        raw_delta_match = raw_delta == overlay_delta
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
                "baseline_integrity": baseline_integrity,
                "candidate_integrity": candidate_integrity,
                "raw_total_delta": raw_delta,
                "overlay_total_delta": overlay_delta,
                "raw_delta_matches_overlay_delta": raw_delta_match,
                "game_vertices_exact": game_exact,
                "match": (
                    baseline_integrity
                    and candidate_integrity
                    and raw_delta_match
                    and game_exact
                ),
            }
        )
    return reconciliations


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as source:
            for block in iter(lambda: source.read(1024 * 1024), b""):
                digest.update(block)
    except OSError as error:
        raise stage_perf.ComparisonError(f"{path}: cannot hash surface: {error}") from error
    return digest.hexdigest()


def _surface_results(
    surface_pairs: list[tuple[str, Path, Path]] | None,
) -> list[dict[str, Any]]:
    if not surface_pairs:
        raise stage_perf.ComparisonError("at least one surface artifact pair is required")
    seen: set[str] = set()
    result = []
    for label, baseline, candidate in surface_pairs:
        if not label or label in seen:
            raise stage_perf.ComparisonError(f"duplicate/empty surface label {label!r}")
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


def compare_logs(
    baseline_path: Path,
    candidate_path: Path,
    *,
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
    keys = _validate_fixed_route(
        baseline_path, candidate_path, baseline, candidate
    )
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
        | REQUIRED_EXACT_RENDER_FIELDS
        | VERTEX_ACCOUNTING_FIELDS
        | frozenset({"render_perf_valid", "render_frames"})
    )
    _require_fields(baseline_path, baseline, required_general)
    _require_fields(candidate_path, candidate, required_general)

    all_fields = set().union(*(record.keys() for record in baseline + candidate))
    manager_fields = {field for field in all_fields if stage_perf._manager_field(field)}
    manager_diagnostics = all_fields & stage_perf.MANAGER_DIAGNOSTIC_FIELDS
    render_fields = {
        field for field in all_fields if stage_perf._render_workload_field(field)
    }
    if not manager_fields:
        raise stage_perf.ComparisonError("no gameplay manager workload fields found")
    if not render_fields:
        raise stage_perf.ComparisonError("no render workload fields found")
    render_exact_fields = render_fields - RAW_VERTEX_FIELDS - FPS_OVERLAY_VERTEX_FIELDS
    workload_differences = (
        _field_differences(manager_fields, "manager", keys, baseline, candidate)
        + _field_differences(
            manager_diagnostics, "manager_diagnostic", keys, baseline, candidate
        )
        + _field_differences(CONTEXT_FIELDS, "context", keys, baseline, candidate)
        + _field_differences(
            render_exact_fields, "render_exact", keys, baseline, candidate
        )
    )
    ordinary_exact_workload_match = all(
        item["match"] for item in workload_differences
    )
    fps_overlay_vertex_reconciliations = _fps_overlay_vertex_reconciliations(
        keys, baseline, candidate, baseline_path, candidate_path
    )
    fps_overlay_vertex_reconciliation_match = all(
        item["match"] for item in fps_overlay_vertex_reconciliations
    )
    exact_workload_match = (
        ordinary_exact_workload_match
        and fps_overlay_vertex_reconciliation_match
    )

    baseline_fastpath = parse_fastpath_records(baseline_path)
    if baseline_fastpath:
        raise stage_perf.ComparisonError(
            f"{baseline_path}: M1-OFF control contains {len(baseline_fastpath)} "
            f"{FASTPATH_PREFIX} records"
        )
    candidate_fastpath = parse_fastpath_records(candidate_path)
    if not candidate_fastpath:
        raise stage_perf.ComparisonError(
            f"{candidate_path}: M1-ON product telemetry is missing"
        )
    fastpath_samples = _sample_records_by_key(
        candidate_path, candidate_fastpath, keys
    )
    teardown = _teardown_record(candidate_path, candidate_fastpath)
    first_sample_line = int(candidate[0]["line"])
    origins = [
        record
        for record in candidate_fastpath
        if int(record["line"]) < first_sample_line
        and record.get("kind") == "MARK"
        and record.get("stage") == 5
        and record.get("stage_frame") == 0
    ]
    if not origins:
        raise stage_perf.ComparisonError(
            f"{candidate_path}: no pre-sample Stage 5 frame-zero M1 origin"
        )

    required_fastpath = (
        "frame",
        "stage",
        "stage_frame",
        "valid",
        "cumulative",
        *COUNTER_FIELDS,
        *GAUGE_FIELDS,
        *STATIC_INT_FIELDS,
    )
    _require_fields(
        candidate_path,
        [*origins, *fastpath_samples.values(), teardown],
        required_fastpath,
    )

    rules = RuleBook()
    for item in fps_overlay_vertex_reconciliations:
        key = (item["stage"], item["stage_frame"])
        rules.check(
            "fps_overlay_vertex_accounting",
            "each log has nonnegative/multiple-of-six overlay counters, raw=game+overlay totals, and legal per-frame peak bounds",
            item["baseline_integrity"] and item["candidate_integrity"],
            key=key,
            observed={
                "baseline": item["baseline"],
                "candidate": item["candidate"],
            },
            expected=(
                "raw_total=game_total+overlay_total; overlay total/peak are "
                "multiples of 6; total/peak/frame bounds hold"
            ),
        )
        rules.check(
            "fps_overlay_raw_delta",
            "the complete A/B raw render-vertex total delta is exactly the measured FPS/replay diagnostic overlay delta",
            item["raw_delta_matches_overlay_delta"],
            key=key,
            observed={
                "raw_total_delta": item["raw_total_delta"],
                "overlay_total_delta": item["overlay_total_delta"],
            },
            expected="raw_total_delta=overlay_total_delta (no tolerance)",
        )
        rules.check(
            "fps_overlay_game_vertices_exact",
            "diagnostic-overlay removal leaves pairwise exact game-owned vertex total and per-frame peak",
            item["game_vertices_exact"],
            key=key,
            observed={
                "baseline_total": item["baseline"]["game_total"],
                "candidate_total": item["candidate"]["game_total"],
                "baseline_peak": item["baseline"]["game_peak"],
                "candidate_peak": item["candidate"]["game_peak"],
            },
            expected="game total and peak exact",
        )
    origin_values_list = [_values(record, candidate_path) for record in origins]
    for origin, origin_values in zip(origins, origin_values_list):
        rules.check(
            "fastpath_origin_metadata",
            "every pre-sample Stage-5 frame-zero M1 origin is a valid cumulative device-lifetime snapshot",
            _metadata_valid(origin) and _static_contract_valid(origin),
            observed={
                field: origin.get(field)
                for field in (
                    "valid",
                    "counter_scope",
                    "cumulative",
                    "counter_bits",
                    *STATIC_INT_FIELDS,
                    "topology",
                )
            },
            expected="valid device lifetime 32-bit telemetry and fixed M1 contract",
        )
        origin_zero_fields = (*COUNTER_FIELDS, "max_run_quads", "arena_high_water_vertices")
        rules.check(
            "fastpath_origin_zero",
            "every Stage-5 origin starts with all cumulative counters and used-arena gauges at zero",
            all(origin_values[field] == 0 for field in origin_zero_fields),
            observed={
                field: origin_values[field]
                for field in origin_zero_fields
                if origin_values[field] != 0
            },
            expected="all counters, max_run, and HWM zero",
        )
        rules.check(
            "fastpath_origin_capacity",
            "the frame-zero origin already owns the complete 6144-vertex arena",
            origin_values["arena_capacity_vertices"]
            == EXPECTED_ARENA_CAPACITY_VERTICES,
            observed=origin_values["arena_capacity_vertices"],
            expected=EXPECTED_ARENA_CAPACITY_VERTICES,
        )

    previous = origin_values_list[-1]
    intervals: list[dict[str, Any]] = []
    total_appended = 0
    for key, baseline_record, candidate_record in zip(keys, baseline, candidate):
        fastpath = fastpath_samples[key]
        values = _values(fastpath, candidate_path)
        rules.check(
            "render_telemetry_valid",
            "both fixed-route snapshots are complete 300-frame render intervals",
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
            "both performance runs are replay-driven with identical demo context",
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
        rules.check(
            "fastpath_sample_metadata",
            "every M1 SAMPLE preserves valid device-lifetime metadata and the fixed writer/topology contract",
            _metadata_valid(fastpath) and _static_contract_valid(fastpath),
            key=key,
            observed={
                field: fastpath.get(field)
                for field in (
                    "valid",
                    "counter_scope",
                    "cumulative",
                    "counter_bits",
                    *STATIC_INT_FIELDS,
                    "topology",
                )
            },
            expected="valid metadata; no 28B staging/extra flush; one packed pass; 4v_indexed",
        )
        rules.check(
            "fastpath_record_alignment",
            "the bounded M1 record is immediately persisted before its same-frame general SAMPLE",
            int(fastpath["line"]) + 1 == int(candidate_record["line"])
            and fastpath.get("frame") == candidate_record.get("frame"),
            key=key,
            observed={
                "fastpath_line": fastpath["line"],
                "sample_line": candidate_record["line"],
                "fastpath_frame": fastpath.get("frame"),
                "sample_frame": candidate_record.get("frame"),
            },
            expected="immediately preceding line and identical game frame",
        )
        monotonic = all(values[field] >= previous[field] for field in COUNTER_FIELDS)
        gauges_monotonic = (
            values["max_run_quads"] >= previous["max_run_quads"]
            and values["arena_high_water_vertices"]
            >= previous["arena_high_water_vertices"]
            and values["arena_capacity_vertices"]
            == previous["arena_capacity_vertices"]
        )
        rules.check(
            "fastpath_counters_no_wrap",
            "bounded Stage-5 cumulative counters and gauges are monotonic and capacity is stable",
            monotonic and gauges_monotonic,
            key=key,
            observed={"previous": previous, "current": values},
            expected="all counters/HWM/max nondecreasing; capacity unchanged",
        )
        _check_snapshot_algebra(rules, key, values)
        interval = {field: values[field] - previous[field] for field in COUNTER_FIELDS}
        rules.check(
            "fastpath_interval_coverage",
            "each measured present executes and accepts exactly one Bullet batch begin",
            interval["begin_attempts"] == candidate_record["render_frames"]
            and interval["accepted_batches"] == candidate_record["render_frames"]
            and interval["canonical_fallback_batches"] == 0,
            key=key,
            observed={
                "begin_attempts": interval["begin_attempts"],
                "accepted_batches": interval["accepted_batches"],
                "canonical_fallback_batches": interval[
                    "canonical_fallback_batches"
                ],
                "render_frames": candidate_record["render_frames"],
            },
            expected="begin=accepted=render_frames=300; canonical fallback=0",
        )
        intervals.append(
            {
                "stage": key[0],
                "stage_frame": key[1],
                **interval,
                "max_run_quads": values["max_run_quads"],
                "arena_high_water_vertices": values[
                    "arena_high_water_vertices"
                ],
                "arena_capacity_vertices": values[
                    "arena_capacity_vertices"
                ],
            }
        )
        total_appended += interval["appended_quads"]
        previous = values

    teardown_values = _values(teardown, candidate_path)
    rules.check(
        "fastpath_teardown_metadata",
        "the Stage-5 teardown is a valid fixed-contract M1 snapshot",
        _metadata_valid(teardown) and _static_contract_valid(teardown),
        observed={
            field: teardown.get(field)
            for field in (
                "valid",
                "counter_scope",
                "cumulative",
                "counter_bits",
                *STATIC_INT_FIELDS,
                "topology",
            )
        },
        expected="valid metadata and fixed M1 contract",
    )
    rules.check(
        "fastpath_teardown_no_wrap",
        "teardown counters/gauges remain monotonic and capacity remains stable",
        all(teardown_values[field] >= previous[field] for field in COUNTER_FIELDS)
        and teardown_values["max_run_quads"] >= previous["max_run_quads"]
        and teardown_values["arena_high_water_vertices"]
        >= previous["arena_high_water_vertices"]
        and teardown_values["arena_capacity_vertices"]
        == previous["arena_capacity_vertices"],
        observed={"last_sample": previous, "teardown": teardown_values},
        expected="no decrease/wrap and unchanged capacity",
    )
    _check_snapshot_algebra(
        rules, (5, EXPECTED_TEARDOWN_STAGE_FRAME), teardown_values
    )
    rules.check(
        "fastpath_product_exercised",
        "the twenty measured windows must append and submit at least one Bullet quad",
        total_appended > 0
        and sum(interval["submitted_quads"] for interval in intervals) > 0,
        observed={
            "appended_quads": total_appended,
            "submitted_quads": sum(
                interval["submitted_quads"] for interval in intervals
            ),
        },
        expected="both >0",
    )

    surfaces = _surface_results(surface_pairs)
    surface_identity_match = all(item["match"] for item in surfaces)
    rules.check(
        "surface_artifact_identity",
        "every supplied OFF/ON surface artifact is byte-identical",
        surface_identity_match,
        observed=[item for item in surfaces if not item["match"]],
        expected="all SHA-256 pairs identical",
    )

    reconciliations = rules.finish()
    reconciliation_match = all(rule["passed"] for rule in reconciliations)
    comparison_valid = (
        exact_workload_match and reconciliation_match and surface_identity_match
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
    hard_gate_failures = [
        rule["rule"] for rule in reconciliations if not rule["passed"]
    ]
    if not exact_workload_match:
        hard_gate_failures.insert(0, "exact_workload_match")

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
        "ordinary_exact_workload_match": ordinary_exact_workload_match,
        "fps_overlay_vertex_reconciliation_match": (
            fps_overlay_vertex_reconciliation_match
        ),
        "fastpath_reconciliation_match": reconciliation_match,
        "surface_identity_match": surface_identity_match,
        "general_strict_workload_match": general["strict_workload_match"],
        "state_emitted_model": (
            "exact: OFF canonical direct-GE and ON M1 use the same submit "
            "bridge; client fallback and any route-dependent state delta are rejected"
        ),
        "replay_correctness_gate": "separate r054/r055 audit pair",
        "bootstrap_iterations_required": REQUIRED_BOOTSTRAP_ITERATIONS,
        "bootstrap_iterations_valid": bootstrap_iterations_valid,
        "performance_gate_passed": performance_gate_passed,
        "acceptance_passed": acceptance_passed,
        "verdict": "GO" if acceptance_passed else "NO-GO",
        "hard_gate_failures": hard_gate_failures,
        "workload_fields": {
            "manager": sorted(manager_fields),
            "manager_diagnostic": sorted(manager_diagnostics),
            "context": sorted(CONTEXT_FIELDS),
            "render_exact": sorted(render_exact_fields),
            "render_owner_accounted": sorted(
                RAW_VERTEX_FIELDS | FPS_OVERLAY_VERTEX_FIELDS
            ),
            "explicit_render_contract": sorted(REQUIRED_EXACT_RENDER_FIELDS),
            "vertex_accounting_contract": sorted(VERTEX_ACCOUNTING_FIELDS),
        },
        "workload_differences": workload_differences,
        "fps_overlay_vertex_reconciliations": (
            fps_overlay_vertex_reconciliations
        ),
        "fastpath_origins": [
            {"line": record["line"], "phase": record.get("phase"), **values}
            for record, values in zip(origins, origin_values_list)
        ],
        "fastpath_intervals": intervals,
        "fastpath_teardown": {
            "line": teardown["line"],
            "phase": teardown.get("phase"),
            **teardown_values,
        },
        "surfaces": surfaces,
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
        "fps_overlay_vertex_reconciliation_match="
        f"{str(result['fps_overlay_vertex_reconciliation_match']).lower()}",
        "fastpath_reconciliation_match="
        f"{str(result['fastpath_reconciliation_match']).lower()}",
        f"surface_identity_match={str(result['surface_identity_match']).lower()}",
        f"state_emitted_model={result['state_emitted_model']}",
        f"replay_correctness_gate={result['replay_correctness_gate']}",
        "bootstrap_iterations_valid="
        f"{str(result['bootstrap_iterations_valid']).lower()} "
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
        for mismatch in difference["mismatches"]:
            lines.append(
                "  MISMATCH "
                f"stage={mismatch['stage']} stage_frame={mismatch['stage_frame']} "
                f"baseline={mismatch['baseline']!r} candidate={mismatch['candidate']!r}"
            )
    for item in result["fps_overlay_vertex_reconciliations"]:
        lines.append(
            "FPS_OVERLAY_VERTICES "
            f"stage={item['stage']} stage_frame={item['stage_frame']} "
            f"match={str(item['match']).lower()} "
            f"baseline_raw={item['baseline']['raw_total']} "
            f"baseline_overlay={item['baseline']['overlay_total']} "
            f"baseline_game={item['baseline']['game_total']} "
            f"candidate_raw={item['candidate']['raw_total']} "
            f"candidate_overlay={item['candidate']['overlay_total']} "
            f"candidate_game={item['candidate']['game_total']} "
            f"raw_delta={item['raw_total_delta']:+d} "
            f"overlay_delta={item['overlay_total_delta']:+d} "
            f"baseline_peaks={item['baseline']['raw_peak']}/"
            f"{item['baseline']['overlay_peak']}/"
            f"{item['baseline']['game_peak']} "
            f"candidate_peaks={item['candidate']['raw_peak']}/"
            f"{item['candidate']['overlay_peak']}/"
            f"{item['candidate']['game_peak']}"
        )
    for surface in result["surfaces"]:
        lines.append(
            f"SURFACE label={surface['label']} match={str(surface['match']).lower()} "
            f"baseline_sha256={surface['baseline_sha256']} "
            f"candidate_sha256={surface['candidate_sha256']}"
        )
    for interval in result["fastpath_intervals"]:
        lines.append(
            "FASTPATH_INTERVAL "
            f"stage={interval['stage']} stage_frame={interval['stage_frame']} "
            f"begin={interval['begin_attempts']} accepted={interval['accepted_batches']} "
            f"appended_quads={interval['appended_quads']} "
            f"runs={interval['submitted_runs']} quads={interval['submitted_quads']} "
            f"native={interval['native_submits']}/{interval['native_submitted_quads']} "
            f"client={interval['client_fallback_submits']}/{interval['client_fallback_quads']} "
            f"max_run={interval['max_run_quads']} "
            f"hwm={interval['arena_high_water_vertices']}/"
            f"{interval['arena_capacity_vertices']}"
        )
    for rule in result["reconciliations"]:
        lines.append(
            f"RECONCILE rule={rule['rule']} passed={str(rule['passed']).lower()} "
            f"violations={rule['violation_count']} description={rule['description']}"
        )
        for violation in rule["violations"]:
            location = ""
            if "stage" in violation:
                location = (
                    f" stage={violation['stage']}"
                    f" stage_frame={violation['stage_frame']}"
                )
            lines.append(
                f"  VIOLATION{location} observed={violation['observed']!r} "
                f"expected={violation['expected']!r}"
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


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Strictly gate r056 OFF / r057 Bullet packed-vertex M1 ON"
    )
    parser.add_argument("baseline_log", type=Path)
    parser.add_argument("candidate_log", type=Path)
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

    surfaces = [
        (label, Path(baseline), Path(candidate))
        for label, baseline, candidate in args.surface_pair
    ]
    try:
        result = compare_logs(
            args.baseline_log,
            args.candidate_log,
            surface_pairs=surfaces,
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
