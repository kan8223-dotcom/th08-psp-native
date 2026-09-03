#!/usr/bin/env python3
"""Host tests for compare_ascii_popup_batch_perf.py."""

from __future__ import annotations

import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools"
TOOL_PATH = TOOLS / "compare_ascii_popup_batch_perf.py"
sys.path.insert(0, str(TOOLS))
SPEC = importlib.util.spec_from_file_location(
    "compare_ascii_popup_batch_perf", TOOL_PATH
)
assert SPEC is not None and SPEC.loader is not None
TOOL = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = TOOL
SPEC.loader.exec_module(TOOL)


def sample(
    stage_frame: int,
    fps: float,
    *,
    stage: int = 5,
    batch: bool = False,
    render_frames: int = 300,
    enemy_exact: int = 4,
    vertices_adjust: int = 0,
    fps_overlay_vertices: int = 0,
    fps_overlay_peak: int = 0,
    raw_vertices_adjust: int = 0,
    draws_adjust: int = 0,
    state_requested_adjust: int = 0,
    state_emitted_adjust: int = 0,
    upload_attempt_adjust: int = 0,
    diagnostic_bytes_adjust: int = 0,
    effect_drawn_adjust: int = 0,
    actual_upload_adjust: int = 0,
) -> str:
    if batch:
        calls_total, calls_peak = 2, 1
        digits_total, digits_peak = 15, 8
        sprites_total, sprites_peak = 10, 5
        saved_total, saved_peak = 40, 20
        bytes_total, bytes_peak = 1120 + diagnostic_bytes_adjust, 560
        fallbacks_total, fallbacks_peak = 0, 0
        vertices = 11_960 + vertices_adjust
        vertices_peak = 160
        draws = 802 + draws_adjust
        draws_peak = 10
        state_requested = 506 + state_requested_adjust
        state_requested_peak = 10
        state_emitted = 4_020 + state_emitted_adjust
        state_emitted_peak = 55
        upload_attempts = 702 + upload_attempt_adjust
        upload_attempts_peak = 9
    else:
        calls_total = calls_peak = 0
        digits_total = digits_peak = 0
        sprites_total = sprites_peak = 0
        saved_total = saved_peak = 0
        bytes_total = bytes_peak = 0
        fallbacks_total = fallbacks_peak = 0
        vertices = 12_000 + vertices_adjust
        vertices_peak = 180
        draws = 800 + draws_adjust
        draws_peak = 9
        state_requested = 500 + state_requested_adjust
        state_requested_peak = 7
        state_emitted = 4_000 + state_emitted_adjust
        state_emitted_peak = 45
        upload_attempts = 700 + upload_attempt_adjust
        upload_attempts_peak = 8

    game_vertices = vertices
    game_vertices_peak = vertices_peak
    vertices = game_vertices + fps_overlay_vertices + raw_vertices_adjust
    vertices_peak = game_vertices_peak + fps_overlay_peak

    return (
        "SAMPLE phase=stage_relative_periodic "
        f"frame={stage_frame + 1000} stage={stage} stage_frame={stage_frame} "
        "is_replay=1 is_demo=0 "
        f"fps={fps:.3f} "
        "enemy_reported=4/480 enemy_reported_peak=7 enemy_exact="
        f"{enemy_exact} enemy_exact_sampled_peak=7 enemy_hi=8 "
        "bullet_reported=20/1536 bullet_reported_peak=30 bullet_exact=20 "
        "bullet_exact_sampled_peak=30 bullet_hi=40 "
        "laser_exact=1/256 laser_exact_sampled_peak=2 laser_hi=3 "
        "item_reported=10/2096 item_reported_peak=14 item_array_exact=10 "
        "item_list_exact=10 item_exact_sampled_peak=14 item_hi=20 "
        "render_arena_live=999 render_perf_valid=1 "
        f"render_frames={render_frames} "
        f"render_draws_total={draws} render_draws_peak={draws_peak} "
        f"render_vertices_total={vertices} render_vertices_peak={vertices_peak} "
        f"render_fps_overlay_vertices_total={fps_overlay_vertices} "
        f"render_fps_overlay_vertices_peak={fps_overlay_peak} "
        f"render_game_vertices_total={game_vertices} "
        f"render_game_vertices_peak={game_vertices_peak} "
        f"render_state_requested_total={state_requested} "
        f"render_state_requested_peak={state_requested_peak} "
        f"render_state_emitted_total={state_emitted} "
        f"render_state_emitted_peak={state_emitted_peak} "
        "render_matrix_recompute_total=20 render_matrix_recompute_peak=2 "
        "render_effect_active_total=90 render_effect_active_peak=4 "
        f"render_effect_drawn_total={80 + effect_drawn_adjust} "
        "render_effect_drawn_peak=4 "
        "render_item_drawn_total=60 render_item_drawn_peak=5 "
        "render_popup_active_total=12 render_popup_active_peak=4 "
        "render_popup_digits_total=30 render_popup_digits_peak=10 "
        f"render_ascii_popup_batch_calls_total={calls_total} "
        f"render_ascii_popup_batch_calls_peak={calls_peak} "
        f"render_ascii_popup_batch_digits_total={digits_total} "
        f"render_ascii_popup_batch_digits_peak={digits_peak} "
        f"render_ascii_popup_batch_sprites_total={sprites_total} "
        f"render_ascii_popup_batch_sprites_peak={sprites_peak} "
        f"render_ascii_popup_batch_vertices_saved_total={saved_total} "
        f"render_ascii_popup_batch_vertices_saved_peak={saved_peak} "
        f"render_ascii_popup_batch_bytes_saved_total={bytes_total} "
        f"render_ascii_popup_batch_bytes_saved_peak={bytes_peak} "
        f"render_ascii_popup_batch_fallbacks_total={fallbacks_total} "
        f"render_ascii_popup_batch_fallbacks_peak={fallbacks_peak} "
        f"render_upload_attempt_total={upload_attempts} "
        f"render_upload_attempt_peak={upload_attempts_peak} "
        f"render_actual_upload_total={3 + actual_upload_adjust} "
        "render_actual_upload_peak=1 render_upload_bytes_total=4096 "
        "render_upload_bytes_peak=4096\n"
    )


