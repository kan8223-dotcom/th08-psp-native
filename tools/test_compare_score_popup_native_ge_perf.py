#!/usr/bin/env python3
"""Focused host tests for compare_score_popup_native_ge_perf.py."""

from __future__ import annotations

import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools"
TOOL_PATH = TOOLS / "compare_score_popup_native_ge_perf.py"
sys.path.insert(0, str(TOOLS))
SPEC = importlib.util.spec_from_file_location(
    "compare_score_popup_native_ge_perf", TOOL_PATH
)
assert SPEC is not None and SPEC.loader is not None
TOOL = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = TOOL
SPEC.loader.exec_module(TOOL)


def sample(
    stage_frame: int,
    fps: float,
    *,
    frame_offset: int,
    calls: int,
    state_emitted_total: int,
    state_emitted_peak: int,
    vertices: int = 12_000,
    draws: int = 800,
    state_requested: int = 500,
    upload_attempts: int = 700,
    replay: int = 1,
    demo: int = 1,
) -> str:
    calls_peak = 1 if calls else 0
    digits = calls * 3
    saved_vertices = digits * 4
    return (
        "SAMPLE phase=stage_relative_periodic "
        f"frame={stage_frame + frame_offset} stage=5 stage_frame={stage_frame} "
        f"replay={replay} demo={demo} fps={fps:.3f} "
        "enemy_reported=4/480 enemy_reported_peak=7 enemy_exact=4 "
        "enemy_exact_sampled_peak=7 enemy_hi=8 "
        "bullet_reported=20/1536 bullet_reported_peak=30 bullet_exact=20 "
        "bullet_exact_sampled_peak=30 bullet_hi=40 "
        "bullet_enum_frames=300 bullet_enum_slot_probes=1000 "
        "bullet_enum_word_probes=2000 bullet_enum_visited=500 "
        "bullet_enum_fallback_frames=0 "
        "laser_exact=1/256 laser_exact_sampled_peak=2 laser_hi=3 "
        "item_reported=10/2096 item_reported_peak=14 item_array_exact=10 "
        "item_list_exact=10 item_exact_sampled_peak=14 item_hi=20 "
        "render_arena_live=999 render_perf_valid=1 render_frames=300 "
        f"render_draws_total={draws} render_draws_peak=9 "
        f"render_vertices_total={vertices} render_vertices_peak=180 "
        f"render_state_requested_total={state_requested} "
        "render_state_requested_peak=7 "
        f"render_state_emitted_total={state_emitted_total} "
        f"render_state_emitted_peak={state_emitted_peak} "
        "render_matrix_recompute_total=20 render_matrix_recompute_peak=2 "
        "render_vfpu_sincos_total=30 render_vfpu_sincos_peak=1 "
        "render_effect_active_total=90 render_effect_active_peak=4 "
        "render_effect_drawn_total=80 render_effect_drawn_peak=4 "
        "render_item_drawn_total=60 render_item_drawn_peak=5 "
        "render_popup_active_total=12 render_popup_active_peak=2 "
        "render_popup_digits_total=36 render_popup_digits_peak=6 "
        f"render_ascii_popup_batch_calls_total={calls} "
        f"render_ascii_popup_batch_calls_peak={calls_peak} "
        f"render_ascii_popup_batch_digits_total={digits} "
        f"render_ascii_popup_batch_digits_peak={3 if calls else 0} "
        f"render_ascii_popup_batch_sprites_total={digits} "
        f"render_ascii_popup_batch_sprites_peak={3 if calls else 0} "
        f"render_ascii_popup_batch_vertices_saved_total={saved_vertices} "
        f"render_ascii_popup_batch_vertices_saved_peak={12 if calls else 0} "
        f"render_ascii_popup_batch_bytes_saved_total={saved_vertices * 28} "
        f"render_ascii_popup_batch_bytes_saved_peak={336 if calls else 0} "
        "render_ascii_popup_batch_fallbacks_total=0 "
        "render_ascii_popup_batch_fallbacks_peak=0 "
        f"render_upload_attempt_total={upload_attempts} "
        "render_upload_attempt_peak=9 "
        "render_actual_upload_total=3 render_actual_upload_peak=1 "
        "render_upload_bytes_total=4096 render_upload_bytes_peak=4096 "
        "render_text_bytes_total=0 render_text_bytes_peak=0\n"
    )


