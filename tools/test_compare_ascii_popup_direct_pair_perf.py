#!/usr/bin/env python3
"""Tests for the direct-pair-specific stage-relative A/B comparator."""

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
TOOL_PATH = TOOLS / "compare_ascii_popup_direct_pair_perf.py"
sys.path.insert(0, str(TOOLS))
SPEC = importlib.util.spec_from_file_location(
    "compare_ascii_popup_direct_pair_perf", TOOL_PATH
)
assert SPEC is not None and SPEC.loader is not None
TOOL = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = TOOL
SPEC.loader.exec_module(TOOL)


def sample(
    stage_frame: int,
    fps: float,
    *,
    direct: bool,
    direct_quads_adjust: int = 0,
    direct_culls_adjust: int = 0,
    direct_nearby_adjust: int = 0,
    baseline_direct_noise: int = 0,
    effect_drawn_adjust: int = 0,
    batch_digits_adjust: int = 0,
    unrelated_render_adjust: int = 0,
    fps_overlay_vertices: int = 0,
) -> str:
    digits_total = 15 + batch_digits_adjust
    digits_peak = 8
    if direct:
        active_total, active_peak = 4, 3
        quads_total = 15 + direct_quads_adjust
        quads_peak = 8
        culls_total = 15 + direct_culls_adjust
        culls_peak = 8
        nearby_total = 82 + direct_nearby_adjust
        nearby_peak = 44
    else:
        active_total = active_peak = baseline_direct_noise
        quads_total = quads_peak = baseline_direct_noise
        culls_total = culls_peak = baseline_direct_noise
        nearby_total = nearby_peak = baseline_direct_noise

    game_vertices = 12_000
    raw_vertices = game_vertices + fps_overlay_vertices
    fps_overlay_peak = 6 if fps_overlay_vertices else 0
    raw_vertices_peak = 180 + fps_overlay_peak
    return (
        "SAMPLE phase=stage_relative_periodic "
        f"frame={stage_frame + 1000} stage=5 stage_frame={stage_frame} "
        "is_replay=1 is_demo=0 "
        f"fps={fps:.3f} "
        "enemy_reported=4/480 enemy_reported_peak=7 enemy_exact=4 "
        "enemy_exact_sampled_peak=7 enemy_hi=8 "
        "bullet_reported=20/1536 bullet_reported_peak=30 bullet_exact=20 "
        "bullet_exact_sampled_peak=30 bullet_hi=40 "
        "laser_exact=1/256 laser_exact_sampled_peak=2 laser_hi=3 "
        "item_reported=10/2096 item_reported_peak=14 item_array_exact=10 "
        "item_list_exact=10 item_exact_sampled_peak=14 item_hi=20 "
        "render_perf_valid=1 render_frames=300 "
        "render_draws_total=802 render_draws_peak=10 "
        f"render_vertices_total={raw_vertices} "
        f"render_vertices_peak={raw_vertices_peak} "
        f"render_fps_overlay_vertices_total={fps_overlay_vertices} "
        f"render_fps_overlay_vertices_peak={fps_overlay_peak} "
        f"render_game_vertices_total={game_vertices} "
        "render_game_vertices_peak=180 "
        "render_state_requested_total=506 render_state_requested_peak=10 "
        "render_state_emitted_total=4020 render_state_emitted_peak=55 "
        "render_matrix_recompute_total=20 render_matrix_recompute_peak=2 "
        "render_effect_active_total=90 render_effect_active_peak=4 "
        f"render_effect_drawn_total={80 + effect_drawn_adjust} "
        "render_effect_drawn_peak=4 "
        "render_item_drawn_total=60 render_item_drawn_peak=5 "
        "render_popup_active_total=12 render_popup_active_peak=4 "
        "render_popup_digits_total=30 render_popup_digits_peak=10 "
        "render_ascii_popup_batch_calls_total=2 "
        "render_ascii_popup_batch_calls_peak=1 "
        f"render_ascii_popup_batch_digits_total={digits_total} "
        f"render_ascii_popup_batch_digits_peak={digits_peak} "
        "render_ascii_popup_batch_sprites_total=10 "
        "render_ascii_popup_batch_sprites_peak=5 "
        "render_ascii_popup_batch_vertices_saved_total=40 "
        "render_ascii_popup_batch_vertices_saved_peak=20 "
        "render_ascii_popup_batch_bytes_saved_total=1120 "
        "render_ascii_popup_batch_bytes_saved_peak=560 "
        "render_ascii_popup_batch_fallbacks_total=0 "
        "render_ascii_popup_batch_fallbacks_peak=0 "
        f"render_ascii_popup_direct_active_popups_total={active_total} "
        f"render_ascii_popup_direct_active_popups_peak={active_peak} "
        f"render_ascii_popup_direct_validation_quads_avoided_total={quads_total} "
        f"render_ascii_popup_direct_validation_quads_avoided_peak={quads_peak} "
        f"render_ascii_popup_direct_validation_culls_avoided_total={culls_total} "
        f"render_ascii_popup_direct_validation_culls_avoided_peak={culls_peak} "
        f"render_ascii_popup_direct_nearbyint_avoided_total={nearby_total} "
        f"render_ascii_popup_direct_nearbyint_avoided_peak={nearby_peak} "
        f"render_unrelated_counter_total={100 + unrelated_render_adjust} "
        "render_unrelated_counter_peak=3 "
        "render_actual_upload_total=3 render_actual_upload_peak=1 "
        "render_upload_bytes_total=4096 render_upload_bytes_peak=4096\n"
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

    def compare(self, baseline: str, candidate: str) -> dict:
        return TOOL.compare_logs(
            self.write("baseline.log", baseline),
            self.write("candidate.log", candidate),
            bootstrap_iterations=50,
            bootstrap_seed=123,
        )


class DirectPairComparatorTests(TemporaryLogs):
    def valid(self, baseline_fps: float = 40.0, candidate_fps: float = 50.0):
        return self.compare(
            sample(301, baseline_fps, direct=False),
            sample(301, candidate_fps, direct=True),
        )

    def rule(self, result: dict, name: str) -> dict:
        return next(item for item in result["reconciliations"]
                    if item["rule"] == name)

    def test_valid_pair_keeps_all_output_exact_and_reconciles_mechanism(self):
        result = self.valid()
        self.assertTrue(result["comparison_valid"])
        self.assertTrue(result["exact_workload_match"])
        self.assertTrue(result["mechanism_reconciliation_match"])
        self.assertFalse(result["general_strict_workload_match"])
        self.assertTrue(self.rule(result, "validation_geometry_exact")["passed"])
        self.assertTrue(self.rule(result, "validation_cull_exact")["passed"])
        self.assertTrue(self.rule(result, "nearbyint_savings_exact")["passed"])
        self.assertAlmostEqual(result["timing"]["paired_improvement_ms_mean"], 5.0)

    def test_each_direct_algebra_error_is_rejected(self):
        cases = (
            ({"direct_quads_adjust": 1}, "validation_geometry_exact"),
            ({"direct_culls_adjust": 1}, "validation_cull_exact"),
            ({"direct_nearby_adjust": 1}, "nearbyint_savings_exact"),
        )
        for adjustments, rule_name in cases:
            with self.subTest(rule=rule_name):
                result = self.compare(
                    sample(301, 40.0, direct=False),
                    sample(301, 50.0, direct=True, **adjustments),
                )
                self.assertFalse(result["comparison_valid"])
                self.assertFalse(self.rule(result, rule_name)["passed"])

    def test_nonzero_baseline_mechanism_is_rejected(self):
        result = self.compare(
            sample(301, 40.0, direct=False, baseline_direct_noise=1),
            sample(301, 50.0, direct=True),
        )
        self.assertFalse(result["comparison_valid"])
        self.assertFalse(self.rule(result, "baseline_direct_counters_zero")["passed"])

    def test_existing_batch_authority_must_match_exactly(self):
        result = self.compare(
            sample(301, 40.0, direct=False),
            sample(301, 50.0, direct=True, batch_digits_adjust=1),
        )
        self.assertFalse(result["comparison_valid"])
        mismatch = next(
            item for item in result["workload_differences"]
            if item["field"] == "render_ascii_popup_batch_digits_total"
        )
        self.assertFalse(mismatch["match"])

    def test_unrelated_render_counter_is_not_blanket_excluded(self):
        result = self.compare(
            sample(301, 40.0, direct=False),
            sample(301, 50.0, direct=True, unrelated_render_adjust=1),
        )
        self.assertFalse(result["comparison_valid"])
        mismatch = next(
            item for item in result["workload_differences"]
            if item["field"] == "render_unrelated_counter_total"
        )
        self.assertFalse(mismatch["match"])

    def test_gameplay_or_logical_render_drift_is_rejected(self):
        result = self.compare(
            sample(301, 40.0, direct=False),
            sample(301, 50.0, direct=True, effect_drawn_adjust=1),
        )
        self.assertFalse(result["comparison_valid"])

    def test_fps_overlay_owned_raw_drift_is_explicitly_reconciled(self):
        result = self.compare(
            sample(301, 40.0, direct=False),
            sample(301, 50.0, direct=True, fps_overlay_vertices=72),
        )
        self.assertTrue(result["comparison_valid"])
        self.assertTrue(result["fps_overlay_vertex_reconciliation_match"])

    def test_missing_direct_field_is_rejected(self):
        candidate = sample(301, 50.0, direct=True).replace(
            "render_ascii_popup_direct_nearbyint_avoided_peak=44 ", ""
        )
        with self.assertRaises(TOOL.stage_perf.ComparisonError):
            self.compare(sample(301, 40.0, direct=False), candidate)

    def test_cli_json_and_failure_exit_codes(self):
        baseline = self.write("baseline.log", sample(301, 40.0, direct=False))
        candidate = self.write("candidate.log", sample(301, 50.0, direct=True))
        completed = subprocess.run(
            [sys.executable, str(TOOL_PATH), str(baseline), str(candidate),
             "--bootstrap-iterations", "20", "--json"],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            check=False,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertTrue(json.loads(completed.stdout)["comparison_valid"])

        broken = self.write(
            "broken.log",
            sample(301, 50.0, direct=True, direct_nearby_adjust=1),
        )
        completed = subprocess.run(
            [sys.executable, str(TOOL_PATH), str(baseline), str(broken),
             "--bootstrap-iterations", "20"],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            check=False,
        )
        self.assertEqual(completed.returncode, 1, completed.stderr)
        self.assertIn("nearbyint_savings_exact passed=false", completed.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
