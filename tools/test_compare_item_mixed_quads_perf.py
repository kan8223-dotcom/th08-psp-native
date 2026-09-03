#!/usr/bin/env python3
"""Focused contracts for compare_item_mixed_quads_perf.py."""

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
TOOL_PATH = TOOLS / "compare_item_mixed_quads_perf.py"
sys.path.insert(0, str(TOOLS))
SPEC = importlib.util.spec_from_file_location(
    "compare_item_mixed_quads_perf", TOOL_PATH
)
assert SPEC is not None and SPEC.loader is not None
TOOL = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = TOOL
SPEC.loader.exec_module(TOOL)


def replace_occurrence(text: str, old: str, new: str, occurrence: int) -> str:
    """Replace one 1-based occurrence without accidentally editing the origin MARK."""
    start = -1
    for _ in range(occurrence):
        start = text.find(old, start + 1)
        if start < 0:
            raise AssertionError(
                f"fixture does not contain occurrence {occurrence} of {old!r}"
            )
    return text[:start] + new + text[start + len(old) :]


def sample(
    stage_frame: int,
    fps: float,
    *,
    frame_offset: int,
    vertices: int,
    vertices_peak: int,
    emitted: int,
    emitted_peak: int,
    draws: int = 800,
    requested: int = 500,
    upload_attempts: int = 700,
    replay: int = 1,
    demo: int = 1,
) -> str:
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
        f"render_vertices_total={vertices} render_vertices_peak={vertices_peak} "
        f"render_state_requested_total={requested} render_state_requested_peak=7 "
        f"render_state_emitted_total={emitted} render_state_emitted_peak={emitted_peak} "
        "render_matrix_recompute_total=20 render_matrix_recompute_peak=2 "
        "render_vfpu_sincos_total=30 render_vfpu_sincos_peak=1 "
        "render_effect_active_total=90 render_effect_active_peak=4 "
        "render_effect_drawn_total=80 render_effect_drawn_peak=4 "
        "render_item_drawn_total=60 render_item_drawn_peak=5 "
        f"render_upload_attempt_total={upload_attempts} render_upload_attempt_peak=9 "
        "render_actual_upload_total=3 render_actual_upload_peak=1 "
        "render_upload_bytes_total=4096 render_upload_bytes_peak=4096 "
        "render_text_bytes_total=0 render_text_bytes_peak=0\n"
    )


def frontend_record(
    stage_frame: int,
    *,
    kind: str = "SAMPLE",
    phase: str = "stage_relative_periodic",
    frame_offset: int = 2000,
    passes: int = 300,
    pairs: int = 5,
    general: int = 3,
    batches: int = 2,
    backend_fallbacks: int = 0,
) -> str:
    if general:
        sticky, axis = 1, general - 1
    else:
        sticky = axis = 0
    potential_frontend = pairs * 4 + general * 2
    ge_saved = pairs * 4
    return (
        f"ITEM_MIXED_QUADS kind={kind} phase={phase} mode=product "
        f"frame={stage_frame + frame_offset} stage=5 stage_frame={stage_frame} "
        f"passes={passes} owner_conflict_passes=0 state_runs={batches} "
        f"batches={batches} candidates={pairs + general} "
        f"eligible_prefix_quads={pairs} general_quads={general} "
        f"sticky_general_quads={sticky} nonfinite_fallbacks=0 "
        f"axis_fallbacks={axis} area_or_mirror_fallbacks=0 "
        "z_or_w_fallbacks=0 uv_fallbacks=0 diffuse_fallbacks=0 "
        f"submitted_batches={batches} submitted_pair_quads={pairs} "
        f"submitted_general_quads={general} "
        f"backend_fallback_batches={backend_fallbacks} "
        "fail_closed_batches=0 missing_run_batches=0 invalid_range_batches=0 "
        "canonical_recovery_draw_failures=0 canonical_recovery_quads=0 "
        f"potential_frontend_vertices_saved={potential_frontend} "
        f"potential_ge_vertices_saved={ge_saved} "
        f"submitted_frontend_vertices_saved={potential_frontend} "
        f"submitted_ge_vertices_saved={ge_saved} "
        f"max_pair_prefix={min(pairs, 4)} max_general_suffix={min(general, 2)}\n"
    )