def native_record(
    *,
    kind: str,
    phase: str,
    stage_frame: int,
    attempts: int,
    submits: int,
    fallbacks: int,
    frame_offset: int = 2000,
    stage: int = 5,
    valid: int = 1,
) -> str:
    return (
        "SCORE_POPUP_NATIVE_GE_TELEMETRY "
        f"kind={kind} phase={phase} frame={stage_frame + frame_offset} "
        f"stage={stage} stage_frame={stage_frame} valid={valid} "
        "counter_scope=device_lifetime cumulative=1 counter_bits=32_wrap "
        f"attempts={attempts} submits={submits} client_fallbacks={fallbacks}\n"
    )


def matched_logs(
    *,
    baseline_fps: float = 40.0,
    candidate_fps: float = 50.0,
) -> tuple[str, str]:
    baseline_parts: list[str] = []
    candidate_parts = [
        native_record(
            kind="MARK",
            phase="stage_setup_complete",
            stage_frame=0,
            attempts=0,
            submits=0,
            fallbacks=0,
        )
    ]
    cumulative = 0
    for index, (_, stage_frame) in enumerate(TOOL.EXPECTED_KEYS):
        calls = 0 if index == 0 else 3
        baseline_parts.append(
            sample(
                stage_frame,
                baseline_fps,
                frame_offset=1000,
                calls=calls,
                state_emitted_total=5000,
                state_emitted_peak=80,
            )
        )
        cumulative += calls
        candidate_parts.append(
            native_record(
                kind="SAMPLE",
                phase="stage_relative_periodic",
                stage_frame=stage_frame,
                attempts=cumulative,
                submits=cumulative,
                fallbacks=0,
            )
        )
        candidate_parts.append(
            sample(
                stage_frame,
                candidate_fps,
                frame_offset=2000,
                calls=calls,
                state_emitted_total=5000 - calls * 9,
                state_emitted_peak=80 if calls == 0 else 71,
            )
        )
    candidate_parts.append(
        native_record(
            kind="MARK",
            phase="stage_teardown_complete",
            stage_frame=TOOL.EXPECTED_TEARDOWN_STAGE_FRAME,
            attempts=cumulative,
            submits=cumulative,
            fallbacks=0,
        )
    )
    return "".join(baseline_parts), "".join(candidate_parts)