def fixed_stage_log(stage: int, fps: float, *, batch: bool) -> str:
    return "".join(
        sample(stage_frame, fps, stage=stage, batch=batch)
        for stage_frame in TOOL.stage_perf.FIXED_STAGE_SAMPLE_FRAMES
    )


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
        baseline_text: str,
        candidate_text: str,
        *,
        stage: int | None = None,
    ) -> dict:
        return TOOL.compare_logs(
            self.write("baseline.log", baseline_text),
            self.write("candidate.log", candidate_text),
            bootstrap_iterations=100,
            bootstrap_seed=123,
            stage=stage,
        )


class ReconciliationTests(TemporaryLogs):
    def test_stage_filter_keeps_popup_timing_to_twenty_stage5_windows(self) -> None:
        result = self.compare(
            fixed_stage_log(5, 40.0, batch=False)
            + fixed_stage_log(3, 10.0, batch=False),
            fixed_stage_log(5, 50.0, batch=True)
            + fixed_stage_log(3, 100.0, batch=True),
            stage=5,
        )
        self.assertTrue(result["comparison_valid"])
        self.assertEqual(result["stage_filter"], 5)
        self.assertEqual(result["sample_count"], 20)
        self.assertEqual({pair["stage"] for pair in result["pairs"]}, {5})
        self.assertAlmostEqual(result["timing"]["paired_improvement_ms_mean"], 5.0)

    def test_valid_batch_reconciles_exact_savings_and_paired_timing(self) -> None:
        result = self.compare(sample(301, 40.0), sample(301, 50.0, batch=True))
        self.assertTrue(result["comparison_valid"])
        self.assertTrue(result["logical_workload_match"])
        self.assertTrue(result["physical_reconciliation_match"])
        # The unchanged generic comparator remains intentionally strict.
        self.assertFalse(result["general_strict_workload_match"])
        self.assertAlmostEqual(result["pairs"][0]["improvement_ms"], 5.0)
        rules = {item["rule"]: item for item in result["reconciliations"]}
        self.assertTrue(rules["submitted_vertices_exact"]["passed"])
        self.assertTrue(rules["candidate_saved_byte_accounting"]["passed"])
        self.assertTrue(rules["state_emitted_total_exact"]["passed"])
        rendered = TOOL.render_text(result)
        self.assertIn("comparison_valid=true", rendered)
        self.assertIn("RECONCILE rule=submitted_vertices_exact passed=true", rendered)

    def test_fps_overlay_only_drift_does_not_hide_popup_game_savings(self) -> None:
        result = self.compare(
            sample(301, 50.0),
            sample(
                301,
                51.0,
                batch=True,
                fps_overlay_vertices=72,
                fps_overlay_peak=6,
            ),
        )
        self.assertTrue(result["comparison_valid"])
        rules = {item["rule"]: item for item in result["reconciliations"]}
        self.assertTrue(rules["fps_overlay_vertex_accounting"]["passed"])
        self.assertTrue(rules["submitted_vertices_exact"]["passed"])
        self.assertTrue(rules["raw_vertex_delta_explained"]["passed"])
        physical = {
            item["field"]: item for item in result["physical_differences"]
        }
        self.assertFalse(physical["render_vertices_total"]["match"])
        self.assertFalse(
            physical["render_fps_overlay_vertices_total"]["match"]
        )

    def test_broken_raw_game_overlay_identity_is_rejected(self) -> None:
        result = self.compare(
            sample(301, 50.0),
            sample(
                301,
                51.0,
                batch=True,
                fps_overlay_vertices=72,
                fps_overlay_peak=6,
                raw_vertices_adjust=1,
            ),
        )
        self.assertFalse(result["comparison_valid"])
        rules = {item["rule"]: item for item in result["reconciliations"]}
        self.assertFalse(rules["fps_overlay_vertex_accounting"]["passed"])
        self.assertFalse(rules["raw_vertex_delta_explained"]["passed"])

    def test_multiple_aligned_windows_use_fixed_seed_bootstrap(self) -> None:
        baseline = sample(301, 40.0) + sample(601, 50.0)
        candidate = sample(301, 50.0, batch=True) + sample(601, 55.0, batch=True)
        first = self.compare(baseline, candidate)
        second = self.compare(baseline, candidate)
        self.assertEqual(
            first["timing"]["bootstrap_95_low_ms"],
            second["timing"]["bootstrap_95_low_ms"],
        )
        self.assertEqual(first["sample_count"], 2)

    def test_manager_difference_is_never_normalized(self) -> None:
        result = self.compare(
            sample(301, 50.0),
            sample(301, 51.0, batch=True, enemy_exact=5),
        )
        self.assertFalse(result["comparison_valid"])
        self.assertFalse(result["logical_workload_match"])
        difference = next(
            item
            for item in result["workload_differences"]
            if item["field"] == "enemy_exact"
        )
        self.assertEqual(difference["candidate_minus_baseline"], 1)

    def test_unrelated_render_difference_is_rejected(self) -> None:
        result = self.compare(
            sample(301, 50.0),
            sample(301, 51.0, batch=True, effect_drawn_adjust=1),
        )
        self.assertFalse(result["comparison_valid"])
        difference = next(
            item
            for item in result["workload_differences"]
            if item["field"] == "render_effect_drawn_total"
        )
        self.assertFalse(difference["match"])

    def test_actual_texture_upload_is_logical_and_must_not_change(self) -> None:
        result = self.compare(
            sample(301, 50.0),
            sample(301, 51.0, batch=True, actual_upload_adjust=1),
        )
        self.assertFalse(result["comparison_valid"])
        fields = {
            item["field"]: item for item in result["workload_differences"]
        }
        self.assertFalse(fields["render_actual_upload_total"]["match"])

    def test_wrong_saved_vertex_delta_is_rejected(self) -> None:
        result = self.compare(
            sample(301, 50.0),
            sample(301, 51.0, batch=True, vertices_adjust=1),
        )
        self.assertFalse(result["comparison_valid"])
        rule = next(
            item
            for item in result["reconciliations"]
            if item["rule"] == "submitted_vertices_exact"
        )
        self.assertFalse(rule["passed"])
        self.assertEqual(rule["violations"][0]["observed"], 39)
        self.assertEqual(rule["violations"][0]["expected"], 40)

    def test_wrong_saved_byte_counter_is_rejected(self) -> None:
        result = self.compare(
            sample(301, 50.0),
            sample(301, 51.0, batch=True, diagnostic_bytes_adjust=1),
        )
        self.assertFalse(result["comparison_valid"])
        rule = next(
            item
            for item in result["reconciliations"]
            if item["rule"] == "candidate_saved_byte_accounting"
        )
        self.assertFalse(rule["passed"])

    def test_draw_state_and_upload_attempt_algebra_are_exact(self) -> None:
        for adjustment, rule_name in (
            ({"draws_adjust": 1}, "state_requested_total_exact"),
            ({"state_requested_adjust": 1}, "state_requested_total_exact"),
            ({"state_emitted_adjust": 1}, "state_emitted_total_exact"),
            ({"upload_attempt_adjust": 1}, "clean_upload_attempt_total_exact"),
        ):
            with self.subTest(adjustment=adjustment):
                result = self.compare(
                    sample(301, 50.0),
                    sample(301, 51.0, batch=True, **adjustment),
                )
                self.assertFalse(result["comparison_valid"])
                rules = {item["rule"]: item for item in result["reconciliations"]}
                self.assertFalse(rules[rule_name]["passed"])

    def test_nonzero_control_diagnostic_is_rejected(self) -> None:
        # Reuse a batch-shaped record as the control to prove the OFF gate is
        # checked independently of the candidate reconciliation.
        result = self.compare(
            sample(301, 50.0, batch=True),
            sample(301, 51.0, batch=True),
        )
        self.assertFalse(result["comparison_valid"])
        rule = next(
            item
            for item in result["reconciliations"]
            if item["rule"] == "baseline_batch_diagnostics_zero"
        )
        self.assertFalse(rule["passed"])

    def test_unexercised_batch_cannot_receive_performance_verdict(self) -> None:
        result = self.compare(sample(301, 50.0), sample(301, 51.0))
        self.assertFalse(result["comparison_valid"])
        rules = {item["rule"]: item for item in result["reconciliations"]}
        self.assertFalse(rules["batch_exercised"]["passed"])