def backend_record(
    stage_frame: int,
    *,
    kind: str,
    phase: str,
    attempts: int,
    batches: int,
    quads: int,
    frame_offset: int = 2000,
    item_fallbacks: int = 0,
    item_exhaustions: int = 0,
    bullet_attempts: int = 0,
    high_water: int = 160,
    valid: int = 1,
) -> str:
    return (
        f"MIXED_GE_BACKEND_TELEMETRY kind={kind} phase={phase} "
        f"frame={stage_frame + frame_offset} stage=5 stage_frame={stage_frame} "
        f"valid={valid} counter_scope=device_lifetime cumulative=1 "
        "counter_bits=32_wrap "
        f"bullet_attempts={bullet_attempts} bullet_submitted_batches=0 "
        "bullet_submitted_quads=0 bullet_fallbacks=0 "
        "bullet_arena_exhaustions=0 "
        f"item_attempts={attempts} item_submitted_batches={batches} "
        f"item_submitted_quads={quads} item_fallbacks={item_fallbacks} "
        f"item_arena_exhaustions={item_exhaustions} "
        f"shared_arena_high_water_vertices={high_water} "
        "shared_arena_capacity_vertices=98304\n"
    )


def matched_logs(
    *, baseline_fps: float = 40.0, candidate_fps: float = 50.0
) -> tuple[str, str]:
    baseline_parts: list[str] = []
    candidate_parts = [
        backend_record(
            0,
            kind="MARK",
            phase="stage_setup_complete",
            attempts=0,
            batches=0,
            quads=0,
            high_water=0,
        )
    ]
    attempts = batches = quads = 0
    for _, stage_frame in TOOL.EXPECTED_KEYS:
        baseline_parts.append(
            sample(
                stage_frame,
                baseline_fps,
                frame_offset=1000,
                vertices=12_000,
                vertices_peak=180,
                emitted=5_000,
                emitted_peak=80,
            )
        )
        attempts += 2
        batches += 2
        quads += 8
        candidate_parts.append(
            backend_record(
                stage_frame,
                kind="SAMPLE",
                phase="stage_relative_periodic",
                attempts=attempts,
                batches=batches,
                quads=quads,
            )
        )
        candidate_parts.append(
            sample(
                stage_frame,
                candidate_fps,
                frame_offset=2000,
                vertices=11_980,
                vertices_peak=160,
                emitted=4_982,
                emitted_peak=71,
            )
        )
        candidate_parts.append(frontend_record(stage_frame))
    candidate_parts.append(
        backend_record(
            TOOL.EXPECTED_TEARDOWN_STAGE_FRAME,
            kind="MARK",
            phase="stage_teardown_complete",
            attempts=attempts,
            batches=batches,
            quads=quads,
        )
    )
    candidate_parts.append(
        frontend_record(
            TOOL.EXPECTED_TEARDOWN_STAGE_FRAME,
            kind="MARK",
            phase="stage_teardown_complete",
            passes=(
                TOOL.EXPECTED_TEARDOWN_STAGE_FRAME - TOOL.EXPECTED_KEYS[-1][1]
            ),
            pairs=0,
            general=0,
            batches=0,
        )
    )
    return "".join(baseline_parts), "".join(candidate_parts)


