#!/usr/bin/env python3
"""Host tests for compare_stage_relative_perf.py."""

from __future__ import annotations

import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TOOL_PATH = ROOT / "tools" / "compare_stage_relative_perf.py"
SPEC = importlib.util.spec_from_file_location("compare_stage_relative_perf", TOOL_PATH)
assert SPEC is not None and SPEC.loader is not None
TOOL = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = TOOL
SPEC.loader.exec_module(TOOL)


def sample(
    stage_frame: int,
    fps: float,
    *,
    stage: int = 5,
    render_frames: int = 300,
    enemy_exact: int = 4,
    vertices: int = 12_000,
    vertices_peak: int = 180,
    fps_overlay_vertices: int = 0,
    fps_overlay_peak: int = 0,
    game_vertices: int | None = None,
    game_vertices_peak: int | None = None,
    draws: int = 800,
    render_perf_valid: int = 1,
    replay: int = 1,
    demo: int = 0,
    extra: str = "",
) -> str:
    if game_vertices is None:
        game_vertices = vertices - fps_overlay_vertices
    if game_vertices_peak is None:
        game_vertices_peak = vertices_peak
    return (
        "SAMPLE phase=stage_relative_periodic "
        f"frame={stage_frame + 1000} stage={stage} stage_frame={stage_frame} "
        f"replay={replay} demo={demo} fps={fps:.3f} "
        "enemy_reported=4/480 enemy_reported_peak=7 enemy_exact="
        f"{enemy_exact} enemy_exact_sampled_peak=7 enemy_hi=8 "
        "bullet_reported=20/1536 bullet_reported_peak=30 bullet_exact=20 "
        "bullet_exact_sampled_peak=30 bullet_hi=40 "
        "laser_exact=1/256 laser_exact_sampled_peak=2 laser_hi=3 "
        "item_reported=10/2096 item_reported_peak=14 item_array_exact=10 "
        "item_list_exact=10 item_exact_sampled_peak=14 item_hi=20 "
        f"render_arena_live=999 render_perf_valid={render_perf_valid} "
        f"render_frames={render_frames} render_draws_total={draws} "
        "render_draws_peak=9 "
        f"render_vertices_total={vertices} render_vertices_peak={vertices_peak} "
        f"render_fps_overlay_vertices_total={fps_overlay_vertices} "
        f"render_fps_overlay_vertices_peak={fps_overlay_peak} "
        f"render_game_vertices_total={game_vertices} "
        f"render_game_vertices_peak={game_vertices_peak} "
        "render_state_requested_total=500 render_state_requested_peak=7 "
        "render_item_drawn_total=60 render_item_drawn_peak=5 "
        f"{extra}\n"
    )


