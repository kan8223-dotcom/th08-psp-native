#!/usr/bin/env python3
"""Strict OFF/ON Stage-5 gate for conservative Bullet cancel coverage."""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path
from typing import Any

import bullet_cancel_spatial_contract as contract
import compare_stage_relative_perf as stage_perf


SCHEMA = "th08_bullet_cancel_spatial_perf_comparison_v1"


def _records(path: Path, prefix: str) -> list[dict[str, Any]]:
    result: list[dict[str, Any]] = []
    marker = prefix + " "
    for line_number, raw in enumerate(
        path.read_text(encoding="utf-8", errors="replace").splitlines(), 1
    ):
        start = raw.find(marker)
        if start < 0:
            continue
        fields = raw[start:].split()
        record: dict[str, Any] = {"record": prefix, "line": line_number}
        for token in fields[1:]:
            if "=" not in token:
                continue
            key, value = token.split("=", 1)
            if key in record:
                raise stage_perf.ComparisonError(
                    f"{path}:{line_number}: duplicate {prefix} field {key!r}"
                )
            record[key] = stage_perf.parse_scalar(value)
        result.append(record)
    return result


def _feature(path: Path) -> dict[str, Any]:
    records = _records(path, "FEATURE")
    if len(records) != 1:
        raise stage_perf.ComparisonError(
            f"{path}: expected one FEATURE fingerprint, got {len(records)}"
        )
    return records[0]


def _telemetry_by_key(path: Path) -> dict[tuple[int, int], dict[str, Any]]:
    result: dict[tuple[int, int], dict[str, Any]] = {}
    for record in _records(path, contract.TELEMETRY_PREFIX):
        if record.get("kind") != "SAMPLE" or record.get("phase") != "stage_relative_periodic":
            raise stage_perf.ComparisonError(
                f"{path}:{record['line']}: cancel telemetry must be SAMPLE-only stage-relative data"
            )
        stage = stage_perf._require_int(record, "stage", path)
        stage_frame = stage_perf._require_int(record, "stage_frame", path)
        if stage != 5:
            continue
        key = (stage, stage_frame)
        if key in result:
            raise stage_perf.ComparisonError(f"{path}: duplicate cancel telemetry {key}")
        result[key] = record
    if tuple(result) != contract.EXPECTED_KEYS:
        raise stage_perf.ComparisonError(
            f"{path}: cancel telemetry route {tuple(result)} != {contract.EXPECTED_KEYS}"
        )
    return result


