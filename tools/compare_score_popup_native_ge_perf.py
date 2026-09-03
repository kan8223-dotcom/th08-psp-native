#!/usr/bin/env python3
"""Strict r049 A/B gate for the PSP score-popup native GE submit.

Both inputs must already use the accepted score-popup two-corner frontend.
The candidate changes only the final PSPGL submission: a successful native
submit bypasses nine public client-array state calls.  Consequently every
gameplay and render-workload field remains exact except the two emitted-state
counters, whose deltas are reconciled against the native-submit telemetry.

The tool deliberately fixes the performance route to the twenty Stage 5
300-frame windows used by r049.  ``acceptance_passed`` additionally requires
the pre-registered one-million-iteration paired bootstrap and a strictly
positive point estimate and lower confidence bound.
"""

from __future__ import annotations

import argparse
import json
import sys
from collections import OrderedDict
from pathlib import Path
from typing import Any, Iterable

import compare_stage_relative_perf as stage_perf


SCHEMA = "th08_score_popup_native_ge_perf_comparison_v1"
NATIVE_PREFIX = "SCORE_POPUP_NATIVE_GE_TELEMETRY"
EXPECTED_KEYS = tuple((5, 301 + 300 * index) for index in range(20))
EXPECTED_TEARDOWN_STAGE_FRAME = 6119
REQUIRED_BOOTSTRAP_ITERATIONS = 1_000_000

CONTEXT_FIELDS = frozenset({"replay", "demo"})
EMITTED_STATE_FIELDS = frozenset(
    {"render_state_emitted_total", "render_state_emitted_peak"}
)
REQUIRED_EXACT_RENDER_FIELDS = frozenset(
    {
        "render_draws_total",
        "render_draws_peak",
        "render_vertices_total",
        "render_vertices_peak",
        "render_state_requested_total",
        "render_state_requested_peak",
        "render_upload_attempt_total",
        "render_upload_attempt_peak",
        "render_actual_upload_total",
        "render_actual_upload_peak",
        "render_upload_bytes_total",
        "render_upload_bytes_peak",
    }
)
BATCH_FIELDS = frozenset(
    f"render_ascii_popup_batch_{metric}_{aggregate}"
    for metric in (
        "calls",
        "digits",
        "sprites",
        "vertices_saved",
        "bytes_saved",
        "fallbacks",
    )
    for aggregate in ("total", "peak")
)
NATIVE_COUNTER_FIELDS = ("attempts", "submits", "client_fallbacks")


class RuleBook:
    """Collect every failed reconciliation without suppressing later rules."""

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
        result = []
        for rule in self._rules.values():
            item = dict(rule)
            item["passed"] = not item["violations"]
            item["violation_count"] = len(item["violations"])
            result.append(item)
        return result


