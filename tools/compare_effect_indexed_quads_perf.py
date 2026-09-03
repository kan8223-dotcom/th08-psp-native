#!/usr/bin/env python3
"""Strict OFF/ON Stage-5 gate for ordinary Effect indexed quads.

The accepted renderer still submits the canonical six indices, so the general
render counters remain a logical-workload authority.  The dedicated interval
record proves the physical frontend change separately: every successful
ordinary Effect quad is written as four unique 28-byte vertices instead of six
duplicated vertices.  Acceptance requires the fixed twenty-window Stage-5
route, exact gameplay/logical workload, zero recovery debt, byte-identical
surface artifacts, and a positive paired timing result.
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
import effect_indexed_quads_contract as contract


SCHEMA = "th08_effect_indexed_quads_perf_comparison_v1"
CONTEXT_FIELDS = frozenset({"replay", "demo"})
BOUNDARY_FIELDS = frozenset(
    {
        "render_draws_total",
        "render_draws_peak",
        "render_state_requested_total",
        "render_state_requested_peak",
        "render_state_emitted_total",
        "render_state_emitted_peak",
        "render_upload_attempt_total",
        "render_upload_attempt_peak",
    }
)
RAW_OR_OVERLAY_VERTEX_FIELDS = (
    stage_perf.RAW_VERTEX_FIELDS | stage_perf.FPS_OVERLAY_VERTEX_FIELDS
)


class RuleBook:
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


def _require_int(record: dict[str, Any], field: str, path: Path) -> int:
    return stage_perf._require_int(record, field, path)


def _require_fields(
    path: Path, records: Iterable[dict[str, Any]], fields: Iterable[str]
) -> None:
    for record in records:
        for field in fields:
            _require_int(record, field, path)


def parse_effect_records(path: Path) -> list[dict[str, Any]]:
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError as error:
        raise stage_perf.ComparisonError(f"{path}: cannot read log: {error}") from error
    marker = contract.TELEMETRY_PREFIX + " "
    records: list[dict[str, Any]] = []
    for line_number, raw_line in enumerate(lines, 1):
        start = raw_line.find(marker)
        if start < 0:
            continue
        fields = raw_line[start:].split()
        if not fields or fields[0] != contract.TELEMETRY_PREFIX:
            continue
        record: dict[str, Any] = {
            "record": contract.TELEMETRY_PREFIX,
            "line": line_number,
        }
        for field in fields[1:]:
            if "=" not in field:
                continue
            name, value = field.split("=", 1)
            if name in record:
                raise stage_perf.ComparisonError(
                    f"{path}:{line_number}: duplicate Effect telemetry field {name!r}"
                )
            record[name] = stage_perf.parse_scalar(value)
        records.append(record)
    return records


def _effect_samples_by_key(
    path: Path,
    records: list[dict[str, Any]],
) -> dict[tuple[int, int], dict[str, Any]]:
    samples: dict[tuple[int, int], dict[str, Any]] = {}
    for record in records:
        if record.get("kind") != "SAMPLE" or record.get("phase") != "stage_relative_periodic":
            raise stage_perf.ComparisonError(
                f"{path}:{record['line']}: Effect telemetry must be SAMPLE-only stage-relative data"
            )
        key = (_require_int(record, "stage", path), _require_int(record, "stage_frame", path))
        if key in samples:
            raise stage_perf.ComparisonError(
                f"{path}:{record['line']}: duplicate Effect telemetry key {key}"
            )
        samples[key] = record
    if tuple(samples) != contract.EXPECTED_KEYS:
        raise stage_perf.ComparisonError(
            f"{path}: Effect telemetry keys {tuple(samples)} do not equal the fixed Stage-5 route"
        )
    return samples


def _validate_route(
    baseline_path: Path,
    candidate_path: Path,
    baseline: list[dict[str, Any]],
    candidate: list[dict[str, Any]],
) -> list[tuple[int, int]]:
    baseline_keys = stage_perf.validate_samples(baseline_path, baseline)
    candidate_keys = stage_perf.validate_samples(candidate_path, candidate)
    if tuple(baseline_keys) != contract.EXPECTED_KEYS:
        raise stage_perf.ComparisonError(
            f"{baseline_path}: expected exactly twenty Stage-5 keys {contract.EXPECTED_KEYS}, got {tuple(baseline_keys)}"
        )
    if tuple(candidate_keys) != contract.EXPECTED_KEYS:
        raise stage_perf.ComparisonError(
            f"{candidate_path}: expected exactly twenty Stage-5 keys {contract.EXPECTED_KEYS}, got {tuple(candidate_keys)}"
        )
    return baseline_keys


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


def _metadata_valid(record: dict[str, Any]) -> bool:
    return (
        record.get("valid") == 1
        and record.get("mode") == "product"
        and record.get("counter_scope") == "stage_relative_interval"
        and record.get("cumulative") == 0
        and record.get("interval_consumed") == 1
        and record.get("owner") == "game_frame_thread"
        and record.get("marks_peek") == 0
        and record.get("ordinary_effect_only") == 1
        and record.get("radial_trail_excluded") == 1
        and record.get("topology") == "6v_to_4v_indexed"
        and record.get("render_perf_vertices") == "logical_6v"
        and record.get("vertex_stride_bytes") == 28
    )


def _effect_values(record: dict[str, Any], path: Path) -> dict[str, int]:
    return {field: _require_int(record, field, path) for field in contract.INTERVAL_FIELDS}


def _check_effect_interval(
    rules: RuleBook,
    key: tuple[int, int],
    record: dict[str, Any],
    values: dict[str, int],
    render_frames: int,
) -> None:
    negative = {field: value for field, value in values.items() if value < 0}
    rules.check(
        "effect_counters_nonnegative",
        "all Effect interval counters and gauges must be nonnegative",
        not negative,
        key=key,
        observed=negative,
        expected="all >= 0",
    )
    rules.check(
        "effect_metadata",
        "Effect records must declare the sampled ordinary-only 6V-to-4V product contract",
        _metadata_valid(record),
        key=key,
        observed={
            name: record.get(name)
            for name in (
                "valid",
                "mode",
                "counter_scope",
                "cumulative",
                "interval_consumed",
                "owner",
                "marks_peek",
                "ordinary_effect_only",
                "radial_trail_excluded",
                "topology",
                "render_perf_vertices",
                "vertex_stride_bytes",
            )
        },
        expected="fixed product metadata",
    )
    failures = {
        field: values[field]
        for field in contract.FAILURE_FIELDS
        if values[field] != 0
    }
    rules.check(
        "effect_failures_zero",
        "fallback, owner-conflict, and abandonment counters must all remain zero",
        not failures,
        key=key,
        observed=failures,
        expected="all zero",
    )
    passes = values["passes"]
    flushes = values["flushes"]
    batches = values["batches"]
    quads = values["successful_ordinary_quads"]
    rules.check(
        "effect_pass_range",
        "the three Effect draw owners may open at most three presentation passes per rendered frame",
        0 < passes <= render_frames * 3,
        key=key,
        observed=passes,
        expected=f"1..{render_frames * 3}",
    )
    rules.check(
        "effect_flush_partition",
        "every nonempty Effect flush must either submit one indexed batch or record one fallback",
        flushes == batches + values["fallbacks"],
        key=key,
        observed={"flushes": flushes, "batches": batches, "fallbacks": values["fallbacks"]},
        expected="flushes=batches+fallbacks",
    )
    rules.check(
        "effect_success_shape",
        "successful batches contain at least one ordinary quad; an unused interval has zero success gauges",
        (
            batches == 0
            and quads == 0
            and values["max_batch_quads"] == 0
        )
        or (
            batches > 0
            and quads >= batches
            and 0 < values["max_batch_quads"] <= quads
            and values["max_batch_quads"] <= contract.MAX_BATCH_QUADS
        ),
        key=key,
        observed={"batches": batches, "quads": quads, "max_batch_quads": values["max_batch_quads"]},
        expected=(
            "all success values zero, or batches>0; quads>=batches; "
            f"0<max<=min(quads,{contract.MAX_BATCH_QUADS})"
        ),
    )
    rules.check(
        "effect_6v_4v_reconciliation",
        "each successful ordinary Effect quad must reconcile canonical 6V input to indexed 4V output",
        values["canonical_input_vertices"] == quads * 6
        and values["indexed_output_vertices"] == quads * 4
        and values["vertices_saved"] == quads * 2,
        key=key,
        observed={
            "quads": quads,
            "canonical_input_vertices": values["canonical_input_vertices"],
            "indexed_output_vertices": values["indexed_output_vertices"],
            "vertices_saved": values["vertices_saved"],
        },
        expected={"canonical": quads * 6, "indexed": quads * 4, "saved": quads * 2},
    )
    rules.check(
        "effect_saved_bytes",
        "saved bytes are exactly the two omitted 28-byte frontend vertices per successful quad",
        values["bytes_saved"] == values["vertices_saved"] * 28,
        key=key,
        observed=values["bytes_saved"],
        expected=values["vertices_saved"] * 28,
    )


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
        raise stage_perf.ComparisonError("at least one OFF/ON surface artifact pair is required")
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
    bootstrap_iterations: int = contract.REQUIRED_BOOTSTRAP_ITERATIONS,
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
    keys = _validate_route(baseline_path, candidate_path, baseline, candidate)
    general = stage_perf.compare_logs(
        baseline_path,
        candidate_path,
        bootstrap_iterations=bootstrap_iterations,
        bootstrap_seed=bootstrap_seed,
        tie_epsilon_ms=tie_epsilon_ms,
        allow_fps_overlay_drift=True,
        stage=5,
    )

    required_general = BOUNDARY_FIELDS | CONTEXT_FIELDS | frozenset(
        {
            "frame",
            "render_perf_valid",
            "render_frames",
            "render_game_vertices_total",
            "render_game_vertices_peak",
        }
    ) | stage_perf.FPS_OVERLAY_VERTEX_ACCOUNTING_FIELDS
    _require_fields(baseline_path, baseline, required_general)
    _require_fields(candidate_path, candidate, required_general)

    all_fields = set().union(*(record.keys() for record in baseline + candidate))
    manager_fields = {field for field in all_fields if stage_perf._manager_field(field)}
    manager_diagnostics = all_fields & stage_perf.MANAGER_DIAGNOSTIC_FIELDS
    render_fields = {
        field
        for field in all_fields
        if stage_perf._render_workload_field(field)
        and field not in BOUNDARY_FIELDS
        and field not in RAW_OR_OVERLAY_VERTEX_FIELDS
    }
    if not manager_fields:
        raise stage_perf.ComparisonError("no gameplay manager workload fields found")
    if not render_fields:
        raise stage_perf.ComparisonError("no logical render workload fields found")
    workload_differences = (
        _field_differences(manager_fields, "manager", keys, baseline, candidate)
        + _field_differences(manager_diagnostics, "manager_diagnostic", keys, baseline, candidate)
        + _field_differences(CONTEXT_FIELDS, "context", keys, baseline, candidate)
        + _field_differences(render_fields, "logical_render", keys, baseline, candidate)
    )
    exact_workload_match = all(item["match"] for item in workload_differences)
    overlay_reconciliation = stage_perf.fps_overlay_vertex_reconciliations(
        keys, baseline, candidate, baseline_path, candidate_path
    )
    overlay_reconciliation_match = all(item["match"] for item in overlay_reconciliation)

    baseline_effect = parse_effect_records(baseline_path)
    if baseline_effect:
        raise stage_perf.ComparisonError(
            f"{baseline_path}: OFF control contains {contract.TELEMETRY_PREFIX}"
        )
    candidate_effect = parse_effect_records(candidate_path)
    candidate_effect = [
        record for record in candidate_effect if record.get("stage") == 5
    ]
    if not candidate_effect:
        raise stage_perf.ComparisonError(
            f"{candidate_path}: missing {contract.TELEMETRY_PREFIX}"
        )
    effect_samples = _effect_samples_by_key(candidate_path, candidate_effect)
    _require_fields(
        candidate_path,
        effect_samples.values(),
        ("frame", "stage", "stage_frame", *contract.INTERVAL_FIELDS),
    )

    rules = RuleBook()
    intervals = []
    total_quads = 0
    boundary_differences = []
    for key, baseline_record, candidate_record in zip(keys, baseline, candidate):
        effect_record = effect_samples[key]
        values = _effect_values(effect_record, candidate_path)
        rules.check(
            "render_interval_valid",
            "both fixed-route records must be complete 300-frame render intervals",
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
            expected="valid=1 and render_frames=300",
        )
        rules.check(
            "replay_context",
            "both runs must be replay-driven with identical demo context",
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
            "effect_record_alignment",
            "Effect telemetry must precede its same-frame general SAMPLE",
            int(effect_record["line"]) < int(candidate_record["line"])
            and effect_record["frame"] == candidate_record["frame"],
            key=key,
            observed={
                "effect_line": effect_record["line"],
                "sample_line": candidate_record["line"],
                "effect_frame": effect_record["frame"],
                "sample_frame": candidate_record["frame"],
            },
            expected="Effect line before SAMPLE and identical game frame",
        )
        _check_effect_interval(
            rules, key, effect_record, values, candidate_record["render_frames"]
        )
        rules.check(
            "logical_vertex_workload_exact",
            "control canonical 6V and candidate indexed 4V must retain identical logical submitted game vertices",
            baseline_record["render_game_vertices_total"]
            == candidate_record["render_game_vertices_total"]
            and baseline_record["render_game_vertices_peak"]
            == candidate_record["render_game_vertices_peak"],
            key=key,
            observed={
                "baseline_total": baseline_record["render_game_vertices_total"],
                "candidate_total": candidate_record["render_game_vertices_total"],
                "baseline_peak": baseline_record["render_game_vertices_peak"],
                "candidate_peak": candidate_record["render_game_vertices_peak"],
            },
            expected="total and peak exact",
        )

        draw_delta = candidate_record["render_draws_total"] - baseline_record["render_draws_total"]
        draw_peak_delta = candidate_record["render_draws_peak"] - baseline_record["render_draws_peak"]
        rules.check(
            "effect_boundary_draw_bound",
            "Begin/End isolation may only add the two edge splits of each Effect pass",
            0 <= draw_delta <= values["passes"] * 2
            and 0 <= draw_peak_delta <= 6,
            key=key,
            observed={"total_delta": draw_delta, "peak_delta": draw_peak_delta},
            expected={"total": f"0..{values['passes'] * 2}", "peak": "0..6"},
        )
        requested_delta = candidate_record["render_state_requested_total"] - baseline_record["render_state_requested_total"]
        requested_peak_delta = candidate_record["render_state_requested_peak"] - baseline_record["render_state_requested_peak"]
        upload_delta = candidate_record["render_upload_attempt_total"] - baseline_record["render_upload_attempt_total"]
        upload_peak_delta = candidate_record["render_upload_attempt_peak"] - baseline_record["render_upload_attempt_peak"]
        rules.check(
            "effect_boundary_frontend_accounting",
            "each added textured draw owns exactly three frontend state requests and one clean upload attempt; interval peaks stay within the six-split per-frame bound",
            requested_delta == draw_delta * 3
            and upload_delta == draw_delta
            and 0 <= requested_peak_delta <= 18
            and 0 <= upload_peak_delta <= 6,
            key=key,
            observed={
                "draw_delta": draw_delta,
                "state_requested_delta": requested_delta,
                "state_requested_peak_delta": requested_peak_delta,
                "upload_attempt_delta": upload_delta,
                "upload_attempt_peak_delta": upload_peak_delta,
            },
            expected={
                "state_requested_delta": draw_delta * 3,
                "state_requested_peak_delta": "0..18",
                "upload_attempt_delta": draw_delta,
                "upload_attempt_peak_delta": "0..6",
            },
        )
        emitted_delta = candidate_record["render_state_emitted_total"] - baseline_record["render_state_emitted_total"]
        emitted_peak_delta = candidate_record["render_state_emitted_peak"] - baseline_record["render_state_emitted_peak"]
        emitted_bound = (values["batches"] + draw_delta) * 12
        rules.check(
            "effect_backend_state_bound",
            "indexed replacement batches and edge splits bound total and peak PSPGL/client-array state-emission drift",
            abs(emitted_delta) <= emitted_bound
            and abs(emitted_peak_delta) <= emitted_bound,
            key=key,
            observed={
                "total_delta": emitted_delta,
                "peak_delta": emitted_peak_delta,
            },
            expected=f"abs(total/peak delta)<={emitted_bound}",
        )
        boundary_differences.append(
            {
                "stage": key[0],
                "stage_frame": key[1],
                "draw_total_delta": draw_delta,
                "draw_peak_delta": draw_peak_delta,
                "state_requested_total_delta": requested_delta,
                "state_requested_peak_delta": requested_peak_delta,
                "state_emitted_total_delta": emitted_delta,
                "state_emitted_peak_delta": emitted_peak_delta,
                "upload_attempt_total_delta": upload_delta,
                "upload_attempt_peak_delta": upload_peak_delta,
            }
        )
        intervals.append({"stage": key[0], "stage_frame": key[1], **values})
        total_quads += values["successful_ordinary_quads"]

    rules.check(
        "effect_product_exercised",
        "the fixed route must submit at least one ordinary Effect quad through the indexed product",
        total_quads > 0,
        observed=total_quads,
        expected=">0",
    )

    surfaces = _surface_results(surface_pairs)
    surface_identity_match = all(item["match"] for item in surfaces)
    rules.check(
        "surface_artifact_identity",
        "every supplied OFF/ON surface artifact must be byte-identical",
        surface_identity_match,
        observed=[item for item in surfaces if not item["match"]],
        expected="all SHA-256 pairs identical",
    )

    reconciliations = rules.finish()
    reconciliation_match = all(rule["passed"] for rule in reconciliations)
    comparison_valid = (
        exact_workload_match
        and overlay_reconciliation_match
        and reconciliation_match
        and surface_identity_match
    )
    timing = general["timing"]
    performance_gate_passed = (
        timing["paired_improvement_ms_mean"] > 0.0
        and timing["bootstrap_95_low_ms"] > 0.0
    )
    bootstrap_iterations_valid = (
        timing["bootstrap_iterations"]
        == contract.REQUIRED_BOOTSTRAP_ITERATIONS
    )
    acceptance_passed = (
        comparison_valid
        and performance_gate_passed
        and bootstrap_iterations_valid
    )
    hard_gate_failures = [
        rule["rule"] for rule in reconciliations if not rule["passed"]
    ]
    if not exact_workload_match:
        hard_gate_failures.insert(0, "exact_workload_match")
    if not overlay_reconciliation_match:
        hard_gate_failures.insert(0, "fps_overlay_vertex_reconciliation")

    return {
        "schema": SCHEMA,
        "feature_macro": contract.FEATURE_MACRO,
        "telemetry_prefix": contract.TELEMETRY_PREFIX,
        "baseline_path": str(baseline_path),
        "candidate_path": str(candidate_path),
        "expected_route": {
            "stage": 5,
            "first_stage_frame": contract.EXPECTED_KEYS[0][1],
            "last_stage_frame": contract.EXPECTED_KEYS[-1][1],
            "step": 300,
            "sample_count": len(contract.EXPECTED_KEYS),
        },
        "sample_count": len(keys),
        "comparison_valid": comparison_valid,
        "acceptance_passed": acceptance_passed,
        "verdict": "GO" if acceptance_passed else "NO-GO",
        "exact_workload_match": exact_workload_match,
        "fps_overlay_vertex_reconciliation_match": overlay_reconciliation_match,
        "effect_reconciliation_match": reconciliation_match,
        "surface_identity_match": surface_identity_match,
        "performance_gate_passed": performance_gate_passed,
        "bootstrap_iterations_required": contract.REQUIRED_BOOTSTRAP_ITERATIONS,
        "bootstrap_iterations_valid": bootstrap_iterations_valid,
        "general_strict_workload_match": general["strict_workload_match"],
        "hard_gate_failures": hard_gate_failures,
        "workload_fields": {
            "manager": sorted(manager_fields),
            "manager_diagnostic": sorted(manager_diagnostics),
            "context": sorted(CONTEXT_FIELDS),
            "logical_render": sorted(render_fields),
            "boundary": sorted(BOUNDARY_FIELDS),
        },
        "workload_differences": workload_differences,
        "fps_overlay_vertex_reconciliations": overlay_reconciliation,
        "effect_intervals": intervals,
        "boundary_differences": boundary_differences,
        "surfaces": surfaces,
        "reconciliations": reconciliations,
        "timing": timing,
        "pairs": general["pairs"],
    }


def render_text(result: dict[str, Any]) -> str:
    lines = [
        f"schema={result['schema']}",
        f"baseline={result['baseline_path']}",
        f"candidate={result['candidate_path']}",
        f"samples={result['sample_count']} exact_workload_match={str(result['exact_workload_match']).lower()}",
        f"effect_reconciliation_match={str(result['effect_reconciliation_match']).lower()} surface_identity_match={str(result['surface_identity_match']).lower()}",
        "bootstrap_iterations_valid="
        f"{str(result['bootstrap_iterations_valid']).lower()} "
        f"required={result['bootstrap_iterations_required']}",
        f"performance_gate_passed={str(result['performance_gate_passed']).lower()} comparison_valid={str(result['comparison_valid']).lower()}",
        f"verdict={result['verdict']}",
    ]
    for rule in result["reconciliations"]:
        lines.append(
            f"RECONCILE rule={rule['rule']} passed={str(rule['passed']).lower()} violations={rule['violation_count']}"
        )
    for surface in result["surfaces"]:
        lines.append(
            f"SURFACE label={surface['label']} match={str(surface['match']).lower()} baseline_sha256={surface['baseline_sha256']} candidate_sha256={surface['candidate_sha256']}"
        )
    timing = result["timing"]
    lines.append(
        "TIMING "
        f"paired_improvement_ms_mean={timing['paired_improvement_ms_mean']:+.9f} "
        f"bootstrap_95_low_ms={timing['bootstrap_95_low_ms']:+.9f} "
        f"bootstrap_95_high_ms={timing['bootstrap_95_high_ms']:+.9f}"
    )
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Compare fixed-route TH08 PSP Effect indexed-quad OFF/ON logs"
    )
    parser.add_argument("baseline_log", type=Path)
    parser.add_argument("candidate_log", type=Path)
    parser.add_argument(
        "--surface-pair",
        action="append",
        nargs=3,
        metavar=("LABEL", "BASELINE", "CANDIDATE"),
        required=True,
    )
    parser.add_argument(
        "--bootstrap-iterations",
        type=int,
        default=contract.REQUIRED_BOOTSTRAP_ITERATIONS,
    )
    parser.add_argument(
        "--seed", type=stage_perf.integer_argument, default=stage_perf.DEFAULT_BOOTSTRAP_SEED
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
    if args.output is not None:
        try:
            args.output.write_text(output, encoding="utf-8")
        except OSError as error:
            print(f"ERROR: {args.output}: cannot write output: {error}", file=sys.stderr)
            return 2
    else:
        sys.stdout.write(output)
    return 0 if result["acceptance_passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