def _hash(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def compare_logs(
    control_path: Path,
    candidate_path: Path,
    *,
    surface_pairs: list[tuple[str, Path, Path]],
    control_feature_path: Path | None = None,
    candidate_feature_path: Path | None = None,
    bootstrap_iterations: int = contract.REQUIRED_BOOTSTRAP_ITERATIONS,
    bootstrap_seed: int = stage_perf.DEFAULT_BOOTSTRAP_SEED,
) -> dict[str, Any]:
    # Runtime performance samples live in TH08PSP_MEMORY.LOG, while the build
    # fingerprint is deliberately written to TH08PSP_BOOT.LOG.  Unit fixtures
    # may still keep both record types in one file, hence the backwards-
    # compatible defaults.
    control_feature_source = control_feature_path or control_path
    candidate_feature_source = candidate_feature_path or candidate_path
    control_feature = _feature(control_feature_source)
    candidate_feature = _feature(candidate_feature_source)
    if control_feature.get(contract.FEATURE_FIELD) != 0:
        raise stage_perf.ComparisonError("control BULLET_CANCEL_SPATIAL must be 0")
    if candidate_feature.get(contract.FEATURE_FIELD) != 1:
        raise stage_perf.ComparisonError("candidate BULLET_CANCEL_SPATIAL must be 1")
    control_identity = {
        key: value for key, value in control_feature.items()
        if key not in {"line", "record", contract.FEATURE_FIELD}
    }
    candidate_identity = {
        key: value for key, value in candidate_feature.items()
        if key not in {"line", "record", contract.FEATURE_FIELD}
    }
    if control_identity != candidate_identity:
        raise stage_perf.ComparisonError("non-target FEATURE fingerprint differs")

    if _records(control_path, contract.TELEMETRY_PREFIX):
        raise stage_perf.ComparisonError("OFF control contains cancel product telemetry")
    telemetry = _telemetry_by_key(candidate_path)

    control = stage_perf.select_fixed_stage_samples(
        control_path, stage_perf.parse_stage_relative_samples(control_path), 5
    )
    candidate = stage_perf.select_fixed_stage_samples(
        candidate_path, stage_perf.parse_stage_relative_samples(candidate_path), 5
    )
    control_keys = stage_perf.validate_samples(control_path, control)
    candidate_keys = stage_perf.validate_samples(candidate_path, candidate)
    if tuple(control_keys) != contract.EXPECTED_KEYS or control_keys != candidate_keys:
        raise stage_perf.ComparisonError("control/candidate fixed Stage-5 route mismatch")

    general = stage_perf.compare_logs(
        control_path, candidate_path, stage=5,
        allow_fps_overlay_drift=True,
        bootstrap_iterations=bootstrap_iterations,
        bootstrap_seed=bootstrap_seed,
    )
    diagnostic_diffs = [
        stage_perf.compare_field(field, "manager_diagnostic", control_keys, control, candidate)
        for field in sorted(stage_perf.MANAGER_DIAGNOSTIC_FIELDS)
        if any(field in record for record in control + candidate)
    ]
    exact_workload = general["strict_workload_match"] and all(
        item["match"] for item in diagnostic_diffs
    )

    violations: list[dict[str, Any]] = []
    totals = {field: 0 for field in contract.INTERVAL_FIELDS}
    for key, general_record in zip(control_keys, candidate):
        record = telemetry[key]
        if record.get("frame") != general_record.get("frame") or record["line"] >= general_record["line"]:
            violations.append({"key": key, "rule": "sample_alignment"})
        metadata_ok = (
            record.get("valid") == 1 and record.get("mode") == "product"
            and record.get("counter_scope") == "stage_relative_interval"
            and record.get("cumulative") == 0
            and record.get("interval_consumed") == 1
            and record.get("conservative_coverage") == 1
            and record.get("canonical_slot_order") == 1
        )
        if not metadata_ok:
            violations.append({"key": key, "rule": "metadata"})
        values: dict[str, int] = {}
        for field in contract.INTERVAL_FIELDS:
            value = stage_perf._require_int(record, field, candidate_path)
            values[field] = value
            totals[field] += value
            if value < 0:
                violations.append({"key": key, "rule": "nonnegative", "field": field})
        if values["calls"] != (
            values["indexed_queries"] + values["rejected_queries"] + values["fallbacks"]
        ):
            violations.append({"key": key, "rule": "all_call_attribution"})
        selected = values["indexed_candidates"] + values["fallback_candidates"]
        if selected > values["full_candidates"] or values["exact_tests"] > selected:
            violations.append({"key": key, "rule": "candidate_exact_algebra"})
        if values["false_positives"] > values["indexed_queries"]:
            violations.append({"key": key, "rule": "false_positive_bound"})
        if values["duplicate_pairs"] != (
            values["duplicate_replays"] + values["duplicate_fallbacks"]
        ) or values["duplicate_exact_tests_saved"] != values["duplicate_replays"]:
            violations.append({"key": key, "rule": "duplicate_reconciliation"})
        if values["unsupported_region_fallbacks"] != 0 or values["nonfinite_fallbacks"] != 0:
            violations.append({"key": key, "rule": "geometry_fallbacks_zero"})
        if values["duplicate_fallbacks"] != 0:
            violations.append({"key": key, "rule": "duplicate_fallbacks_zero"})

    if totals["calls"] <= 0 or totals["rejected_queries"] <= 0:
        violations.append({"rule": "product_exercised"})

    if not surface_pairs:
        raise stage_perf.ComparisonError("at least one OFF/ON surface pair is required")
    surfaces = []
    for label, control_surface, candidate_surface in surface_pairs:
        control_sha = _hash(control_surface)
        candidate_sha = _hash(candidate_surface)
        surfaces.append({"label": label, "control_sha256": control_sha,
                         "candidate_sha256": candidate_sha,
                         "match": control_sha == candidate_sha})
    surfaces_match = all(item["match"] for item in surfaces)
    if not surfaces_match:
        violations.append({"rule": "surface_identity"})

    timing = general["timing"]
    performance_passed = (
        bootstrap_iterations == contract.REQUIRED_BOOTSTRAP_ITERATIONS
        and timing["paired_improvement_ms_mean"] > 0.0
        and timing["bootstrap_95_low_ms"] > 0.0
    )
    comparison_valid = exact_workload and surfaces_match and not violations
    return {
        "schema": SCHEMA,
        "feature_macro": contract.FEATURE_MACRO,
        "fixed_route": list(contract.EXPECTED_KEYS),
        "exact_workload_match": exact_workload,
        "manager_diagnostic_differences": diagnostic_diffs,
        "telemetry_totals": totals,
        "violations": violations,
        "surfaces": surfaces,
        "timing": timing,
        "comparison_valid": comparison_valid,
        "performance_gate_passed": performance_passed,
        "acceptance_passed": comparison_valid and performance_passed,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("control_log", type=Path)
    parser.add_argument("candidate_log", type=Path)
    parser.add_argument("--control-feature-log", type=Path)
    parser.add_argument("--candidate-feature-log", type=Path)
    parser.add_argument("--surface", nargs=3, action="append", metavar=("LABEL", "OFF", "ON"), required=True)
    parser.add_argument("--bootstrap-iterations", type=int, default=contract.REQUIRED_BOOTSTRAP_ITERATIONS)
    parser.add_argument("--seed", type=lambda text: int(text, 0), default=stage_perf.DEFAULT_BOOTSTRAP_SEED)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)
    try:
        result = compare_logs(
            args.control_log, args.candidate_log,
            surface_pairs=[(label, Path(off), Path(on)) for label, off, on in args.surface],
            control_feature_path=args.control_feature_log,
            candidate_feature_path=args.candidate_feature_log,
            bootstrap_iterations=args.bootstrap_iterations,
            bootstrap_seed=args.seed,
        )
    except (OSError, stage_perf.ComparisonError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 2
    output = json.dumps(result, ensure_ascii=False, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.write_text(output, encoding="utf-8")
    print(output, end="")
    return 0 if result["acceptance_passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
