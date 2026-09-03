#!/usr/bin/env python3
"""Paired A/B comparator for the TH08 PSP popup direct-pair frontend.

Both inputs must already use the accepted score-popup pair batch.  This tool
keeps every gameplay, logical-render and physical-render field exact, except
for FPS-overlay-owned raw vertex drift proven by the shared owner accounting.
Only the explicitly named ``render_ascii_popup_direct_*`` mechanism counters
may differ, and their complete savings algebra is checked per interval.
"""

from __future__ import annotations

import argparse
import json
import sys
from collections import OrderedDict
from pathlib import Path
from typing import Any

import compare_stage_relative_perf as stage_perf


SCHEMA = "th08_ascii_popup_direct_pair_perf_comparison_v1"

DIRECT_DIAGNOSTIC_FIELDS = frozenset(
    f"render_ascii_popup_direct_{metric}_{aggregate}"
    for metric in (
        "active_popups",
        "validation_quads_avoided",
        "validation_culls_avoided",
        "nearbyint_avoided",
    )
    for aggregate in ("total", "peak")
)

REQUIRED_BATCH_FIELDS = frozenset(
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
            raise AssertionError(f"rule {name!r} used with two descriptions")
        if condition:
            return
        violation: dict[str, Any] = {
            "observed": observed,
            "expected": expected,
        }
        if key is not None:
            violation["stage"] = key[0]
            violation["stage_frame"] = key[1]
        rule["violations"].append(violation)

    def finish(self) -> list[dict[str, Any]]:
        result = []
        for rule in self._rules.values():
            item = dict(rule)
            item["passed"] = not item["violations"]
            item["violation_count"] = len(item["violations"])
            result.append(item)
        return result


def _require_fields(
    path: Path, records: list[dict[str, Any]], fields: frozenset[str]
) -> None:
    for record in records:
        for field in fields:
            stage_perf._require_int(record, field, path)


def _differences(
    fields: list[str],
    category: str,
    keys: list[tuple[int, int]],
    baseline: list[dict[str, Any]],
    candidate: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    return [
        stage_perf.compare_field(field, category, keys, baseline, candidate)
        for field in fields
    ]


def _check_candidate_algebra(
    rules: RuleBook,
    key: tuple[int, int],
    candidate: dict[str, Any],
) -> None:
    prefix = "render_ascii_popup_direct_"
    direct = {
        field.removeprefix(prefix): candidate[field]
        for field in DIRECT_DIAGNOSTIC_FIELDS
    }
    negative = {name: value for name, value in direct.items() if value < 0}
    rules.check(
        "candidate_direct_counters_nonnegative",
        "direct-pair mechanism counters are unsigned workloads",
        not negative,
        key=key,
        observed=negative,
        expected="all >= 0",
    )

    digits_total = candidate["render_ascii_popup_batch_digits_total"]
    digits_peak = candidate["render_ascii_popup_batch_digits_peak"]
    sprites_total = candidate["render_ascii_popup_batch_sprites_total"]
    active_total = direct["active_popups_total"]
    active_peak = direct["active_popups_peak"]
    quads_total = direct["validation_quads_avoided_total"]
    quads_peak = direct["validation_quads_avoided_peak"]
    culls_total = direct["validation_culls_avoided_total"]
    culls_peak = direct["validation_culls_avoided_peak"]
    nearby_total = direct["nearbyint_avoided_total"]
    nearby_peak = direct["nearbyint_avoided_peak"]

    rules.check(
        "candidate_batch_authority",
        "direct work is defined for each validated digit, including culled glyphs",
        digits_total >= sprites_total
        and 0 <= active_total <= digits_total
        and 0 <= active_peak <= digits_peak,
        key=key,
        observed={
            "digits_total": digits_total,
            "visible_sprites_total": sprites_total,
            "active_popups_total": active_total,
            "digits_peak": digits_peak,
            "active_popups_peak": active_peak,
        },
        expected="visible sprites <= digits; active popups <= digits",
    )
    rules.check(
        "validation_geometry_exact",
        "one old full validation quad is avoided for every successful batch digit",
        quads_total == digits_total and quads_peak == digits_peak,
        key=key,
        observed={"total": quads_total, "peak": quads_peak},
        expected={"total": digits_total, "peak": digits_peak},
    )
    rules.check(
        "validation_cull_exact",
        "one old validation viewport-cull test is avoided for every successful batch digit",
        culls_total == digits_total and culls_peak == digits_peak,
        key=key,
        observed={"total": culls_total, "peak": culls_peak},
        expected={"total": digits_total, "peak": digits_peak},
    )

    expected_nearby_total = digits_total * 6 - active_total * 2
    if digits_peak == 0:
        peak_valid = nearby_peak == 0 and active_peak == 0
        peak_expected: Any = 0
    else:
        # The telemetry stores independent per-field maxima.  The exact total
        # is available above; for the peak, 1<=P<=D on the D-peak frame gives
        # 4D <= max(6D-2P) <= 6D-2 without assuming coincident P maxima.
        peak_valid = (
            active_peak >= 1
            and digits_peak * 4 <= nearby_peak <= digits_peak * 6 - 2
        )
        peak_expected = f"{digits_peak * 4}..{digits_peak * 6 - 2}"
    rules.check(
        "nearbyint_savings_exact",
        "old 8-per-digit minus direct 2X-per-digit/2Y-per-popup accounting must reconcile",
        nearby_total == expected_nearby_total and peak_valid,
        key=key,
        observed={"total": nearby_total, "peak": nearby_peak},
        expected={"total": expected_nearby_total, "peak": peak_expected},
    )


def compare_logs(
    baseline_path: Path,
    candidate_path: Path,
    *,
    bootstrap_iterations: int = stage_perf.DEFAULT_BOOTSTRAP_ITERATIONS,
    bootstrap_seed: int = stage_perf.DEFAULT_BOOTSTRAP_SEED,
    stage: int | None = None,
) -> dict[str, Any]:
    # Overlay-owned glyph count can legitimately follow measured FPS.  The
    # shared comparator proves raw=game+overlay and keeps game vertices exact.
    general = stage_perf.compare_logs(
        baseline_path,
        candidate_path,
        bootstrap_iterations=bootstrap_iterations,
        bootstrap_seed=bootstrap_seed,
        allow_fps_overlay_drift=True,
        stage=stage,
    )
    baseline = stage_perf.parse_stage_relative_samples(baseline_path)
    candidate = stage_perf.parse_stage_relative_samples(candidate_path)
    if stage is not None:
        baseline = stage_perf.select_fixed_stage_samples(
            baseline_path, baseline, stage
        )
        candidate = stage_perf.select_fixed_stage_samples(
            candidate_path, candidate, stage
        )
    keys = stage_perf.validate_samples(baseline_path, baseline)
    if keys != stage_perf.validate_samples(candidate_path, candidate):
        raise stage_perf.ComparisonError("stage/stage_frame alignment failure")

    required = DIRECT_DIAGNOSTIC_FIELDS | REQUIRED_BATCH_FIELDS | frozenset(
        {"render_perf_valid", "render_frames"}
    )
    _require_fields(baseline_path, baseline, required)
    _require_fields(candidate_path, candidate, required)

    all_fields = set().union(*(record.keys() for record in baseline + candidate))
    manager_fields = sorted(
        field for field in all_fields if stage_perf._manager_field(field)
    )
    context_fields = sorted(all_fields & stage_perf.LOGICAL_CONTEXT_FIELDS)
    overlay_fields = stage_perf.RAW_VERTEX_FIELDS | stage_perf.FPS_OVERLAY_VERTEX_FIELDS
    exact_render_fields = sorted(
        field
        for field in all_fields
        if stage_perf._render_workload_field(field)
        and field not in DIRECT_DIAGNOSTIC_FIELDS
        and field not in overlay_fields
    )
    if not manager_fields:
        raise stage_perf.ComparisonError("no gameplay manager count fields found")
    if not exact_render_fields:
        raise stage_perf.ComparisonError("no exact render workload fields found")

    workload_differences = (
        _differences(manager_fields, "manager", keys, baseline, candidate)
        + _differences(context_fields, "context", keys, baseline, candidate)
        + _differences(
            exact_render_fields, "render_exact", keys, baseline, candidate
        )
    )
    exact_workload_match = all(item["match"] for item in workload_differences)
    diagnostic_differences = _differences(
        sorted(DIRECT_DIAGNOSTIC_FIELDS),
        "direct_mechanism",
        keys,
        baseline,
        candidate,
    )

    rules = RuleBook()
    total_candidate_direct_quads = 0
    total_baseline_batch_calls = 0
    for key, baseline_record, candidate_record in zip(keys, baseline, candidate):
        rules.check(
            "render_telemetry_valid",
            "both runs must report complete render telemetry",
            baseline_record["render_perf_valid"] == 1
            and candidate_record["render_perf_valid"] == 1,
            key=key,
            observed={
                "baseline": baseline_record["render_perf_valid"],
                "candidate": candidate_record["render_perf_valid"],
            },
            expected={"baseline": 1, "candidate": 1},
        )
        baseline_nonzero = {
            field: baseline_record[field]
            for field in sorted(DIRECT_DIAGNOSTIC_FIELDS)
            if baseline_record[field] != 0
        }
        rules.check(
            "baseline_direct_counters_zero",
            "the control must have only the direct-pair gate disabled",
            not baseline_nonzero,
            key=key,
            observed=baseline_nonzero,
            expected="all direct mechanism counters zero",
        )
        _check_candidate_algebra(rules, key, candidate_record)
        total_candidate_direct_quads += candidate_record[
            "render_ascii_popup_direct_validation_quads_avoided_total"
        ]
        total_baseline_batch_calls += baseline_record[
            "render_ascii_popup_batch_calls_total"
        ]

    rules.check(
        "paired_features_exercised",
        "the control batch and candidate direct frontend must both execute",
        total_baseline_batch_calls > 0 and total_candidate_direct_quads > 0,
        observed={
            "baseline_batch_calls": total_baseline_batch_calls,
            "candidate_direct_quads": total_candidate_direct_quads,
        },
        expected="both > 0",
    )
    reconciliations = rules.finish()
    mechanism_reconciliation_match = all(
        item["passed"] for item in reconciliations
    )
    overlay_match = general["fps_overlay_vertex_reconciliation_match"]
    comparison_valid = (
        exact_workload_match
        and mechanism_reconciliation_match
        and overlay_match
        and general["render_telemetry_valid"]
    )

    return {
        "schema": SCHEMA,
        "baseline_path": str(baseline_path),
        "candidate_path": str(candidate_path),
        "aligned": True,
        "sample_count": len(keys),
        "stage_filter": stage,
        "comparison_valid": comparison_valid,
        "exact_workload_match": exact_workload_match,
        "mechanism_reconciliation_match": mechanism_reconciliation_match,
        "fps_overlay_vertex_reconciliation_match": overlay_match,
        "general_strict_workload_match": general["strict_workload_match"],
        "workload_fields": {
            "manager": manager_fields,
            "context": context_fields,
            "render_exact": exact_render_fields,
            "direct_mechanism": sorted(DIRECT_DIAGNOSTIC_FIELDS),
        },
        "workload_differences": workload_differences,
        "diagnostic_differences": diagnostic_differences,
        "reconciliations": reconciliations,
        "timing": general["timing"],
        "pairs": general["pairs"],
    }


def render_text(result: dict[str, Any]) -> str:
    timing = result["timing"]
    lines = [
        f"schema={result['schema']}",
        f"baseline={result['baseline_path']}",
        f"candidate={result['candidate_path']}",
        f"aligned={str(result['aligned']).lower()} samples={result['sample_count']}",
        f"stage_filter={result['stage_filter']!r}",
        f"comparison_valid={str(result['comparison_valid']).lower()}",
        f"exact_workload_match={str(result['exact_workload_match']).lower()}",
        "mechanism_reconciliation_match="
        f"{str(result['mechanism_reconciliation_match']).lower()}",
        "fps_overlay_vertex_reconciliation_match="
        f"{str(result['fps_overlay_vertex_reconciliation_match']).lower()}",
        "general_strict_workload_match="
        f"{str(result['general_strict_workload_match']).lower()} "
        "note=expected_false_when_direct_mechanism_counters_are_exercised",
    ]
    for difference in result["workload_differences"]:
        lines.append(
            f"WORKLOAD category={difference['category']} field={difference['field']} "
            f"match={str(difference['match']).lower()} "
            f"mismatched_pairs={difference['mismatched_pairs']}"
        )
    for rule in result["reconciliations"]:
        lines.append(
            f"RECONCILE rule={rule['rule']} "
            f"passed={str(rule['passed']).lower()} "
            f"violations={rule['violation_count']} "
            f"description={rule['description']}"
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
            f"improvement_ms={pair['improvement_ms']:+.9f} "
            f"result={pair['result']}"
        )
    lines.append(
        "TIMING "
        f"paired_improvement_ms_mean={timing['paired_improvement_ms_mean']:+.9f} "
        f"bootstrap_95_low_ms={timing['bootstrap_95_low_ms']:+.9f} "
        f"bootstrap_95_high_ms={timing['bootstrap_95_high_ms']:+.9f}"
    )
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Compare TH08 PSP popup-batch vs popup direct-pair A/B logs"
    )
    parser.add_argument("baseline_log", type=Path)
    parser.add_argument("candidate_log", type=Path)
    parser.add_argument(
        "--bootstrap-iterations",
        type=int,
        default=stage_perf.DEFAULT_BOOTSTRAP_ITERATIONS,
    )
    parser.add_argument("--bootstrap-seed", type=int, default=12345)
    parser.add_argument("--stage", type=int)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)
    try:
        result = compare_logs(
            args.baseline_log,
            args.candidate_log,
            bootstrap_iterations=args.bootstrap_iterations,
            bootstrap_seed=args.bootstrap_seed,
            stage=args.stage,
        )
    except (OSError, ValueError, stage_perf.ComparisonError) as error:
        print(f"alignment failure: {error}", file=sys.stderr)
        return 2
    if args.json:
        json.dump(result, sys.stdout, indent=2, sort_keys=True)
        sys.stdout.write("\n")
    else:
        sys.stdout.write(render_text(result))
    return 0 if result["comparison_valid"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