class TemporaryLogs(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.directory = Path(self.temporary.name)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def write(self, name: str, text: str) -> Path:
        path = self.directory / name
        path.write_text(text, encoding="utf-8")
        return path

    def compare(
        self,
        baseline: str,
        candidate: str,
        *,
        iterations: int = 50,
    ):
        return TOOL.compare_logs(
            self.write("baseline.log", baseline),
            self.write("candidate.log", candidate),
            bootstrap_iterations=iterations,
            bootstrap_seed=123,
        )


class ValidComparisonTests(TemporaryLogs):
    def test_exact_native_delta_reconciles_all_twenty_windows(self) -> None:
        baseline, candidate = matched_logs()
        result = self.compare(baseline, candidate)
        self.assertTrue(result["comparison_valid"])
        self.assertTrue(result["exact_workload_match"])
        self.assertTrue(result["native_reconciliation_match"])
        self.assertEqual(result["sample_count"], 20)
        self.assertEqual(result["native_total_submits_in_samples"], 57)
        self.assertTrue(result["performance_gate_passed"])
        self.assertFalse(result["bootstrap_iterations_valid"])
        self.assertFalse(result["acceptance_passed"])

    def test_one_million_iteration_contract_controls_acceptance(self) -> None:
        baseline, candidate = matched_logs()
        with mock.patch.object(
            TOOL.stage_perf,
            "paired_bootstrap_ci",
            return_value=(4.9, 5.1),
        ):
            result = self.compare(
                baseline,
                candidate,
                iterations=TOOL.REQUIRED_BOOTSTRAP_ITERATIONS,
            )
        self.assertTrue(result["bootstrap_iterations_valid"])
        self.assertTrue(result["performance_gate_passed"])
        self.assertTrue(result["acceptance_passed"])

    def test_native_interval_and_nine_call_algebra_are_reported(self) -> None:
        baseline, candidate = matched_logs()
        result = self.compare(baseline, candidate)
        first_busy = result["native_intervals"][1]
        self.assertEqual(first_busy["frontend_calls"], 3)
        self.assertEqual(first_busy["native_submits"], 3)
        self.assertEqual(first_busy["emitted_state_saved"], 27)
        self.assertEqual(first_busy["expected_emitted_state_saved"], 27)


class RejectionTests(TemporaryLogs):
    def test_requires_exact_twenty_window_stage5_route(self) -> None:
        baseline, candidate = matched_logs()
        baseline = "\n".join(baseline.splitlines()[:-1]) + "\n"
        with self.assertRaises(TOOL.stage_perf.ComparisonError):
            self.compare(baseline, candidate)

    def test_any_topology_delta_is_rejected(self) -> None:
        baseline, candidate = matched_logs()
        candidate = candidate.replace(
            "render_vertices_total=12000", "render_vertices_total=12001", 1
        )
        result = self.compare(baseline, candidate)
        self.assertFalse(result["comparison_valid"])
        difference = next(
            item
            for item in result["workload_differences"]
            if item["field"] == "render_vertices_total"
        )
        self.assertFalse(difference["match"])

    def test_draw_request_and_upload_deltas_are_all_strict(self) -> None:
        replacements = (
            ("render_draws_total=800", "render_draws_total=801"),
            ("render_state_requested_total=500", "render_state_requested_total=501"),
            ("render_upload_attempt_total=700", "render_upload_attempt_total=701"),
        )
        for old, new in replacements:
            with self.subTest(field=old):
                baseline, candidate = matched_logs()
                result = self.compare(baseline, candidate.replace(old, new, 1))
                self.assertFalse(result["comparison_valid"])
                self.assertFalse(result["exact_workload_match"])

    def test_wrong_nine_call_total_is_rejected(self) -> None:
        baseline, candidate = matched_logs()
        candidate = candidate.replace(
            "render_state_emitted_total=4973",
            "render_state_emitted_total=4974",
            1,
        )
        result = self.compare(baseline, candidate)
        rule = next(
            item
            for item in result["reconciliations"]
            if item["rule"] == "emitted_state_total_exact"
        )
        self.assertFalse(rule["passed"])
        self.assertFalse(result["comparison_valid"])

    def test_peak_must_be_zero_delta_when_interval_has_no_submit(self) -> None:
        baseline, candidate = matched_logs()
        candidate = candidate.replace(
            "render_state_emitted_peak=80",
            "render_state_emitted_peak=79",
            1,
        )
        result = self.compare(baseline, candidate)
        rule = next(
            item
            for item in result["reconciliations"]
            if item["rule"] == "emitted_state_peak_bound"
        )
        self.assertFalse(rule["passed"])

    def test_native_cumulative_delta_must_equal_frontend_calls(self) -> None:
        baseline, candidate = matched_logs()
        candidate = candidate.replace(
            "stage_frame=601 valid=1 counter_scope=device_lifetime "
            "cumulative=1 counter_bits=32_wrap attempts=3 submits=3",
            "stage_frame=601 valid=1 counter_scope=device_lifetime "
            "cumulative=1 counter_bits=32_wrap attempts=4 submits=4",
            1,
        )
        result = self.compare(baseline, candidate)
        rule = next(
            item
            for item in result["reconciliations"]
            if item["rule"] == "native_interval_matches_frontend"
        )
        self.assertFalse(rule["passed"])

    def test_any_native_fallback_is_rejected(self) -> None:
        baseline, candidate = matched_logs()
        candidate = candidate.replace(
            "attempts=3 submits=3 client_fallbacks=0",
            "attempts=3 submits=2 client_fallbacks=1",
            1,
        )
        result = self.compare(baseline, candidate)
        self.assertFalse(result["native_reconciliation_match"])
        rule = next(
            item
            for item in result["reconciliations"]
            if item["rule"] == "native_interval_matches_frontend"
        )
        self.assertFalse(rule["passed"])

    def test_origin_is_required(self) -> None:
        baseline, candidate = matched_logs()
        candidate = candidate.replace(
            "stage=5 stage_frame=0 valid=1",
            "stage=4 stage_frame=0 valid=1",
            1,
        )
        with self.assertRaises(TOOL.stage_perf.ComparisonError):
            self.compare(baseline, candidate)

    def test_native_record_must_immediately_precede_general_sample(self) -> None:
        baseline, candidate = matched_logs()
        needle = "client_fallbacks=0\nSAMPLE phase=stage_relative_periodic"
        candidate = candidate.replace(
            needle,
            "client_fallbacks=0\nMARK phase=interposed\n"
            "SAMPLE phase=stage_relative_periodic",
            1,
        )
        result = self.compare(baseline, candidate)
        rule = next(
            item
            for item in result["reconciliations"]
            if item["rule"] == "native_record_precedes_snapshot"
        )
        self.assertFalse(rule["passed"])

    def test_native_off_control_must_not_contain_native_records(self) -> None:
        baseline, candidate = matched_logs()
        baseline = native_record(
            kind="MARK",
            phase="stage_setup_complete",
            stage_frame=0,
            attempts=0,
            submits=0,
            fallbacks=0,
        ) + baseline
        with self.assertRaises(TOOL.stage_perf.ComparisonError):
            self.compare(baseline, candidate)

    def test_replay_and_demo_context_are_strict(self) -> None:
        baseline, candidate = matched_logs()
        candidate = candidate.replace("replay=1 demo=1", "replay=1 demo=0", 1)
        result = self.compare(baseline, candidate)
        self.assertFalse(result["comparison_valid"])
        self.assertFalse(result["exact_workload_match"])

    def test_positive_speed_without_positive_ci_is_no_go(self) -> None:
        baseline, candidate = matched_logs()
        with mock.patch.object(
            TOOL.stage_perf,
            "paired_bootstrap_ci",
            return_value=(-0.01, 0.10),
        ):
            result = self.compare(
                baseline,
                candidate,
                iterations=TOOL.REQUIRED_BOOTSTRAP_ITERATIONS,
            )
        self.assertTrue(result["comparison_valid"])
        self.assertFalse(result["performance_gate_passed"])
        self.assertFalse(result["acceptance_passed"])


class CommandLineTests(TemporaryLogs):
    def test_json_is_emitted_and_nonaccepted_run_returns_one(self) -> None:
        baseline, candidate = matched_logs()
        baseline_path = self.write("baseline.log", baseline)
        candidate_path = self.write("candidate.log", candidate)
        completed = subprocess.run(
            [
                sys.executable,
                str(TOOL_PATH),
                str(baseline_path),
                str(candidate_path),
                "--bootstrap-iterations",
                "20",
                "--json",
            ],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        self.assertEqual(completed.returncode, 1, completed.stderr)
        result = json.loads(completed.stdout)
        self.assertTrue(result["comparison_valid"])
        self.assertFalse(result["bootstrap_iterations_valid"])
        self.assertFalse(result["acceptance_passed"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