def parse_native_records(path: Path) -> list[dict[str, Any]]:
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError as error:
        raise stage_perf.ComparisonError(f"{path}: cannot read log: {error}") from error

    marker = NATIVE_PREFIX + " "
    records: list[dict[str, Any]] = []
    for line_number, raw_line in enumerate(lines, 1):
        start = raw_line.find(marker)
        if start < 0:
            continue
        fields = raw_line[start:].split()
        if not fields or fields[0] != NATIVE_PREFIX:
            continue
        record: dict[str, Any] = {"record": NATIVE_PREFIX, "line": line_number}
        for field in fields[1:]:
            if "=" not in field:
                continue
            name, value = field.split("=", 1)
            if name in record:
                raise stage_perf.ComparisonError(
                    f"{path}:{line_number}: duplicate native field {name!r}"
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


def _native_metadata_valid(record: dict[str, Any]) -> bool:
    return (
        record.get("valid") == 1
        and record.get("counter_scope") == "device_lifetime"
        and record.get("cumulative") == 1
        and record.get("counter_bits") == "32_wrap"
    )


def _native_counters(record: dict[str, Any], path: Path) -> dict[str, int]:
    return {field: _require_int(record, field, path) for field in NATIVE_COUNTER_FIELDS}


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


def _validate_fixed_route(
    baseline_path: Path,
    candidate_path: Path,
    baseline: list[dict[str, Any]],
    candidate: list[dict[str, Any]],
) -> list[tuple[int, int]]:
    baseline_keys = stage_perf.validate_samples(baseline_path, baseline)
    candidate_keys = stage_perf.validate_samples(candidate_path, candidate)
    expected = list(EXPECTED_KEYS)
    if baseline_keys != expected:
        raise stage_perf.ComparisonError(
            f"{baseline_path}: expected exact Stage 5 keys {expected}, got {baseline_keys}"
        )
    if candidate_keys != expected:
        raise stage_perf.ComparisonError(
            f"{candidate_path}: expected exact Stage 5 keys {expected}, got {candidate_keys}"
        )
    return expected


def compare_logs(
    baseline_path: Path,
    candidate_path: Path,
    *,
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

    # Retain the established paired timing and raw strict result.  The raw
    # result is expected to reject successful native submissions because it
    # cannot know why emitted-state calls disappeared.
    general = stage_perf.compare_logs(
        baseline_path,
        candidate_path,
        bootstrap_iterations=bootstrap_iterations,
        bootstrap_seed=bootstrap_seed,
        tie_epsilon_ms=tie_epsilon_ms,
        stage=5,
    )

    required = (
        REQUIRED_EXACT_RENDER_FIELDS
        | BATCH_FIELDS
        | EMITTED_STATE_FIELDS
        | CONTEXT_FIELDS
        | frozenset({"render_perf_valid", "render_frames"})
    )
    _require_fields(baseline_path, baseline, required)
    _require_fields(candidate_path, candidate, required)

    all_fields = set().union(*(record.keys() for record in baseline + candidate))
    manager_fields = {
        field for field in all_fields if stage_perf._manager_field(field)
    }
    # Both r049 variants use the accepted live enumerator.  Its counters are
    # therefore workload evidence here, not a changing implementation detail.
    manager_diagnostics = all_fields & stage_perf.MANAGER_DIAGNOSTIC_FIELDS
    exact_render_fields = {
        field
        for field in all_fields
        if stage_perf._render_workload_field(field)
        and field not in EMITTED_STATE_FIELDS
    }
    if not manager_fields:
        raise stage_perf.ComparisonError("no gameplay manager fields found")
    if not exact_render_fields:
        raise stage_perf.ComparisonError("no exact render-workload fields found")

    workload_differences = (
        _field_differences(manager_fields, "manager", keys, baseline, candidate)
        + _field_differences(
            manager_diagnostics,
            "manager_diagnostic",
            keys,
            baseline,
            candidate,
        )
        + _field_differences(CONTEXT_FIELDS, "context", keys, baseline, candidate)
        + _field_differences(
            exact_render_fields, "render_exact", keys, baseline, candidate
        )
    )
    exact_workload_match = all(item["match"] for item in workload_differences)

    control_native = parse_native_records(baseline_path)
    if control_native:
        raise stage_perf.ComparisonError(
            f"{baseline_path}: native-OFF control contains {len(control_native)} "
            f"{NATIVE_PREFIX} records"
        )
    product_native = parse_native_records(candidate_path)
    if not product_native:
        raise stage_perf.ComparisonError(
            f"{candidate_path}: native-ON product has no {NATIVE_PREFIX} records"
        )

    native_samples: dict[tuple[int, int], dict[str, Any]] = {}
    for record in product_native:
        if (
            record.get("kind") != "SAMPLE"
            or record.get("phase") != "stage_relative_periodic"
            or record.get("stage") != 5
        ):
            continue
        key = (5, _require_int(record, "stage_frame", candidate_path))
        if key in native_samples:
            raise stage_perf.ComparisonError(
                f"{candidate_path}:{record['line']}: duplicate native sample {key}"
            )
        native_samples[key] = record
    if list(native_samples) != keys:
        raise stage_perf.ComparisonError(
            f"{candidate_path}: native sample keys {list(native_samples)} "
            f"do not exactly match {keys}"
        )

    first_general_line = int(candidate[0]["line"])
    origins = [
        record
        for record in product_native
        if record["line"] < first_general_line
        and record.get("kind") == "MARK"
        and record.get("stage") == 5
        and record.get("stage_frame") == 0
        and record.get("valid") == 1
    ]
    if not origins:
        raise stage_perf.ComparisonError(
            f"{candidate_path}: no valid Stage 5 stage_frame=0 native origin"
        )
    origin = origins[-1]

    teardowns = [
        record
        for record in product_native
        if record.get("kind") == "MARK"
        and record.get("phase") == "stage_teardown_complete"
        and record.get("stage") == 5
        and record.get("stage_frame") == EXPECTED_TEARDOWN_STAGE_FRAME
    ]
    if len(teardowns) != 1:
        raise stage_perf.ComparisonError(
            f"{candidate_path}: expected one Stage 5 frame "
            f"{EXPECTED_TEARDOWN_STAGE_FRAME} native teardown, got {len(teardowns)}"
        )
    teardown = teardowns[0]

    _require_fields(
        candidate_path,
        [origin, *native_samples.values(), teardown],
        ("frame", "stage", "stage_frame", "valid", "cumulative", *NATIVE_COUNTER_FIELDS),
    )

    rules = RuleBook()
    origin_values = _native_counters(origin, candidate_path)
    rules.check(
        "native_origin_metadata",
        "the last pre-sample Stage 5 frame-zero MARK is a valid device-lifetime snapshot",
        _native_metadata_valid(origin),
        observed={field: origin.get(field) for field in ("valid", "counter_scope", "cumulative", "counter_bits")},
        expected={"valid": 1, "counter_scope": "device_lifetime", "cumulative": 1, "counter_bits": "32_wrap"},
    )
    rules.check(
        "native_origin_zero",
        "the fixed Stage 5 route begins with zero native counters",
        all(value == 0 for value in origin_values.values()),
        observed=origin_values,
        expected={field: 0 for field in NATIVE_COUNTER_FIELDS},
    )

    previous = origin_values
    native_intervals: list[dict[str, Any]] = []
    total_submits = 0
    for key, baseline_record, candidate_record in zip(keys, baseline, candidate):
        native = native_samples[key]
        values = _native_counters(native, candidate_path)
        rules.check(
            "render_telemetry_valid",
            "both general render snapshots are complete",
            baseline_record["render_perf_valid"] == 1
            and candidate_record["render_perf_valid"] == 1,
            key=key,
            observed={"baseline": baseline_record["render_perf_valid"], "candidate": candidate_record["render_perf_valid"]},
            expected={"baseline": 1, "candidate": 1},
        )
        rules.check(
            "replay_context",
            "both fixed-route inputs are replay-driven with identical demo context",
            baseline_record["replay"] == candidate_record["replay"] == 1
            and baseline_record["demo"] == candidate_record["demo"],
            key=key,
            observed={"baseline_replay": baseline_record["replay"], "candidate_replay": candidate_record["replay"], "baseline_demo": baseline_record["demo"], "candidate_demo": candidate_record["demo"]},
            expected="replay=1 on both; demo identical",
        )
        rules.check(
            "native_sample_metadata",
            "every native SAMPLE is a valid cumulative device-lifetime snapshot",
            _native_metadata_valid(native),
            key=key,
            observed={field: native.get(field) for field in ("valid", "counter_scope", "cumulative", "counter_bits")},
            expected={"valid": 1, "counter_scope": "device_lifetime", "cumulative": 1, "counter_bits": "32_wrap"},
        )
        rules.check(
            "native_record_precedes_snapshot",
            "the native record is immediately persisted before its general SAMPLE",
            int(native["line"]) + 1 == int(candidate_record["line"])
            and native["frame"] == candidate_record["frame"],
            key=key,
            observed={"native_line": native["line"], "sample_line": candidate_record["line"], "native_frame": native["frame"], "sample_frame": candidate_record["frame"]},
            expected="native line immediately before SAMPLE with identical frame",
        )
        rules.check(
            "native_cumulative_accounting",
            "attempts equal submits plus client fallbacks at every sample",
            values["attempts"] == values["submits"] + values["client_fallbacks"],
            key=key,
            observed=values,
            expected="attempts=submits+client_fallbacks",
        )
        monotonic = all(values[field] >= previous[field] for field in NATIVE_COUNTER_FIELDS)
        rules.check(
            "native_counters_no_wrap",
            "the small r049 counter range is monotonic and never wraps",
            monotonic,
            key=key,
            observed={"previous": previous, "current": values},
            expected="all cumulative counters nondecreasing",
        )
        interval = {
            field: values[field] - previous[field] for field in NATIVE_COUNTER_FIELDS
        }
        calls = candidate_record["render_ascii_popup_batch_calls_total"]
        calls_peak = candidate_record["render_ascii_popup_batch_calls_peak"]
        rules.check(
            "popup_batch_call_shape",
            "the score frontend runs at most once per frame and has a valid interval peak",
            0 <= calls <= candidate_record["render_frames"]
            and calls_peak in (0, 1)
            and ((calls == 0) == (calls_peak == 0)),
            key=key,
            observed={"calls": calls, "calls_peak": calls_peak, "render_frames": candidate_record["render_frames"]},
            expected="0<=calls<=300; peak in {0,1}; zero total iff zero peak",
        )
        rules.check(
            "frontend_fallbacks_zero",
            "both matched score frontends retain zero canonical fallback",
            baseline_record["render_ascii_popup_batch_fallbacks_total"] == 0
            and baseline_record["render_ascii_popup_batch_fallbacks_peak"] == 0
            and candidate_record["render_ascii_popup_batch_fallbacks_total"] == 0
            and candidate_record["render_ascii_popup_batch_fallbacks_peak"] == 0,
            key=key,
            observed={
                "baseline_total": baseline_record["render_ascii_popup_batch_fallbacks_total"],
                "baseline_peak": baseline_record["render_ascii_popup_batch_fallbacks_peak"],
                "candidate_total": candidate_record["render_ascii_popup_batch_fallbacks_total"],
                "candidate_peak": candidate_record["render_ascii_popup_batch_fallbacks_peak"],
            },
            expected="all zero",
        )
        rules.check(
            "native_interval_matches_frontend",
            "every successful frontend batch makes exactly one accepted native submit",
            interval["attempts"] == calls
            and interval["submits"] == calls
            and interval["client_fallbacks"] == 0,
            key=key,
            observed={"native_interval": interval, "frontend_calls": calls},
            expected={"attempts": calls, "submits": calls, "client_fallbacks": 0},
        )

        emitted_total_saved = (
            baseline_record["render_state_emitted_total"]
            - candidate_record["render_state_emitted_total"]
        )
        expected_emitted_saved = interval["submits"] * 9
        rules.check(
            "emitted_state_total_exact",
            "each accepted native submit bypasses exactly nine client-array state calls",
            emitted_total_saved == expected_emitted_saved,
            key=key,
            observed=emitted_total_saved,
            expected=expected_emitted_saved,
        )
        emitted_peak_saved = (
            baseline_record["render_state_emitted_peak"]
            - candidate_record["render_state_emitted_peak"]
        )
        peak_valid = (
            emitted_peak_saved == 0
            if interval["submits"] == 0
            else 0 <= emitted_peak_saved <= 9
        )
        rules.check(
            "emitted_state_peak_bound",
            "a frame has at most one native score batch, so its interval peak falls by zero through nine calls",
            peak_valid,
            key=key,
            observed=emitted_peak_saved,
            expected="0 with zero submits; otherwise 0..9",
        )
        total_submits += interval["submits"]
        native_intervals.append(
            {
                "stage": key[0],
                "stage_frame": key[1],
                "frontend_calls": calls,
                "native_attempts": interval["attempts"],
                "native_submits": interval["submits"],
                "native_client_fallbacks": interval["client_fallbacks"],
                "emitted_state_saved": emitted_total_saved,
                "expected_emitted_state_saved": expected_emitted_saved,
                "emitted_state_peak_saved": emitted_peak_saved,
            }
        )
        previous = values

    teardown_values = _native_counters(teardown, candidate_path)
    rules.check(
        "native_teardown_metadata",
        "the persisted Stage 5 teardown is a valid native snapshot",
        _native_metadata_valid(teardown),
        observed={field: teardown.get(field) for field in ("valid", "counter_scope", "cumulative", "counter_bits")},
        expected={"valid": 1, "counter_scope": "device_lifetime", "cumulative": 1, "counter_bits": "32_wrap"},
    )
    rules.check(
        "native_teardown_accounting",
        "teardown remains monotonic, accepted, and fallback-free",
        all(teardown_values[field] >= previous[field] for field in NATIVE_COUNTER_FIELDS)
        and teardown_values["attempts"]
        == teardown_values["submits"] + teardown_values["client_fallbacks"]
        and teardown_values["client_fallbacks"] == 0,
        observed={"last_sample": previous, "teardown": teardown_values},
        expected="monotonic; attempts=submits; client_fallbacks=0",
    )
    rules.check(
        "native_path_exercised",
        "the twenty measured windows contain at least one accepted native submit",
        total_submits > 0,
        observed=total_submits,
        expected=">0",
    )

    reconciliations = rules.finish()
    reconciliation_match = all(rule["passed"] for rule in reconciliations)
    comparison_valid = exact_workload_match and reconciliation_match

    timing = general["timing"]
    point_positive = timing["paired_improvement_ms_mean"] > 0.0
    lower_bound_positive = timing["bootstrap_95_low_ms"] > 0.0
    performance_gate_passed = point_positive and lower_bound_positive
    bootstrap_iterations_valid = (
        timing["bootstrap_iterations"] == REQUIRED_BOOTSTRAP_ITERATIONS
    )
    acceptance_passed = (
        comparison_valid and bootstrap_iterations_valid and performance_gate_passed
    )

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
        "native_reconciliation_match": reconciliation_match,
        "general_strict_workload_match": general["strict_workload_match"],
        "general_strict_note": (
            "the general comparator cannot reconcile the intentional nine-call "
            "state-emission reduction; any additional mismatch remains visible"
        ),
        "bootstrap_iterations_required": REQUIRED_BOOTSTRAP_ITERATIONS,
        "bootstrap_iterations_valid": bootstrap_iterations_valid,
        "performance_gate_passed": performance_gate_passed,
        "acceptance_passed": acceptance_passed,
        "workload_fields": {
            "manager": sorted(manager_fields),
            "manager_diagnostic": sorted(manager_diagnostics),
            "context": sorted(CONTEXT_FIELDS),
            "render_exact": sorted(exact_render_fields),
            "emitted_state_reconciled": sorted(EMITTED_STATE_FIELDS),
        },
        "workload_differences": workload_differences,
        "native_origin": {
            "line": origin["line"],
            "phase": origin.get("phase"),
            **origin_values,
        },
        "native_intervals": native_intervals,
        "native_total_submits_in_samples": total_submits,
        "native_teardown": {
            "line": teardown["line"],
            "phase": teardown.get("phase"),
            **teardown_values,
        },
        "reconciliations": reconciliations,
        "timing": timing,
        "pairs": general["pairs"],
    }


def _render_difference(difference: dict[str, Any]) -> str:
    text = (
        f"WORKLOAD category={difference['category']} field={difference['field']} "
        f"match={str(difference['match']).lower()} "
        f"mismatched_pairs={difference['mismatched_pairs']}"
    )
    if "baseline_sum" in difference:
        text += (
            f" baseline_sum={difference['baseline_sum']}"
            f" candidate_sum={difference['candidate_sum']}"
            f" candidate_minus_baseline={difference['candidate_minus_baseline']}"
            f" max_abs_pair_delta={difference['max_abs_pair_delta']}"
        )
    return text


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
        "native_reconciliation_match="
        f"{str(result['native_reconciliation_match']).lower()}",
        "general_strict_workload_match="
        f"{str(result['general_strict_workload_match']).lower()} "
        f"note={result['general_strict_note']}",
        "bootstrap_iterations_valid="
        f"{str(result['bootstrap_iterations_valid']).lower()} "
        f"required={result['bootstrap_iterations_required']}",
        f"performance_gate_passed={str(result['performance_gate_passed']).lower()}",
        f"acceptance_passed={str(result['acceptance_passed']).lower()}",
    ]
    for difference in result["workload_differences"]:
        lines.append(_render_difference(difference))
        for mismatch in difference["mismatches"]:
            lines.append(
                "  MISMATCH "
                f"stage={mismatch['stage']} stage_frame={mismatch['stage_frame']} "
                f"baseline={mismatch['baseline']!r} candidate={mismatch['candidate']!r}"
            )
    origin = result["native_origin"]
    lines.append(
        "NATIVE_ORIGIN "
        f"line={origin['line']} phase={origin['phase']} attempts={origin['attempts']} "
        f"submits={origin['submits']} client_fallbacks={origin['client_fallbacks']}"
    )
    for interval in result["native_intervals"]:
        lines.append(
            "NATIVE_INTERVAL "
            f"stage={interval['stage']} stage_frame={interval['stage_frame']} "
            f"frontend_calls={interval['frontend_calls']} "
            f"attempts={interval['native_attempts']} submits={interval['native_submits']} "
            f"client_fallbacks={interval['native_client_fallbacks']} "
            f"emitted_saved={interval['emitted_state_saved']} "
            f"expected_emitted_saved={interval['expected_emitted_state_saved']} "
            f"peak_saved={interval['emitted_state_peak_saved']}"
        )
    teardown = result["native_teardown"]
    lines.append(
        "NATIVE_TEARDOWN "
        f"line={teardown['line']} phase={teardown['phase']} "
        f"attempts={teardown['attempts']} submits={teardown['submits']} "
        f"client_fallbacks={teardown['client_fallbacks']}"
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
        description="Strictly gate the TH08 PSP score-popup native GE A/B"
    )
    parser.add_argument("baseline_log", type=Path)
    parser.add_argument("candidate_log", type=Path)
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

    try:
        result = compare_logs(
            args.baseline_log,
            args.candidate_log,
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
