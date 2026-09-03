#!/usr/bin/env python3
"""Paired A/B comparator for the TH08 PSP score-popup sprite batch.

The general stage-relative comparator intentionally treats every render-counter
difference as a mismatch.  This dedicated comparator keeps that tool strict and
accepts only the differences mechanically implied by
``TH08_PSP_ASCII_POPUP_BATCH``:

* each visible score glyph removes four game-owned submitted vertices;
* raw vertices equal game-owned plus FPS-overlay-owned vertices in each run,
  so wall-clock overlay digit drift cannot hide or falsely reject the saving;
* isolating the score batch can split one compatible ANM run at either edge;
* every additional textured draw has the corresponding frontend state calls,
  cached PSPGL state/client-array calls, and one clean texture-upload attempt.

All gameplay-manager and logical-render counters remain exact.  Positive
``improvement_ms`` means that the candidate used less wall-clock time per
frame than the baseline.
"""

from __future__ import annotations

import argparse
import json
import sys
from collections import OrderedDict
from pathlib import Path
from typing import Any

import compare_stage_relative_perf as stage_perf


SCHEMA = "th08_ascii_popup_batch_perf_comparison_v1"

BATCH_DIAGNOSTIC_FIELDS = frozenset(
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

# These are the only ordinary render counters that the implementation may
# change.  Totals have exact algebra below; interval peaks have conservative
# bounds because the telemetry does not retain which of the 300 frames owned
# each maximum.
PHYSICAL_RENDER_FIELDS = frozenset(
    {
        "render_draws_total",
        "render_draws_peak",
        "render_vertices_total",
        "render_vertices_peak",
        "render_fps_overlay_vertices_total",
        "render_fps_overlay_vertices_peak",
        "render_game_vertices_total",
        "render_game_vertices_peak",
        "render_state_requested_total",
        "render_state_requested_peak",
        "render_state_emitted_total",
        "render_state_emitted_peak",
        "render_upload_attempt_total",
        "render_upload_attempt_peak",
    }
)

LOGICAL_CONTEXT_FIELDS = stage_perf.LOGICAL_CONTEXT_FIELDS


class RuleBook:
    """Collect named reconciliation rules without hiding any failing pair."""

    def __init__(self) -> None:
        self._rules: OrderedDict[str, dict[str, Any]] = OrderedDict()

    def check(
        self,
        name: str,
        description: str,
        condition: bool,
        *,
        stage: int | None = None,
        stage_frame: int | None = None,
        observed: Any = None,
        expected: Any = None,
    ) -> None:
        rule = self._rules.setdefault(
            name,
            {"rule": name, "description": description, "violations": []},
        )
        if rule["description"] != description:
            raise AssertionError(f"rule {name!r} used with two descriptions")
        if condition:
            return
        violation: dict[str, Any] = {
            "observed": observed,
            "expected": expected,
        }
        if stage is not None:
            violation["stage"] = stage
        if stage_frame is not None:
            violation["stage_frame"] = stage_frame
        rule["violations"].append(violation)

    def finish(self) -> list[dict[str, Any]]:
        finished: list[dict[str, Any]] = []
        for rule in self._rules.values():
            item = dict(rule)
            item["passed"] = not item["violations"]
            item["violation_count"] = len(item["violations"])
            finished.append(item)
        return finished


def _require_int(record: dict[str, Any], field: str, path: Path) -> int:
    return stage_perf._require_int(record, field, path)


def _require_required_fields(
    path: Path, samples: list[dict[str, Any]], fields: frozenset[str]
) -> None:
    for record in samples:
        for field in fields:
            _require_int(record, field, path)


def _logical_render_field(name: str) -> bool:
    return (
        stage_perf._render_workload_field(name)
        and name not in BATCH_DIAGNOSTIC_FIELDS
        and name not in PHYSICAL_RENDER_FIELDS
    )


def _field_differences(
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


def _check_zero_baseline_diagnostics(
    rules: RuleBook,
    key: tuple[int, int],
    baseline_record: dict[str, Any],
) -> None:
    nonzero = {
        field: baseline_record[field]
        for field in sorted(BATCH_DIAGNOSTIC_FIELDS)
        if baseline_record[field] != 0
    }
    rules.check(
        "baseline_batch_diagnostics_zero",
        "the control build must have every popup-batch diagnostic equal to zero",
        not nonzero,
        stage=key[0],
        stage_frame=key[1],
        observed=nonzero,
        expected="all zero",
    )


def _check_candidate_diagnostics(
    rules: RuleBook,
    key: tuple[int, int],
    candidate_record: dict[str, Any],
    render_frames: int,
) -> None:
    prefix = "render_ascii_popup_batch_"
    values = {
        field.removeprefix(prefix): candidate_record[field]
        for field in BATCH_DIAGNOSTIC_FIELDS
    }
    negative = {name: value for name, value in values.items() if value < 0}
    rules.check(
        "candidate_diagnostics_nonnegative",
        "popup-batch counters are unsigned workloads",
        not negative,
        stage=key[0],
        stage_frame=key[1],
        observed=negative,
        expected="all >= 0",
    )

    calls_total = values["calls_total"]
    calls_peak = values["calls_peak"]
    digits_total = values["digits_total"]
    digits_peak = values["digits_peak"]
    sprites_total = values["sprites_total"]
    sprites_peak = values["sprites_peak"]
    saved_total = values["vertices_saved_total"]
    saved_peak = values["vertices_saved_peak"]
    bytes_total = values["bytes_saved_total"]
    bytes_peak = values["bytes_saved_peak"]
    fallbacks_total = values["fallbacks_total"]
    fallbacks_peak = values["fallbacks_peak"]

    rules.check(
        "candidate_call_accounting",
        "one score-batch attempt can succeed or fall back at most once per frame",
        calls_total + fallbacks_total <= render_frames
        and calls_peak in (0, 1)
        and fallbacks_peak in (0, 1)
        and (calls_total == 0) == (calls_peak == 0)
        and (fallbacks_total == 0) == (fallbacks_peak == 0),
        stage=key[0],
        stage_frame=key[1],
        observed={
            "frames": render_frames,
            "calls_total": calls_total,
            "calls_peak": calls_peak,
            "fallbacks_total": fallbacks_total,
            "fallbacks_peak": fallbacks_peak,
        },
        expected="calls+fallbacks <= frames; nonzero total iff peak=1",
    )
    rules.check(
        "candidate_digit_sprite_accounting",
        "every success has digits and visible sprites are a subset of those digits",
        digits_total >= calls_total
        and (calls_total == 0) == (digits_total == 0)
        and digits_total >= sprites_total
        and digits_peak >= sprites_peak
        and digits_peak <= digits_total
        and sprites_peak <= sprites_total
        and (digits_total == 0) == (digits_peak == 0)
        and (sprites_total == 0) == (sprites_peak == 0),
        stage=key[0],
        stage_frame=key[1],
        observed={
            "calls_total": calls_total,
            "digits_total": digits_total,
            "digits_peak": digits_peak,
            "sprites_total": sprites_total,
            "sprites_peak": sprites_peak,
        },
        expected=(
            "calls and digits become nonzero together; calls <= digits; "
            "visible sprites <= digits; valid total/peak pairs"
        ),
    )
    rules.check(
        "candidate_saved_vertex_accounting",
        "each visible sprite replaces six canonical vertices with two",
        saved_total == sprites_total * 4
        and saved_peak == sprites_peak * 4,
        stage=key[0],
        stage_frame=key[1],
        observed={
            "sprites_total": sprites_total,
            "sprites_peak": sprites_peak,
            "vertices_saved_total": saved_total,
            "vertices_saved_peak": saved_peak,
        },
        expected={
            "vertices_saved_total": sprites_total * 4,
            "vertices_saved_peak": sprites_peak * 4,
        },
    )
    rules.check(
        "candidate_saved_byte_accounting",
        "saved bytes count the omitted 28-byte frontend vertices",
        bytes_total == saved_total * 28 and bytes_peak == saved_peak * 28,
        stage=key[0],
        stage_frame=key[1],
        observed={"bytes_total": bytes_total, "bytes_peak": bytes_peak},
        expected={"bytes_total": saved_total * 28, "bytes_peak": saved_peak * 28},
    )
    rules.check(
        "candidate_batch_is_score_popup_subset",
        "batched score digits must be contained in the canonical all-popup counters",
        digits_total <= candidate_record["render_popup_digits_total"]
        and digits_peak <= candidate_record["render_popup_digits_peak"],
        stage=key[0],
        stage_frame=key[1],
        observed={"digits_total": digits_total, "digits_peak": digits_peak},
        expected={
            "at_most_popup_digits_total": candidate_record[
                "render_popup_digits_total"
            ],
            "at_most_popup_digits_peak": candidate_record[
                "render_popup_digits_peak"
            ],
        },
    )


def _check_physical_render_delta(
    rules: RuleBook,
    key: tuple[int, int],
    baseline: dict[str, Any],
    candidate: dict[str, Any],
    vertex_accounting: dict[str, Any],
) -> None:
    saved_total = candidate["render_ascii_popup_batch_vertices_saved_total"]
    saved_peak = candidate["render_ascii_popup_batch_vertices_saved_peak"]
    calls_total = candidate["render_ascii_popup_batch_calls_total"]
    sprites_total = candidate["render_ascii_popup_batch_sprites_total"]

    rules.check(
        "fps_overlay_vertex_accounting",
        "each run must have raw=game+overlay totals and legal independent peak bounds",
        vertex_accounting["baseline_integrity"]
        and vertex_accounting["candidate_integrity"],
        stage=key[0],
        stage_frame=key[1],
        observed={
            "baseline": vertex_accounting["baseline"],
            "candidate": vertex_accounting["candidate"],
        },
        expected=(
            "raw_total=game_total+overlay_total; nonnegative canonical overlay; "
            "max(game_peak,overlay_peak)<=raw_peak<=game_peak+overlay_peak"
        ),
    )

    vertex_delta = baseline["render_game_vertices_total"] - candidate[
        "render_game_vertices_total"
    ]
    rules.check(
        "submitted_vertices_exact",
        "game-owned submitted vertices must fall by exactly the popup saved-vertex total",
        vertex_delta == saved_total,
        stage=key[0],
        stage_frame=key[1],
        observed=vertex_delta,
        expected=saved_total,
    )
    vertex_peak_delta = baseline["render_game_vertices_peak"] - candidate[
        "render_game_vertices_peak"
    ]
    rules.check(
        "submitted_vertices_peak_bound",
        "the game-owned interval maximum can fall by no more than the largest per-frame saving",
        0 <= vertex_peak_delta <= saved_peak,
        stage=key[0],
        stage_frame=key[1],
        observed=vertex_peak_delta,
        expected=f"0..{saved_peak}",
    )

    raw_delta = baseline["render_vertices_total"] - candidate[
        "render_vertices_total"
    ]
    overlay_delta = baseline["render_fps_overlay_vertices_total"] - candidate[
        "render_fps_overlay_vertices_total"
    ]
    rules.check(
        "raw_vertex_delta_explained",
        "raw A/B vertex drift must equal popup game savings plus the measured FPS-overlay drift",
        raw_delta == saved_total + overlay_delta,
        stage=key[0],
        stage_frame=key[1],
        observed={
            "raw_baseline_minus_candidate": raw_delta,
            "overlay_baseline_minus_candidate": overlay_delta,
        },
        expected={
            "raw_baseline_minus_candidate": saved_total + overlay_delta,
            "popup_game_vertices_saved": saved_total,
        },
    )

    draw_delta = candidate["render_draws_total"] - baseline[
        "render_draws_total"
    ]
    # One successful visible batch replaces the canonical score-containing
    # draw and can split compatible geometry once before and once after it.
    visible_frame_upper = min(calls_total, sprites_total)
    draw_delta_upper = visible_frame_upper * 2
    rules.check(
        "draw_total_split_bound",
        "a visible score batch may add only the two edge splits around its replacement draw",
        0 <= draw_delta <= draw_delta_upper,
        stage=key[0],
        stage_frame=key[1],
        observed=draw_delta,
        expected=f"0..{draw_delta_upper}",
    )
    draw_peak_delta = candidate["render_draws_peak"] - baseline[
        "render_draws_peak"
    ]
    per_frame_draw_upper = 2 if sprites_total else 0
    rules.check(
        "draw_peak_split_bound",
        "the candidate adds at most two draw boundaries in any frame",
        0 <= draw_peak_delta <= per_frame_draw_upper,
        stage=key[0],
        stage_frame=key[1],
        observed=draw_peak_delta,
        expected=f"0..{per_frame_draw_upper}",
    )

    requested_delta = candidate["render_state_requested_total"] - baseline[
        "render_state_requested_total"
    ]
    rules.check(
        "state_requested_total_exact",
        "every added ANM/direct draw has exactly two texture-stage setters and one FVF setter",
        requested_delta == draw_delta * 3,
        stage=key[0],
        stage_frame=key[1],
        observed=requested_delta,
        expected=draw_delta * 3,
    )
    requested_peak_delta = candidate["render_state_requested_peak"] - baseline[
        "render_state_requested_peak"
    ]
    rules.check(
        "state_requested_peak_bound",
        "two possible per-frame edge splits add at most six frontend state requests",
        0 <= requested_peak_delta <= per_frame_draw_upper * 3,
        stage=key[0],
        stage_frame=key[1],
        observed=requested_peak_delta,
        expected=f"0..{per_frame_draw_upper * 3}",
    )

    emitted_delta = candidate["render_state_emitted_total"] - baseline[
        "render_state_emitted_total"
    ]
    rules.check(
        "state_emitted_total_exact",
        "with the accepted state cache each added textured draw emits one bind and nine client-array calls",
        emitted_delta == draw_delta * 10,
        stage=key[0],
        stage_frame=key[1],
        observed=emitted_delta,
        expected=draw_delta * 10,
    )
    emitted_peak_delta = candidate["render_state_emitted_peak"] - baseline[
        "render_state_emitted_peak"
    ]
    rules.check(
        "state_emitted_peak_bound",
        "two possible edge splits add at most twenty cached PSPGL/client-array calls",
        0 <= emitted_peak_delta <= per_frame_draw_upper * 10,
        stage=key[0],
        stage_frame=key[1],
        observed=emitted_peak_delta,
        expected=f"0..{per_frame_draw_upper * 10}",
    )

    upload_attempt_delta = candidate["render_upload_attempt_total"] - baseline[
        "render_upload_attempt_total"
    ]
    rules.check(
        "clean_upload_attempt_total_exact",
        "each additional textured draw performs one clean texture Upload entry attempt",
        upload_attempt_delta == draw_delta,
        stage=key[0],
        stage_frame=key[1],
        observed=upload_attempt_delta,
        expected=draw_delta,
    )
    upload_attempt_peak_delta = candidate[
        "render_upload_attempt_peak"
    ] - baseline["render_upload_attempt_peak"]
    rules.check(
        "clean_upload_attempt_peak_bound",
        "two possible edge splits add at most two clean upload attempts per frame",
        0 <= upload_attempt_peak_delta <= per_frame_draw_upper,
        stage=key[0],
        stage_frame=key[1],
        observed=upload_attempt_peak_delta,
        expected=f"0..{per_frame_draw_upper}",
    )


def compare_logs(
    baseline_path: Path,
    candidate_path: Path,
    *,
    bootstrap_iterations: int = stage_perf.DEFAULT_BOOTSTRAP_ITERATIONS,
    bootstrap_seed: int = stage_perf.DEFAULT_BOOTSTRAP_SEED,
    tie_epsilon_ms: float = 0.0,
    stage: int | None = None,
) -> dict[str, Any]:
    # Reuse the established parser, alignment validation, paired timing, and
    # fixed-seed bootstrap.  Its strict result is retained for transparency,
    # but this experiment is judged by the narrower proof below.
    general = stage_perf.compare_logs(
        baseline_path,
        candidate_path,
        bootstrap_iterations=bootstrap_iterations,
        bootstrap_seed=bootstrap_seed,
        tie_epsilon_ms=tie_epsilon_ms,
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
    baseline_keys = stage_perf.validate_samples(baseline_path, baseline)
    candidate_keys = stage_perf.validate_samples(candidate_path, candidate)
    if baseline_keys != candidate_keys:
        # The general comparator normally raises first; keep this explicit if
        # its implementation ever changes.
        raise stage_perf.ComparisonError("stage/stage_frame alignment failure")

    required = BATCH_DIAGNOSTIC_FIELDS | PHYSICAL_RENDER_FIELDS | frozenset(
        {
            "render_popup_digits_total",
            "render_popup_digits_peak",
            "render_perf_valid",
            "render_frames",
        }
    )
    _require_required_fields(baseline_path, baseline, required)
    _require_required_fields(candidate_path, candidate, required)
    vertex_accounting_results = stage_perf.fps_overlay_vertex_reconciliations(
        baseline_keys,
        baseline,
        candidate,
        baseline_path,
        candidate_path,
    )

    all_fields = set().union(*(record.keys() for record in baseline + candidate))
    manager_fields = sorted(
        field for field in all_fields if stage_perf._manager_field(field)
    )
    logical_render_fields = sorted(
        field for field in all_fields if _logical_render_field(field)
    )
    context_fields = sorted(
        field for field in LOGICAL_CONTEXT_FIELDS if field in all_fields
    )
    if not manager_fields:
        raise stage_perf.ComparisonError("no gameplay manager count fields found")
    if not logical_render_fields:
        raise stage_perf.ComparisonError("no logical render workload fields found")

    workload_differences = (
        _field_differences(
            manager_fields, "manager", baseline_keys, baseline, candidate
        )
        + _field_differences(
            context_fields, "context", baseline_keys, baseline, candidate
        )
        + _field_differences(
            logical_render_fields,
            "logical_render",
            baseline_keys,
            baseline,
            candidate,
        )
    )
    logical_workload_match = all(item["match"] for item in workload_differences)

    physical_differences = _field_differences(
        sorted(PHYSICAL_RENDER_FIELDS),
        "physical_render",
        baseline_keys,
        baseline,
        candidate,
    )
    diagnostic_differences = _field_differences(
        sorted(BATCH_DIAGNOSTIC_FIELDS),
        "batch_diagnostic",
        baseline_keys,
        baseline,
        candidate,
    )

    rules = RuleBook()
    total_batch_calls = 0
    total_batch_sprites = 0
    for key, baseline_record, candidate_record, vertex_accounting in zip(
        baseline_keys, baseline, candidate, vertex_accounting_results
    ):
        baseline_perf_valid = baseline_record["render_perf_valid"]
        candidate_perf_valid = candidate_record["render_perf_valid"]
        rules.check(
            "render_telemetry_valid",
            "both runs must report complete render telemetry",
            baseline_perf_valid == 1 and candidate_perf_valid == 1,
            stage=key[0],
            stage_frame=key[1],
            observed={
                "baseline": baseline_perf_valid,
                "candidate": candidate_perf_valid,
            },
            expected={"baseline": 1, "candidate": 1},
        )
        _check_zero_baseline_diagnostics(rules, key, baseline_record)
        _check_candidate_diagnostics(
            rules,
            key,
            candidate_record,
            candidate_record["render_frames"],
        )
        _check_physical_render_delta(
            rules,
            key,
            baseline_record,
            candidate_record,
            vertex_accounting,
        )
        total_batch_calls += candidate_record[
            "render_ascii_popup_batch_calls_total"
        ]
        total_batch_sprites += candidate_record[
            "render_ascii_popup_batch_sprites_total"
        ]

    rules.check(
        "batch_exercised",
        "a performance verdict requires at least one successful visible score batch",
        total_batch_calls > 0 and total_batch_sprites > 0,
        observed={"calls": total_batch_calls, "sprites": total_batch_sprites},
        expected="calls > 0 and sprites > 0",
    )
    reconciliations = rules.finish()
    physical_reconciliation_match = all(rule["passed"] for rule in reconciliations)
    comparison_valid = logical_workload_match and physical_reconciliation_match

    return {
        "schema": SCHEMA,
        "baseline_path": str(baseline_path),
        "candidate_path": str(candidate_path),
        "aligned": True,
        "expected_render_frames_per_sample": stage_perf.EXPECTED_RENDER_FRAMES,
        "sample_count": len(baseline_keys),
        "stage_filter": stage,
        "comparison_valid": comparison_valid,
        "logical_workload_match": logical_workload_match,
        "physical_reconciliation_match": physical_reconciliation_match,
        "general_strict_workload_match": general["strict_workload_match"],
        "general_strict_note": (
            "the unchanged general comparator is expected to reject the "
            "experiment when its physical or diagnostic counters differ"
        ),
        "workload_fields": {
            "manager": manager_fields,
            "context": context_fields,
            "logical_render": logical_render_fields,
            "physical_render": sorted(PHYSICAL_RENDER_FIELDS),
            "batch_diagnostic": sorted(BATCH_DIAGNOSTIC_FIELDS),
            "manager_diagnostic_excluded": sorted(
                set(all_fields) & stage_perf.MANAGER_DIAGNOSTIC_FIELDS
            ),
        },
        "workload_differences": workload_differences,
        "physical_differences": physical_differences,
        "diagnostic_differences": diagnostic_differences,
        "reconciliations": reconciliations,
        "timing": general["timing"],
        "pairs": general["pairs"],
    }


def _render_difference(prefix: str, difference: dict[str, Any]) -> str:
    line = (
        f"{prefix} category={difference['category']} field={difference['field']} "
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
    return line


def render_text(result: dict[str, Any]) -> str:
    timing = result["timing"]
    lines = [
        f"schema={result['schema']}",
        f"baseline={result['baseline_path']}",
        f"candidate={result['candidate_path']}",
        f"aligned={str(result['aligned']).lower()} samples={result['sample_count']} "
        f"render_frames_each={result['expected_render_frames_per_sample']}",
        f"stage_filter={result['stage_filter']!r}",
        f"comparison_valid={str(result['comparison_valid']).lower()}",
        f"logical_workload_match={str(result['logical_workload_match']).lower()}",
        "physical_reconciliation_match="
        f"{str(result['physical_reconciliation_match']).lower()}",
        "general_strict_workload_match="
        f"{str(result['general_strict_workload_match']).lower()} "
        f"note={result['general_strict_note']}",
    ]
    for difference in result["workload_differences"]:
        lines.append(_render_difference("WORKLOAD", difference))
        for mismatch in difference["mismatches"]:
            lines.append(
                "  MISMATCH "
                f"stage={mismatch['stage']} stage_frame={mismatch['stage_frame']} "
                f"baseline={mismatch['baseline']!r} candidate={mismatch['candidate']!r}"
            )
    for difference in result["physical_differences"]:
        lines.append(_render_difference("PHYSICAL", difference))
    for difference in result["diagnostic_differences"]:
        lines.append(_render_difference("DIAGNOSTIC", difference))
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


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Compare paired TH08 PSP ASCII score-popup batch samples"
    )
    parser.add_argument("baseline_log", type=Path)
    parser.add_argument("candidate_log", type=Path)
    parser.add_argument(
        "--bootstrap-iterations",
        type=int,
        default=stage_perf.DEFAULT_BOOTSTRAP_ITERATIONS,
    )
    parser.add_argument(
        "--seed",
        type=stage_perf.integer_argument,
        default=stage_perf.DEFAULT_BOOTSTRAP_SEED,
    )
    parser.add_argument("--tie-epsilon-ms", type=float, default=0.0)
    parser.add_argument(
        "--stage",
        type=int,
        help="compare one fixed 20-window stage and ignore later demo stages",
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
            stage=args.stage,
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
    return 0 if result["comparison_valid"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