def fixed_stage_log(stage: int, fps: float) -> str:
    return "".join(
        sample(stage_frame, fps, stage=stage)
        for stage_frame in TOOL.FIXED_STAGE_SAMPLE_FRAMES
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


class ComparisonTests(TemporaryLogs):
    def test_explicit_stage_ignores_later_demo_without_timing_dilution(self) -> None:
        baseline = self.write(
            "baseline.log",
            fixed_stage_log(5, 40.0) + fixed_stage_log(3, 10.0),
        )
        candidate = self.write(
            "candidate.log",
            fixed_stage_log(5, 50.0) + fixed_stage_log(3, 100.0),
        )
        result = TOOL.compare_logs(
            baseline,
            candidate,
            bootstrap_iterations=20,
            stage=5,
        )
        self.assertEqual(result["sample_count"], 20)
        self.assertEqual(result["stage_filter"], 5)
        self.assertTrue(result["fixed_stage_route_required"])
        self.assertEqual({pair["stage"] for pair in result["pairs"]}, {5})
        self.assertAlmostEqual(result["timing"]["paired_improvement_ms_mean"], 5.0)

    def test_default_behavior_still_compares_all_stages(self) -> None:
        baseline = self.write(
            "baseline.log", sample(301, 50.0, stage=5) + sample(301, 50.0, stage=3)
        )
        candidate = self.write(
            "candidate.log", sample(301, 51.0, stage=5) + sample(301, 51.0, stage=3)
        )
        result = TOOL.compare_logs(baseline, candidate, bootstrap_iterations=20)
        self.assertEqual(result["sample_count"], 2)
        self.assertIsNone(result["stage_filter"])
        self.assertFalse(result["fixed_stage_route_required"])

    def test_appended_stage_note_is_an_independent_record_boundary(self) -> None:
        concatenated = (
            sample(301, 50.0).rstrip("\n")
            + " audio_bgm_by"
            + "STAGE_RELATIVE_PERF_SAMPLE stage=5 baseline_stage_frame=1 "
            + "sample_stage_frame=301 elapsed_stage_frames=300 "
            + "elapsed_game_frames=300 render_frames=300\n"
        )
        path = self.write("truncated-concatenated.log", concatenated)
        parsed = TOOL.parse_stage_relative_samples(path)
        self.assertEqual(len(parsed), 1)
        self.assertEqual(parsed[0]["stage"], 5)
        self.assertEqual(parsed[0]["stage_frame"], 301)
        self.assertEqual(parsed[0]["render_frames"], 300)
        self.assertNotIn("baseline_stage_frame", parsed[0])
        self.assertNotIn("sample_stage_frame", parsed[0])

    def test_aligned_workload_reports_paired_ms_wins_and_fixed_seed_ci(self) -> None:
        baseline = self.write(
            "baseline.log",
            "MARK phase=ignored\n"
            + sample(301, 40.0)
            + sample(601, 50.0)
            + sample(901, 60.0),
        )
        candidate = self.write(
            "candidate.log",
            sample(301, 50.0) + sample(601, 50.0) + sample(901, 50.0),
        )
        first = TOOL.compare_logs(
            baseline, candidate, bootstrap_iterations=1000, bootstrap_seed=123
        )
        second = TOOL.compare_logs(
            baseline, candidate, bootstrap_iterations=1000, bootstrap_seed=123
        )
        self.assertTrue(first["aligned"])
        self.assertTrue(first["strict_workload_match"])
        self.assertEqual(first["sample_count"], 3)
        self.assertEqual(first["timing"]["wins"], 1)
        self.assertEqual(first["timing"]["ties"], 1)
        self.assertEqual(first["timing"]["losses"], 1)
        self.assertEqual(
            first["timing"]["bootstrap_95_low_ms"],
            second["timing"]["bootstrap_95_low_ms"],
        )
        self.assertEqual(
            first["timing"]["bootstrap_95_high_ms"],
            second["timing"]["bootstrap_95_high_ms"],
        )
        self.assertAlmostEqual(first["pairs"][0]["improvement_ms"], 5.0)

    def test_vertices_difference_is_never_ignored(self) -> None:
        baseline = self.write("baseline.log", sample(301, 50.0, vertices=12000))
        candidate = self.write("candidate.log", sample(301, 51.0, vertices=12001))
        result = TOOL.compare_logs(baseline, candidate, bootstrap_iterations=20)
        self.assertFalse(result["strict_workload_match"])
        self.assertTrue(result["render_vertices_mismatch"])
        vertices = next(
            item
            for item in result["workload_differences"]
            if item["field"] == "render_vertices_total"
        )
        self.assertEqual(vertices["candidate_minus_baseline"], 1)
        rendered = TOOL.render_text(result)
        self.assertIn("strict_workload_match=false", rendered)
        self.assertIn("render_vertices_mismatch=true", rendered)
        self.assertIn("not ignored", rendered)

    def test_overlay_aware_route_accepts_only_owned_raw_vertex_drift(self) -> None:
        baseline = self.write("baseline.log", sample(301, 50.0))
        candidate = self.write(
            "candidate.log",
            sample(
                301,
                51.0,
                vertices=12_072,
                fps_overlay_vertices=72,
                fps_overlay_peak=6,
                game_vertices=12_000,
                game_vertices_peak=180,
            ),
        )
        result = TOOL.compare_logs(
            baseline,
            candidate,
            bootstrap_iterations=20,
            allow_fps_overlay_drift=True,
        )
        self.assertTrue(result["strict_workload_match"])
        self.assertTrue(result["exact_game_and_logical_workload_match"])
        self.assertTrue(result["fps_overlay_vertex_reconciliation_match"])
        self.assertTrue(result["render_vertices_mismatch"])
        item = result["fps_overlay_vertex_reconciliations"][0]
        self.assertTrue(item["match"])
        self.assertEqual(item["raw_total_delta"], 72)
        self.assertEqual(item["overlay_total_delta"], 72)
        self.assertIn("entirely the FPS overlay", result["render_vertices_mismatch_note"])

    def test_overlay_aware_route_rejects_game_vertex_drift(self) -> None:
        baseline = self.write("baseline.log", sample(301, 50.0))
        candidate = self.write(
            "candidate.log",
            sample(
                301,
                51.0,
                vertices=12_072,
                fps_overlay_vertices=66,
                fps_overlay_peak=6,
                game_vertices=12_006,
                game_vertices_peak=180,
            ),
        )
        result = TOOL.compare_logs(
            baseline,
            candidate,
            bootstrap_iterations=20,
            allow_fps_overlay_drift=True,
        )
        self.assertFalse(result["strict_workload_match"])
        self.assertFalse(result["exact_game_and_logical_workload_match"])
        self.assertFalse(result["fps_overlay_vertex_reconciliation_match"])
        self.assertFalse(result["fps_overlay_vertex_reconciliations"][0]["game_vertices_exact"])

    def test_overlay_aware_route_rejects_broken_per_run_total_accounting(self) -> None:
        baseline = self.write("baseline.log", sample(301, 50.0))
        candidate = self.write(
            "candidate.log",
            sample(
                301,
                51.0,
                vertices=12_072,
                fps_overlay_vertices=66,
                fps_overlay_peak=6,
                game_vertices=12_000,
                game_vertices_peak=180,
            ),
        )
        result = TOOL.compare_logs(
            baseline,
            candidate,
            bootstrap_iterations=20,
            allow_fps_overlay_drift=True,
        )
        self.assertFalse(result["strict_workload_match"])
        item = result["fps_overlay_vertex_reconciliations"][0]
        self.assertFalse(item["candidate_integrity"])
        self.assertFalse(item["raw_delta_matches_overlay_delta"])

    def test_overlay_aware_route_requires_valid_render_telemetry(self) -> None:
        baseline = self.write("baseline.log", sample(301, 50.0))
        candidate = self.write(
            "candidate.log", sample(301, 51.0, render_perf_valid=0)
        )
        result = TOOL.compare_logs(
            baseline,
            candidate,
            bootstrap_iterations=20,
            allow_fps_overlay_drift=True,
        )
        self.assertFalse(result["strict_workload_match"])
        self.assertFalse(result["render_telemetry_valid"])

    def test_overlay_aware_route_keeps_replay_context_strict(self) -> None:
        baseline = self.write("baseline.log", sample(301, 50.0))
        candidate = self.write("candidate.log", sample(301, 51.0, replay=0))
        result = TOOL.compare_logs(
            baseline,
            candidate,
            bootstrap_iterations=20,
            allow_fps_overlay_drift=True,
        )
        self.assertFalse(result["strict_workload_match"])
        difference = next(
            item
            for item in result["workload_differences"]
            if item["field"] == "replay"
        )
        self.assertFalse(difference["match"])

    def test_manager_and_dynamic_render_fields_are_all_compared(self) -> None:
        baseline = self.write(
            "baseline.log", sample(301, 50.0, enemy_exact=4, extra="render_new_total=2")
        )
        candidate = self.write(
            "candidate.log", sample(301, 50.0, enemy_exact=5, extra="render_new_total=9")
        )
        result = TOOL.compare_logs(baseline, candidate, bootstrap_iterations=20)
        by_field = {item["field"]: item for item in result["workload_differences"]}
        self.assertFalse(result["strict_workload_match"])
        self.assertEqual(by_field["enemy_exact"]["candidate_minus_baseline"], 1)
        self.assertEqual(by_field["render_new_total"]["candidate_minus_baseline"], 7)
        self.assertEqual(by_field["render_new_total"]["category"], "render")
        self.assertNotIn("render_arena_live", by_field)
        self.assertNotIn("render_frames", by_field)

    def test_bullet_enumerator_diagnostics_do_not_define_workload(self) -> None:
        baseline = self.write(
            "baseline.log",
            sample(
                301,
                50.0,
                extra=(
                    "bullet_enum_frames=0 bullet_enum_slot_probes=0 "
                    "bullet_enum_word_probes=0 bullet_enum_visited=0 "
                    "bullet_enum_fallback_frames=0"
                ),
            ),
        )
        candidate = self.write(
            "candidate.log",
            sample(
                301,
                51.0,
                extra=(
                    "bullet_enum_frames=300 bullet_enum_slot_probes=90000 "
                    "bullet_enum_word_probes=120000 bullet_enum_visited=80000 "
                    "bullet_enum_fallback_frames=3"
                ),
            ),
        )
        result = TOOL.compare_logs(baseline, candidate, bootstrap_iterations=20)
        self.assertTrue(result["strict_workload_match"])
        compared = {
            item["field"] for item in result["workload_differences"]
        }
        self.assertTrue(compared.isdisjoint(TOOL.MANAGER_DIAGNOSTIC_FIELDS))
        self.assertEqual(
            set(result["workload_fields"]["diagnostic_excluded"]),
            set(TOOL.MANAGER_DIAGNOSTIC_FIELDS),
        )

    def test_field_missing_on_one_side_is_a_visible_strict_mismatch(self) -> None:
        baseline = self.write(
            "baseline.log", sample(301, 50.0, extra="render_optional_total=10")
        )
        candidate = self.write("candidate.log", sample(301, 50.0))
        result = TOOL.compare_logs(baseline, candidate, bootstrap_iterations=20)
        difference = next(
            item
            for item in result["workload_differences"]
            if item["field"] == "render_optional_total"
        )
        self.assertFalse(result["strict_workload_match"])
        self.assertEqual(difference["mismatched_pairs"], 1)
        self.assertEqual(difference["mismatches"][0]["candidate"], None)


class RejectionTests(TemporaryLogs):
    def assert_comparison_error(self, baseline_text: str, candidate_text: str) -> None:
        baseline = self.write("baseline.log", baseline_text)
        candidate = self.write("candidate.log", candidate_text)
        with self.assertRaises(TOOL.ComparisonError):
            TOOL.compare_logs(baseline, candidate, bootstrap_iterations=20)

    def test_boundary_shift_between_runs_is_rejected(self) -> None:
        self.assert_comparison_error(sample(301, 50.0), sample(302, 50.0))

    def test_duplicate_sample_is_rejected(self) -> None:
        self.assert_comparison_error(
            sample(301, 50.0) + sample(301, 50.0), sample(301, 50.0)
        )

    def test_wrong_render_frame_count_is_rejected(self) -> None:
        self.assert_comparison_error(
            sample(301, 50.0, render_frames=299), sample(301, 50.0)
        )

    def test_same_missing_300_frame_window_is_still_rejected(self) -> None:
        sparse = sample(301, 50.0) + sample(901, 50.0)
        self.assert_comparison_error(sparse, sparse)

    def test_missing_file_is_rejected(self) -> None:
        existing = self.write("candidate.log", sample(301, 50.0))
        with self.assertRaises(TOOL.ComparisonError):
            TOOL.compare_logs(
                self.directory / "absent.log", existing, bootstrap_iterations=20
            )

    def test_overlay_aware_route_requires_complete_owner_schema(self) -> None:
        baseline = self.write("baseline.log", sample(301, 50.0))
        candidate = self.write(
            "candidate.log",
            sample(301, 51.0).replace(
                "render_fps_overlay_vertices_total=0 ", "", 1
            ),
        )
        with self.assertRaises(TOOL.ComparisonError):
            TOOL.compare_logs(
                baseline,
                candidate,
                bootstrap_iterations=20,
                allow_fps_overlay_drift=True,
            )

    def test_explicit_stage_rejects_missing_or_extra_fixed_window(self) -> None:
        complete = fixed_stage_log(5, 50.0)
        missing = "".join(complete.splitlines(keepends=True)[:-1])
        extra = complete + sample(6301, 50.0, stage=5)
        for malformed in (missing, extra):
            with self.subTest(lines=len(malformed.splitlines())):
                baseline = self.write("baseline.log", complete)
                candidate = self.write("candidate.log", malformed)
                with self.assertRaises(TOOL.ComparisonError):
                    TOOL.compare_logs(
                        baseline,
                        candidate,
                        bootstrap_iterations=20,
                        stage=5,
                    )

    def test_explicit_stage_rejects_absent_stage(self) -> None:
        stage3 = fixed_stage_log(3, 50.0)
        baseline = self.write("baseline.log", stage3)
        candidate = self.write("candidate.log", stage3)
        with self.assertRaises(TOOL.ComparisonError):
            TOOL.compare_logs(
                baseline, candidate, bootstrap_iterations=20, stage=5
            )


class CommandLineTests(TemporaryLogs):
    def run_tool(self, baseline: Path, candidate: Path, *arguments: str) -> subprocess.CompletedProcess[str]:
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

    def test_json_mode_is_machine_readable_and_success_is_zero(self) -> None:
        baseline = self.write("baseline.log", sample(301, 50.0))
        candidate = self.write("candidate.log", sample(301, 51.0))
        completed = self.run_tool(baseline, candidate, "--json")
        self.assertEqual(completed.returncode, 0, completed.stderr)
        parsed = json.loads(completed.stdout)
        self.assertEqual(parsed["schema"], "th08_stage_relative_perf_comparison_v1")
        self.assertTrue(parsed["strict_workload_match"])

    def test_workload_mismatch_emits_report_and_returns_one(self) -> None:
        baseline = self.write("baseline.log", sample(301, 50.0, vertices=100))
        candidate = self.write("candidate.log", sample(301, 51.0, vertices=101))
        completed = self.run_tool(baseline, candidate)
        self.assertEqual(completed.returncode, 1, completed.stderr)
        self.assertIn("strict_workload_match=false", completed.stdout)

    def test_overlay_aware_cli_accepts_only_reconciled_raw_vertex_drift(self) -> None:
        baseline = self.write("baseline.log", sample(301, 50.0))
        candidate = self.write(
            "candidate.log",
            sample(
                301,
                51.0,
                vertices=12_072,
                fps_overlay_vertices=72,
                fps_overlay_peak=6,
                game_vertices=12_000,
                game_vertices_peak=180,
            ),
        )
        completed = self.run_tool(
            baseline, candidate, "--allow-fps-overlay-drift", "--json"
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        parsed = json.loads(completed.stdout)
        self.assertTrue(parsed["allow_fps_overlay_drift"])
        self.assertTrue(parsed["fps_overlay_vertex_reconciliation_match"])

    def test_invalid_boundary_returns_two(self) -> None:
        baseline = self.write("baseline.log", sample(301, 50.0))
        candidate = self.write("candidate.log", sample(302, 51.0))
        completed = self.run_tool(baseline, candidate)
        self.assertEqual(completed.returncode, 2)
        self.assertIn("alignment failure", completed.stderr)

    def test_stage_cli_filters_later_demo_and_reports_twenty_samples(self) -> None:
        baseline = self.write(
            "baseline.log", fixed_stage_log(5, 40.0) + fixed_stage_log(3, 10.0)
        )
        candidate = self.write(
            "candidate.log", fixed_stage_log(5, 50.0) + fixed_stage_log(3, 100.0)
        )
        completed = self.run_tool(baseline, candidate, "--stage", "5", "--json")
        self.assertEqual(completed.returncode, 0, completed.stderr)
        result = json.loads(completed.stdout)
        self.assertEqual(result["stage_filter"], 5)
        self.assertEqual(result["sample_count"], 20)


if __name__ == "__main__":
    unittest.main()