class InputRejectionTests(TemporaryLogs):
    def test_stage_relative_alignment_is_exact(self) -> None:
        with self.assertRaises(TOOL.stage_perf.ComparisonError):
            self.compare(sample(301, 50.0), sample(302, 51.0, batch=True))

    def test_same_missing_window_is_rejected(self) -> None:
        sparse_control = sample(301, 50.0) + sample(901, 50.0)
        sparse_candidate = sample(301, 51.0, batch=True) + sample(
            901, 51.0, batch=True
        )
        with self.assertRaises(TOOL.stage_perf.ComparisonError):
            self.compare(sparse_control, sparse_candidate)

    def test_required_diagnostic_field_missing_is_rejected(self) -> None:
        candidate = sample(301, 51.0, batch=True).replace(
            "render_ascii_popup_batch_bytes_saved_peak=560 ", ""
        )
        with self.assertRaises(TOOL.stage_perf.ComparisonError):
            self.compare(sample(301, 50.0), candidate)


class CommandLineTests(TemporaryLogs):
    def run_tool(
        self, baseline: Path, candidate: Path, *arguments: str
    ) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                sys.executable,
                str(TOOL_PATH),
                str(baseline),
                str(candidate),
                "--bootstrap-iterations",
                "20",
                *arguments,
            ],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )

    def test_valid_json_is_machine_readable_and_returns_zero(self) -> None:
        baseline = self.write("baseline.log", sample(301, 50.0))
        candidate = self.write("candidate.log", sample(301, 51.0, batch=True))
        completed = self.run_tool(baseline, candidate, "--json")
        self.assertEqual(completed.returncode, 0, completed.stderr)
        result = json.loads(completed.stdout)
        self.assertEqual(result["schema"], TOOL.SCHEMA)
        self.assertTrue(result["comparison_valid"])

    def test_reconciliation_failure_returns_one_with_report(self) -> None:
        baseline = self.write("baseline.log", sample(301, 50.0))
        candidate = self.write(
            "candidate.log", sample(301, 51.0, batch=True, vertices_adjust=1)
        )
        completed = self.run_tool(baseline, candidate)
        self.assertEqual(completed.returncode, 1, completed.stderr)
        self.assertIn("comparison_valid=false", completed.stdout)
        self.assertIn("RECONCILE rule=submitted_vertices_exact passed=false", completed.stdout)

    def test_alignment_failure_returns_two(self) -> None:
        baseline = self.write("baseline.log", sample(301, 50.0))
        candidate = self.write("candidate.log", sample(302, 51.0, batch=True))
        completed = self.run_tool(baseline, candidate)
        self.assertEqual(completed.returncode, 2)
        self.assertIn("alignment failure", completed.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
