#!/usr/bin/env python3
"""Strict OFF/ON Stage-5 gate for ITEM_TIME-triggered natural-batch 4V.

The candidate may only compact all quads in an *existing* canonical 6V Item
batch when that batch contains a visible ITEM_TIME.  It may not introduce a
Begin/End bracket, flush, draw, state request, texture upload attempt, split,
or abandonment.  The submitted GE index count therefore remains exact even
though the backend reads/packs four unique vertices instead of six duplicates.

The fixed gate uses twenty Stage-5 windows, exact supplied surfaces, and a
one-million-sample paired bootstrap whose point estimate and lower 95% bound
must both be positive.
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
import item_natural_quads_contract as contract


SCHEMA = "th08_item_natural_quads_perf_comparison_v1"
CONTEXT_FIELDS = frozenset({"replay", "demo"})
BOUNDARY_FIELDS = frozenset(
    {
        "render_draws_total",
        "render_draws_peak",
        "render_state_requested_total",
        "render_state_requested_peak",
        "render_upload_attempt_total",
        "render_upload_attempt_peak",
    }
)
REQUIRED_EXACT_RENDER_FIELDS = BOUNDARY_FIELDS | frozenset(
    {
        "render_vertices_total",
        "render_vertices_peak",
        "render_state_emitted_total",
        "render_state_emitted_peak",
        "render_matrix_recompute_total",
        "render_matrix_recompute_peak",
        "render_vfpu_sincos_total",
        "render_vfpu_sincos_peak",
        "render_actual_upload_total",
        "render_actual_upload_peak",
        "render_upload_bytes_total",
        "render_upload_bytes_peak",
    }
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


def parse_natural_records(path: Path) -> list[dict[str, Any]]:
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
                    f"{path}:{line_number}: duplicate natural-quad field {name!r}"
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
    expected = list(contract.EXPECTED_KEYS)
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
                f"{path}:{record['line']}: duplicate natural-quad SAMPLE {key}"
            )
        samples[key] = record
    if list(samples) != keys:
        raise stage_perf.ComparisonError(
            f"{path}: natural-quad SAMPLE keys {list(samples)} do not match {keys}"
        )
    return samples


def _teardown_record(path: Path, records: list[dict[str, Any]]) -> dict[str, Any]:
    matches = [
        record
        for record in records
        if record.get("kind") == "MARK"
        and record.get("phase") == "stage_teardown_complete"
        and record.get("stage") == 5
        and record.get("stage_frame") == contract.EXPECTED_TEARDOWN_STAGE_FRAME
    ]
    if len(matches) != 1:
        raise stage_perf.ComparisonError(
            f"{path}: expected one natural-quad Stage 5 frame "
            f"{contract.EXPECTED_TEARDOWN_STAGE_FRAME} teardown, got {len(matches)}"
        )
    return matches[0]


def _metadata_valid(record: dict[str, Any]) -> bool:
    return (
        record.get("mode") == contract.MODE
        and record.get("counter_scope") == "stage_relative_interval"
        and record.get("cumulative") == 0
        and record.get("existing_flush") == 1
        and record.get("begin_end_added") == 0
        and record.get("topology") == "6v_to_4v_indexed"
    )


def _values(record: dict[str, Any], path: Path) -> dict[str, int]:
    values = {
        field: _require_int(record, field, path)
        for field in contract.INTERVAL_FIELDS
    }
    if any(value < 0 for value in values.values()):
        raise stage_perf.ComparisonError(
            f"{path}:{record['line']}: negative natural-quad counter"
        )
    return values


def _check_interval_algebra(
    rules: RuleBook,
    key: tuple[int, int],
    values: dict[str, int],
    *,
    expected_passes: int | None,
) -> None:
    failures = {
        field: values[field]
        for field in contract.FAILURE_FIELDS
        if values[field] != 0
    }
    rules.check(
        "natural_failures_zero",
        "natural batching must never fallback, add a flush/split, or abandon a batch/quad",
        not failures,
        key=key,
        observed=failures,
        expected="all zero",
    )
    if expected_passes is not None:
        rules.check(
            "natural_pass_count",
            "the existing Item draw pass executes once per measured present",
            values["passes"] == expected_passes,
            key=key,
            observed=values["passes"],
            expected=expected_passes,
        )
    rules.check(
        "natural_candidate_partition",
        "every ITEM_TIME candidate is visible or culled and every visible ITEM_TIME contributes one trigger quad",
        values["item_time_candidates"]
        == values["visible_item_time"] + values["culled_item_time"]
        and values["visible_item_time"] == values["trigger_quads"],
        key=key,
        observed={
            field: values[field]
            for field in (
                "item_time_candidates",
                "visible_item_time",
                "culled_item_time",
                "trigger_quads",
            )
        },
        expected="candidates=visible+culled; visible=trigger_quads",
    )
    trigger_shape = (
        values["trigger_batches"] == values["canonical_batches"]
        and values["trigger_batches"] <= values["trigger_quads"]
        and ((values["trigger_batches"] == 0) == (values["trigger_quads"] == 0))
    )
    rules.check(
        "natural_trigger_batch_shape",
        "canonical_batches counts exactly the existing batches marked by at least one visible ITEM_TIME",
        trigger_shape,
        key=key,
        observed={
            "canonical_batches": values["canonical_batches"],
            "trigger_batches": values["trigger_batches"],
            "trigger_quads": values["trigger_quads"],
        },
        expected="trigger_batches=canonical_batches and <=trigger_quads; zero together",
    )
    rules.check(
        "natural_whole_batch_accounting",
        "an ITEM_TIME trigger packs every quad in that existing batch and submits it once",
        values["eligible_quads"]
        == values["trigger_quads"] + values["coalesced_quads"]
        and values["submitted_batches"] == values["trigger_batches"]
        and values["submitted_quads"] == values["eligible_quads"],
        key=key,
        observed={
            field: values[field]
            for field in (
                "trigger_batches",
                "trigger_quads",
                "coalesced_quads",
                "eligible_quads",
                "submitted_batches",
                "submitted_quads",
            )
        },
        expected="eligible=trigger+coalesced; submitted batches/quads equal trigger/eligible",
    )
    unsafe_fallback_sum = sum(
        values[field]
        for field in (
            "pointer_fallbacks",
            "span_fallbacks",
            "capacity_fallbacks",
            "topology_fallbacks",
            "state_fallbacks",
            "index_fallbacks",
        )
    )
    rules.check(
        "natural_public_client_only",
        "every replacement submit uses the public same-call client path; private native attempts, submits, and fallbacks are forbidden",
        values["native_submits"] == 0
        and values["native_submitted_quads"] == 0
        and values["native_fallbacks"] == 0
        and values["submitted_batches"] == values["client_fallback_submits"]
        and values["submitted_quads"] == values["client_fallback_quads"]
        and values["client_fallback_quads"] >= values["client_fallback_submits"]
        and ((values["client_fallback_submits"] == 0) == (values["client_fallback_quads"] == 0)),
        key=key,
        observed={
            field: values[field]
            for field in (
                "submitted_batches",
                "submitted_quads",
                "native_submits",
                "native_submitted_quads",
                "native_fallbacks",
                "client_fallback_submits",
                "client_fallback_quads",
            )
        },
        expected="native counters all zero; submitted batches/quads equal the public client route",
    )
    rules.check(
        "natural_backend_partition",
        "topology checks cover each marked whole batch and unsafe fallback buckets exactly partition fallback_batches",
        values["extra_topology_batches"] == values["topology_fallbacks"]
        and values["topology_checks"] == values["trigger_batches"]
        and values["topology_checked_quads"] == values["eligible_quads"]
        and unsafe_fallback_sum == values["fallback_batches"],
        key=key,
        observed={
            field: values[field]
            for field in (
                "submitted_batches",
                "submitted_quads",
                "native_submits",
                "native_submitted_quads",
                "client_fallback_submits",
                "client_fallback_quads",
                "native_fallbacks",
                "fallback_batches",
                "pointer_fallbacks",
                "span_fallbacks",
                "capacity_fallbacks",
                "topology_fallbacks",
                "state_fallbacks",
                "extra_topology_batches",
                "index_fallbacks",
                "topology_checks",
                "topology_checked_quads",
            )
        },
        expected="topology checks cover trigger batches/all eligible quads; unsafe sum=fallback_batches",
    )
    rules.check(
        "natural_vertex_accounting",
        "each submitted canonical quad is read as 6V and packed once as 4V, avoiding exactly two duplicate reads",
        values["canonical_input_vertices"] == values["eligible_quads"] * 6
        and values["packed_output_vertices"] == values["eligible_quads"] * 4
        and values["duplicate_vertices_avoided"]
        == values["eligible_quads"] * 2,
        key=key,
        observed={
            "canonical_input_vertices": values["canonical_input_vertices"],
            "packed_output_vertices": values["packed_output_vertices"],
            "duplicate_vertices_avoided": values[
                "duplicate_vertices_avoided"
            ],
        },
        expected={
            "canonical_input_vertices": values["eligible_quads"] * 6,
            "packed_output_vertices": values["eligible_quads"] * 4,
            "duplicate_vertices_avoided": values["eligible_quads"] * 2,
        },
    )
    batch_bound = (
        (values["submitted_batches"] == 0 and values["max_batch_quads"] == 0)
        or (
            values["submitted_batches"] > 0
            and 1 <= values["max_batch_quads"] <= values["submitted_quads"]
            and values["submitted_quads"]
            <= values["submitted_batches"] * values["max_batch_quads"]
        )
    )
    rules.check(
        "natural_batch_bound",
        "max batch size bounds every nonempty submitted whole-batch partition",
        batch_bound,
        key=key,
        observed={
            "submitted_batches": values["submitted_batches"],
            "submitted_quads": values["submitted_quads"],
            "max_batch_quads": values["max_batch_quads"],
        },
        expected="empty=>max0; otherwise 1<=max<=quads<=batches*max",
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
    workload_differences = (
        _field_differences(manager_fields, "manager", keys, baseline, candidate)
        + _field_differences(
            manager_diagnostics, "manager_diagnostic", keys, baseline, candidate
        )
        + _field_differences(CONTEXT_FIELDS, "context", keys, baseline, candidate)
        + _field_differences(render_fields, "render_exact", keys, baseline, candidate)
    )
    exact_workload_match = all(item["match"] for item in workload_differences)

    baseline_natural = parse_natural_records(baseline_path)
    if baseline_natural:
        raise stage_perf.ComparisonError(
            f"{baseline_path}: natural-quad OFF control contains product telemetry"
        )
    candidate_natural = parse_natural_records(candidate_path)
    if not candidate_natural:
        raise stage_perf.ComparisonError(
            f"{candidate_path}: {contract.TELEMETRY_PREFIX} is missing"
        )
    natural_samples = _sample_records_by_key(
        candidate_path, candidate_natural, keys
    )
    teardown = _teardown_record(candidate_path, candidate_natural)
    _require_fields(
        candidate_path,
        [*natural_samples.values(), teardown],
        ("frame", "stage", "stage_frame", "cumulative", *contract.INTERVAL_FIELDS),
    )

    rules = RuleBook()
    intervals: list[dict[str, Any]] = []
    total_batches = total_quads = total_triggers = 0
    for key, baseline_record, candidate_record in zip(keys, baseline, candidate):
        natural = natural_samples[key]
        values = _values(natural, candidate_path)
        rules.check(
            "render_telemetry_valid",
            "both fixed-route snapshots are complete 300-frame intervals",
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
            expected="valid=1 and frames=300 on both",
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
            "natural_metadata",
            "every natural-quad record declares interval scope, existing flush, no added Begin/End, and indexed 6V-to-4V topology",
            _metadata_valid(natural),
            key=key,
            observed={
                field: natural.get(field)
                for field in (
                    "mode",
                    "counter_scope",
                    "cumulative",
                    "existing_flush",
                    "begin_end_added",
                    "topology",
                )
            },
            expected="product/stage_relative_interval/0/existing_flush=1/begin_end_added=0/6v_to_4v_indexed",
        )
        rules.check(
            "natural_record_alignment",
            "natural telemetry is immediately persisted before its same-frame general SAMPLE",
            int(natural["line"]) + 1 == int(candidate_record["line"])
            and natural.get("frame") == candidate_record.get("frame"),
            key=key,
            observed={
                "natural_line": natural["line"],
                "sample_line": candidate_record["line"],
                "natural_frame": natural.get("frame"),
                "sample_frame": candidate_record.get("frame"),
            },
            expected="immediately preceding line and identical frame",
        )
        boundary_deltas = {
            field: candidate_record[field] - baseline_record[field]
            for field in BOUNDARY_FIELDS
        }
        rules.check(
            "natural_existing_boundary_exact",
            "OFF/ON draw, state-request, and texture-upload boundaries must be exactly identical (rejects r052/r053 extra flushes)",
            all(delta == 0 for delta in boundary_deltas.values()),
            key=key,
            observed={
                field: delta for field, delta in boundary_deltas.items() if delta != 0
            },
            expected="all draw/state-request/upload total+peak deltas zero",
        )
        _check_interval_algebra(
            rules,
            key,
            values,
            expected_passes=candidate_record["render_frames"],
        )
        intervals.append({"stage": key[0], "stage_frame": key[1], **values})
        total_batches += values["submitted_batches"]
        total_quads += values["submitted_quads"]
        total_triggers += values["trigger_quads"]

    teardown_values = _values(teardown, candidate_path)
    rules.check(
        "natural_teardown_metadata",
        "the Stage-5 teardown retains the same natural-batch metadata contract",
        _metadata_valid(teardown),
        observed={
            field: teardown.get(field)
            for field in (
                "mode",
                "counter_scope",
                "cumulative",
                "existing_flush",
                "begin_end_added",
                "topology",
            )
        },
        expected="same fixed product contract",
    )
    _check_interval_algebra(
        rules,
        (5, contract.EXPECTED_TEARDOWN_STAGE_FRAME),
        teardown_values,
        expected_passes=None,
    )
    rules.check(
        "natural_product_exercised",
        "measured windows must contain visible ITEM_TIME triggers and submitted whole batches/quads",
        total_triggers > 0 and total_batches > 0 and total_quads > 0,
        observed={
            "trigger_quads": total_triggers,
            "submitted_batches": total_batches,
            "submitted_quads": total_quads,
        },
        expected="all >0",
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
        timing["bootstrap_iterations"] == contract.REQUIRED_BOOTSTRAP_ITERATIONS
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
            "teardown_stage_frame": contract.EXPECTED_TEARDOWN_STAGE_FRAME,
        },
        "aligned": True,
        "sample_count": len(keys),
        "comparison_valid": comparison_valid,
        "exact_workload_match": exact_workload_match,
        "natural_reconciliation_match": reconciliation_match,
        "surface_identity_match": surface_identity_match,
        "general_strict_workload_match": general["strict_workload_match"],
        "bootstrap_iterations_required": contract.REQUIRED_BOOTSTRAP_ITERATIONS,
        "bootstrap_iterations_valid": bootstrap_iterations_valid,
        "performance_gate_passed": performance_gate_passed,
        "acceptance_passed": acceptance_passed,
        "verdict": "GO" if acceptance_passed else "NO-GO",
        "hard_gate_failures": hard_gate_failures,
        "workload_fields": {
            "manager": sorted(manager_fields),
            "manager_diagnostic": sorted(manager_diagnostics),
            "context": sorted(CONTEXT_FIELDS),
            "render_exact": sorted(render_fields),
            "boundary_exact": sorted(BOUNDARY_FIELDS),
        },
        "workload_differences": workload_differences,
        "natural_intervals": intervals,
        "natural_teardown": teardown_values,
        "surfaces": surfaces,
        "reconciliations": reconciliations,
        "timing": timing,
        "pairs": general["pairs"],
    }


def render_text(result: dict[str, Any]) -> str:
    timing = result["timing"]
    lines = [
        f"schema={result['schema']}",
        f"feature_macro={result['feature_macro']}",
        f"telemetry_prefix={result['telemetry_prefix']}",
        f"baseline={result['baseline_path']}",
        f"candidate={result['candidate_path']}",
        f"aligned={str(result['aligned']).lower()} samples={result['sample_count']} "
        "stage=5 stage_frames=301..6001 step=300",
        f"comparison_valid={str(result['comparison_valid']).lower()}",
        f"exact_workload_match={str(result['exact_workload_match']).lower()}",
        "natural_reconciliation_match="
        f"{str(result['natural_reconciliation_match']).lower()}",
        f"surface_identity_match={str(result['surface_identity_match']).lower()}",
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
    for surface in result["surfaces"]:
        lines.append(
            f"SURFACE label={surface['label']} match={str(surface['match']).lower()} "
            f"baseline_sha256={surface['baseline_sha256']} "
            f"candidate_sha256={surface['candidate_sha256']}"
        )
    for interval in result["natural_intervals"]:
        lines.append(
            "NATURAL_INTERVAL "
            f"stage={interval['stage']} stage_frame={interval['stage_frame']} "
            f"candidates={interval['item_time_candidates']} "
            f"visible={interval['visible_item_time']} culled={interval['culled_item_time']} "
            f"batches={interval['submitted_batches']} quads={interval['submitted_quads']} "
            f"trigger={interval['trigger_quads']} coalesced={interval['coalesced_quads']} "
            f"packed4v={interval['packed_output_vertices']}"
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
        description="Strictly gate ITEM_TIME-triggered natural-batch 4V OFF/ON"
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
        default=contract.REQUIRED_BOOTSTRAP_ITERATIONS,
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