class TemporaryComparison(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.directory = Path(self.temporary.name)
        self.replay_base = self.write_bytes("replay-base.bin", b"replay-authority")
        self.replay_candidate = self.write_bytes(
            "replay-candidate.bin", b"replay-authority"
        )
        self.surface_base = self.write_bytes("surface-base.bin", b"surface-pixels")
        self.surface_candidate = self.write_bytes(
            "surface-candidate.bin", b"surface-pixels"
        )

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def write(self, name: str, text: str) -> Path:
        path = self.directory / name
        path.write_text(text, encoding="utf-8")
        return path

    def write_bytes(self, name: str, data: bytes) -> Path:
        path = self.directory / name
        path.write_bytes(data)
        return path

    def compare(
        self,
        baseline: str,
        candidate: str,
        *,
        iterations: int = 50,
        replay_pair=None,
        surface_pairs=None,
    ):
        return TOOL.compare_logs(
            self.write("baseline.log", baseline),
            self.write("candidate.log", candidate),
            replay_pair=replay_pair
            or (self.replay_base, self.replay_candidate),
            surface_pairs=surface_pairs
            or [("surface0", self.surface_base, self.surface_candidate)],
            bootstrap_iterations=iterations,
            bootstrap_seed=123,
        )


class ValidComparisonTests(TemporaryComparison):
    def test_later_stage3_demo_does_not_dilute_stage5_timing(self) -> None:
        baseline, candidate = matched_logs()
        for _, stage_frame in TOOL.EXPECTED_KEYS:
            baseline += sample(
                stage_frame,
                10.0,
                frame_offset=10_000,
                vertices=12_000,
                vertices_peak=180,
                emitted=5_000,
                emitted_peak=80,
            ).replace(" stage=5 ", " stage=3 ", 1)
            candidate += sample(
                stage_frame,
                100.0,
                frame_offset=20_000,
                vertices=11_980,
                vertices_peak=160,
                emitted=4_982,
                emitted_peak=71,
            ).replace(" stage=5 ", " stage=3 ", 1)
        result = self.compare(baseline, candidate)
        self.assertTrue(result["comparison_valid"])
        self.assertEqual(result["sample_count"], 20)
        self.assertEqual({pair["stage"] for pair in result["pairs"]}, {5})
        self.assertAlmostEqual(result["timing"]["paired_improvement_ms_mean"], 5.0)

    def test_twenty_windows_reconcile_frontend_backend_render_and_artifacts(self) -> None:
        baseline, candidate = matched_logs()
        result = self.compare(baseline, candidate)
        self.assertTrue(result["comparison_valid"])
        self.assertTrue(result["exact_workload_match"])
        self.assertTrue(result["reconciliation_match"])
        self.assertTrue(result["artifact_identity_match"])
        self.assertEqual(result["sample_count"], 20)
        self.assertEqual(result["frontend_intervals"][0]["ge_saved"], 20)
        self.assertEqual(result["backend_intervals"][0]["item_submitted_quads"], 8)
        self.assertEqual(
            result["render_reconciliations"][0]["expected_state_emitted_saved"],
            18,
        )
        self.assertTrue(result["performance_gate_passed"])
        self.assertFalse(result["bootstrap_iterations_valid"])
        self.assertFalse(result["acceptance_passed"])
        self.assertEqual(result["verdict"], "NO-GO")

    def test_registered_bootstrap_contract_can_produce_go(self) -> None:
        baseline, candidate = matched_logs()
        with mock.patch.object(
            TOOL.stage_perf, "paired_bootstrap_ci", return_value=(4.9, 5.1)
        ):
            result = self.compare(
                baseline,
                candidate,
                iterations=TOOL.REQUIRED_BOOTSTRAP_ITERATIONS,
            )
        self.assertTrue(result["acceptance_passed"])
        self.assertEqual(result["verdict"], "GO")


class HardRejectionTests(TemporaryComparison):
    def failed_rule(self, result, name: str) -> bool:
        return not next(
            rule for rule in result["reconciliations"] if rule["rule"] == name
        )["passed"]

    def test_route_must_be_exact_twenty_stage5_windows(self) -> None:
        baseline, candidate = matched_logs()
        baseline = "\n".join(baseline.splitlines()[:-1]) + "\n"
        with self.assertRaises(TOOL.stage_perf.ComparisonError):
            self.compare(baseline, candidate)

    def test_unexplained_render_difference_is_immediate_no_go(self) -> None:
        baseline, candidate = matched_logs()
        candidate = candidate.replace("render_draws_total=800", "render_draws_total=801", 1)
        result = self.compare(baseline, candidate)
        self.assertFalse(result["comparison_valid"])
        self.assertFalse(result["exact_workload_match"])
        self.assertEqual(result["verdict"], "NO-GO")

    def test_wrong_vertex_delta_is_immediate_no_go(self) -> None:
        baseline, candidate = matched_logs()
        candidate = candidate.replace(
            "render_vertices_total=11980", "render_vertices_total=11981", 1
        )
        result = self.compare(baseline, candidate)
        self.assertTrue(self.failed_rule(result, "render_vertex_total_exact"))
        self.assertFalse(result["comparison_valid"])

    def test_frontend_fallback_is_immediate_no_go(self) -> None:
        baseline, candidate = matched_logs()
        candidate = candidate.replace("backend_fallback_batches=0", "backend_fallback_batches=1", 1)
        result = self.compare(baseline, candidate)
        self.assertTrue(self.failed_rule(result, "item_frontend_failures_zero"))
        self.assertFalse(result["comparison_valid"])

    def test_backend_fallback_is_immediate_no_go(self) -> None:
        baseline, candidate = matched_logs()
        candidate = candidate.replace(
            "item_attempts=2 item_submitted_batches=2",
            "item_attempts=2 item_submitted_batches=1",
            1,
        )
        candidate = replace_occurrence(
            candidate, "item_fallbacks=0", "item_fallbacks=1", 2
        )
        result = self.compare(baseline, candidate)
        self.assertTrue(self.failed_rule(result, "item_backend_failures_zero"))
        self.assertFalse(result["comparison_valid"])

    def test_arena_exhaustion_is_immediate_no_go(self) -> None:
        baseline, candidate = matched_logs()
        candidate = replace_occurrence(
            candidate,
            "item_arena_exhaustions=0",
            "item_arena_exhaustions=1",
            2,
        )
        result = self.compare(baseline, candidate)
        self.assertTrue(self.failed_rule(result, "item_backend_failures_zero"))
        self.assertFalse(result["comparison_valid"])

    def test_bullet_owner_activity_is_immediate_no_go(self) -> None:
        baseline, candidate = matched_logs()
        candidate = replace_occurrence(
            candidate, "bullet_attempts=0", "bullet_attempts=1", 2
        )
        result = self.compare(baseline, candidate)
        self.assertTrue(self.failed_rule(result, "bullet_backend_owner_quiescent"))
        self.assertFalse(result["comparison_valid"])

    def test_backend_frontend_count_mismatch_is_immediate_no_go(self) -> None:
        baseline, candidate = matched_logs()
        candidate = candidate.replace("item_submitted_quads=8", "item_submitted_quads=7", 1)
        result = self.compare(baseline, candidate)
        self.assertTrue(self.failed_rule(result, "item_backend_matches_frontend"))
        self.assertFalse(result["comparison_valid"])

    def test_replay_artifact_mismatch_is_immediate_no_go(self) -> None:
        baseline, candidate = matched_logs()
        self.replay_candidate.write_bytes(b"desynchronized")
        result = self.compare(baseline, candidate)
        self.assertFalse(result["replay_identity_match"])
        self.assertFalse(result["comparison_valid"])
        self.assertIn("replay_artifact_identity", result["hard_gate_failures"])

    def test_surface_artifact_mismatch_is_immediate_no_go(self) -> None:
        baseline, candidate = matched_logs()
        self.surface_candidate.write_bytes(b"different pixels")
        result = self.compare(baseline, candidate)
        self.assertFalse(result["surface_identity_match"])
        self.assertFalse(result["comparison_valid"])
        self.assertIn("surface_artifact_identity", result["hard_gate_failures"])

    def test_artifact_pairs_are_mandatory_inputs(self) -> None:
        baseline, candidate = matched_logs()
        baseline_path = self.write("baseline.log", baseline)
        candidate_path = self.write("candidate.log", candidate)
        with self.assertRaises(TOOL.stage_perf.ComparisonError):
            TOOL.compare_logs(
                baseline_path,
                candidate_path,
                replay_pair=None,
                surface_pairs=[("surface0", self.surface_base, self.surface_candidate)],
                bootstrap_iterations=10,
            )
        with self.assertRaises(TOOL.stage_perf.ComparisonError):
            TOOL.compare_logs(
                baseline_path,
                candidate_path,
                replay_pair=(self.replay_base, self.replay_candidate),
                surface_pairs=[],
                bootstrap_iterations=10,
            )

    def test_replay_context_mismatch_is_immediate_no_go(self) -> None:
        baseline, candidate = matched_logs()
        candidate = candidate.replace("replay=1 demo=1", "replay=1 demo=0", 1)
        result = self.compare(baseline, candidate)
        self.assertFalse(result["exact_workload_match"])
        self.assertTrue(self.failed_rule(result, "replay_context"))
        self.assertFalse(result["comparison_valid"])

    def test_nonpositive_bootstrap_lower_bound_is_no_go(self) -> None:
        baseline, candidate = matched_logs()
        with mock.patch.object(
            TOOL.stage_perf, "paired_bootstrap_ci", return_value=(-0.01, 5.1)
        ):
            result = self.compare(
                baseline,
                candidate,
                iterations=TOOL.REQUIRED_BOOTSTRAP_ITERATIONS,
            )
        self.assertTrue(result["comparison_valid"])
        self.assertFalse(result["performance_gate_passed"])
        self.assertFalse(result["acceptance_passed"])
        self.assertEqual(result["verdict"], "NO-GO")


class CommandLineTests(TemporaryComparison):
    def test_json_low_iteration_run_returns_no_go_exit_one(self) -> None:
        baseline, candidate = matched_logs()
        completed = subprocess.run(
            [
                sys.executable,
                str(TOOL_PATH),
                str(self.write("cli-baseline.log", baseline)),
                str(self.write("cli-candidate.log", candidate)),
                "--replay-pair",
                str(self.replay_base),
                str(self.replay_candidate),
                "--surface-pair",
                "surface0",
                str(self.surface_base),
                str(self.surface_candidate),
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
        self.assertFalse(result["acceptance_passed"])
        self.assertEqual(result["verdict"], "NO-GO")


if __name__ == "__main__":
    unittest.main(verbosity=2)
