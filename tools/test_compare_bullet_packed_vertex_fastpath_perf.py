#!/usr/bin/env python3
"""Host contracts for compare_bullet_packed_vertex_fastpath_perf.py."""

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
TOOL_PATH = TOOLS / "compare_bullet_packed_vertex_fastpath_perf.py"
sys.path.insert(0, str(TOOLS))
SPEC = importlib.util.spec_from_file_location(
    "compare_bullet_packed_vertex_fastpath_perf", TOOL_PATH
)
assert SPEC is not None and SPEC.loader is not None
TOOL = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = TOOL
SPEC.loader.exec_module(TOOL)


def replace_occurrence(text: str, old: str, new: str, occurrence: int) -> str:
    start = -1
    for _ in range(occurrence):
        start = text.find(old, start + 1)
        if start < 0:
            raise AssertionError(
                f"fixture lacks occurrence {occurrence} of {old!r}"
            )
    return text[:start] + new + text[start + len(old) :]


def sample(
    stage_frame: int,
    fps: float,
    *,
    frame_offset: int,
    replay: int = 1,
    demo: int = 1,
    vertices: int = 12_000,
    vertex_peak: int = 180,
    fps_overlay_vertices: int = 0,
    fps_overlay_peak: int = 0,
    game_vertices: int | None = None,
    game_vertex_peak: int | None = None,
    draws: int = 800,
    state_requested: int = 500,
    state_emitted: int = 450,
    upload_attempts: int = 700,
) -> str:
    if game_vertices is None:
        game_vertices = vertices - fps_overlay_vertices
    if game_vertex_peak is None:
        game_vertex_peak = vertex_peak
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
        f"render_vertices_total={vertices} render_vertices_peak={vertex_peak} "
        f"render_fps_overlay_vertices_total={fps_overlay_vertices} "
        f"render_fps_overlay_vertices_peak={fps_overlay_peak} "
        f"render_game_vertices_total={game_vertices} "
        f"render_game_vertices_peak={game_vertex_peak} "
        f"render_state_requested_total={state_requested} "
        "render_state_requested_peak=7 "
        f"render_state_emitted_total={state_emitted} "
        "render_state_emitted_peak=6 "
        "render_matrix_recompute_total=20 render_matrix_recompute_peak=2 "
        "render_vfpu_sincos_total=30 render_vfpu_sincos_peak=1 "
        "render_effect_active_total=90 render_effect_active_peak=4 "
        "render_effect_drawn_total=80 render_effect_drawn_peak=4 "
        "render_item_drawn_total=60 render_item_drawn_peak=5 "
        f"render_upload_attempt_total={upload_attempts} "
        "render_upload_attempt_peak=9 "
        "render_actual_upload_total=3 render_actual_upload_peak=1 "
        "render_upload_bytes_total=4096 render_upload_bytes_peak=4096 "
        "render_text_bytes_total=0 render_text_bytes_peak=0\n"
    )


def zero_state() -> dict[str, int]:
    return {field: 0 for field in TOOL.COUNTER_FIELDS}


def fastpath_record(
    *,
    kind: str,
    phase: str,
    stage_frame: int,
    state: dict[str, int],
    frame_offset: int = 2000,
    max_run: int = 0,
    high_water: int = 0,
    capacity: int = TOOL.EXPECTED_ARENA_CAPACITY_VERTICES,
    valid: int = 1,
    stage: int = 5,
) -> str:
    return (
        f"{TOOL.FASTPATH_PREFIX} kind={kind} phase={phase} "
        f"frame={stage_frame + frame_offset} stage={stage} "
        f"stage_frame={stage_frame} valid={valid} "
        "counter_scope=device_lifetime cumulative=1 counter_bits=32_wrap "
        f"begin_attempts={state['begin_attempts']} "
        f"accepted_batches={state['accepted_batches']} "
        f"canonical_fallback_batches={state['canonical_fallback_batches']} "
        f"append_attempts={state['append_attempts']} "
        f"appended_quads={state['appended_quads']} "
        f"packed_vertices={state['packed_vertices']} "
        f"uniform_diffuse_quads={state['uniform_diffuse_quads']} "
        f"per_vertex_diffuse_quads={state['per_vertex_diffuse_quads']} "
        f"submit_attempts={state['submit_attempts']} "
        f"submitted_runs={state['submitted_runs']} "
        f"submitted_quads={state['submitted_quads']} "
        f"native_submits={state['native_submits']} "
        f"native_submitted_quads={state['native_submitted_quads']} "
        f"client_fallback_submits={state['client_fallback_submits']} "
        f"client_fallback_quads={state['client_fallback_quads']} "
        f"owner_fallbacks={state['owner_fallbacks']} "
        f"state_fallbacks={state['state_fallbacks']} "
        f"index_fallbacks={state['index_fallbacks']} "
        f"capacity_fallbacks={state['capacity_fallbacks']} "
        f"contract_fallbacks={state['contract_fallbacks']} "
        f"abandoned_runs={state['abandoned_runs']} "
        f"abandoned_quads={state['abandoned_quads']} "
        f"recovery_split_runs={state['recovery_split_runs']} "
        f"recovery_split_quads={state['recovery_split_quads']} "
        f"max_run_quads={max_run} "
        f"arena_high_water_vertices={high_water} "
        f"arena_capacity_vertices={capacity} "
        f"frontend_28b_bytes_avoided={state['frontend_28b_bytes_avoided']} "
        f"packed_24b_bytes={state['packed_24b_bytes']} "
        "manager_28B_staging_writes=0 packed_generation_passes=1 "
        "extra_flush=0 topology=4v_indexed\n"
    )


def add_interval(
    state: dict[str, int],
    *,
    frames: int,
    quads: int,
    runs: int,
    client_runs: int = 0,
    client_quads: int = 0,
) -> None:
    native_runs = runs - client_runs
    native_quads = quads - client_quads
    state["begin_attempts"] += frames
    state["accepted_batches"] += frames
    state["append_attempts"] += quads
    state["appended_quads"] += quads
    state["packed_vertices"] += quads * 4
    state["uniform_diffuse_quads"] += quads
    state["submit_attempts"] += runs
    state["submitted_runs"] += runs
    state["submitted_quads"] += quads
    state["native_submits"] += native_runs
    state["native_submitted_quads"] += native_quads
    state["client_fallback_submits"] += client_runs
    state["client_fallback_quads"] += client_quads
    state["frontend_28b_bytes_avoided"] += quads * 4 * 28
    state["packed_24b_bytes"] += quads * 4 * 24


def matched_logs(
    *, baseline_fps: float = 40.0, candidate_fps: float = 50.0
) -> tuple[str, str]:
    baseline_parts: list[str] = []
    state = zero_state()
    candidate_parts = [
        fastpath_record(
            kind="MARK",
            phase="stage_setup_complete",
            stage_frame=0,
            state=state,
        )
    ]
    for _, stage_frame in TOOL.EXPECTED_KEYS:
        baseline_parts.append(
            sample(stage_frame, baseline_fps, frame_offset=1000)
        )
        add_interval(state, frames=300, quads=8, runs=2)
        candidate_parts.append(
            fastpath_record(
                kind="SAMPLE",
                phase="stage_relative_periodic",
                stage_frame=stage_frame,
                state=state,
                max_run=4,
                high_water=32,
            )
        )
        candidate_parts.append(
            sample(stage_frame, candidate_fps, frame_offset=2000)
        )
    add_interval(state, frames=119, quads=0, runs=0)
    candidate_parts.append(
        fastpath_record(
            kind="MARK",
            phase="stage_teardown_complete",
            stage_frame=TOOL.EXPECTED_TEARDOWN_STAGE_FRAME,
            state=state,
            max_run=4,
            high_water=32,
        )
    )
    return "".join(baseline_parts), "".join(candidate_parts)


class TemporaryComparison(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.directory = Path(self.temporary.name)
        self.surface0_base = self.write_bytes("surface0-base.bin", b"surface-0")
        self.surface0_candidate = self.write_bytes(
            "surface0-candidate.bin", b"surface-0"
        )
        self.surface8_base = self.write_bytes("surface8-base.bin", b"surface-8")
        self.surface8_candidate = self.write_bytes(
            "surface8-candidate.bin", b"surface-8"
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

    @property
    def surfaces(self):
        return [
            ("surface0", self.surface0_base, self.surface0_candidate),
            ("surface8", self.surface8_base, self.surface8_candidate),
        ]

    def compare(
        self,
        baseline: str,
        candidate: str,
        *,
        iterations: int = 50,
        surfaces=None,
    ):
        return TOOL.compare_logs(
            self.write("baseline.log", baseline),
            self.write("candidate.log", candidate),
            surface_pairs=self.surfaces if surfaces is None else surfaces,
            bootstrap_iterations=iterations,
            bootstrap_seed=123,
        )

    @staticmethod
    def rule_failed(result, name: str) -> bool:
        return not next(
            rule
            for rule in result["reconciliations"]
            if rule["rule"] == name
        )["passed"]


class ValidComparisonTests(TemporaryComparison):
    def test_twenty_windows_reconcile_exact_workload_and_m1_algebra(self) -> None:
        baseline, candidate = matched_logs()
        result = self.compare(baseline, candidate)
        self.assertTrue(result["comparison_valid"])
        self.assertTrue(result["exact_workload_match"])
        self.assertTrue(result["fastpath_reconciliation_match"])
        self.assertTrue(result["surface_identity_match"])
        self.assertEqual(result["sample_count"], 20)
        self.assertEqual(result["fastpath_intervals"][0]["begin_attempts"], 300)
        self.assertEqual(result["fastpath_intervals"][0]["submitted_quads"], 8)
        self.assertTrue(result["performance_gate_passed"])
        self.assertFalse(result["bootstrap_iterations_valid"])
        self.assertFalse(result["acceptance_passed"])
        self.assertEqual(result["verdict"], "NO-GO")

    def test_owned_fps_overlay_delta_preserves_strict_game_workload(self) -> None:
        baseline, candidate = matched_logs()
        candidate = candidate.replace(
            "render_vertices_total=12000",
            "render_vertices_total=12072",
            1,
        ).replace(
            "render_fps_overlay_vertices_total=0",
            "render_fps_overlay_vertices_total=72",
            1,
        ).replace(
            "render_fps_overlay_vertices_peak=0",
            "render_fps_overlay_vertices_peak=6",
            1,
        )
        result = self.compare(baseline, candidate)
        self.assertTrue(result["comparison_valid"])
        self.assertTrue(result["exact_workload_match"])
        self.assertTrue(result["ordinary_exact_workload_match"])
        self.assertTrue(result["fps_overlay_vertex_reconciliation_match"])
        self.assertFalse(result["general_strict_workload_match"])
        first = result["fps_overlay_vertex_reconciliations"][0]
        self.assertEqual(first["raw_total_delta"], 72)
        self.assertEqual(first["overlay_total_delta"], 72)
        self.assertTrue(first["game_vertices_exact"])

    def test_one_million_positive_ci_is_the_only_go(self) -> None:
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

    def test_any_client_fallback_is_rejected(self) -> None:
        baseline, _ = matched_logs()
        state = zero_state()
        candidate_parts = [
            fastpath_record(
                kind="MARK",
                phase="stage_setup_complete",
                stage_frame=0,
                state=state,
            )
        ]
        for _, stage_frame in TOOL.EXPECTED_KEYS:
            add_interval(
                state,
                frames=300,
                quads=8,
                runs=2,
                client_runs=1,
                client_quads=4,
            )
            candidate_parts.append(
                fastpath_record(
                    kind="SAMPLE",
                    phase="stage_relative_periodic",
                    stage_frame=stage_frame,
                    state=state,
                    max_run=4,
                    high_water=32,
                )
            )
            candidate_parts.append(sample(stage_frame, 50.0, frame_offset=2000))
        add_interval(state, frames=119, quads=0, runs=0)
        candidate_parts.append(
            fastpath_record(
                kind="MARK",
                phase="stage_teardown_complete",
                stage_frame=TOOL.EXPECTED_TEARDOWN_STAGE_FRAME,
                state=state,
                max_run=4,
                high_water=32,
            )
        )
        result = self.compare(baseline, "".join(candidate_parts))
        self.assertFalse(result["comparison_valid"])
        self.assertTrue(self.rule_failed(result, "fastpath_failures_zero"))
        self.assertEqual(
            result["fastpath_intervals"][0]["client_fallback_submits"], 1
        )


class HardRejectionTests(TemporaryComparison):
    def test_route_must_be_exact_twenty_stage5_windows(self) -> None:
        baseline, candidate = matched_logs()
        baseline = "\n".join(baseline.splitlines()[:-1]) + "\n"
        with self.assertRaises(TOOL.stage_perf.ComparisonError):
            self.compare(baseline, candidate)

    def test_manager_workload_mismatch_is_rejected(self) -> None:
        baseline, candidate = matched_logs()
        candidate = candidate.replace("bullet_exact=20", "bullet_exact=21", 1)
        result = self.compare(baseline, candidate)
        self.assertFalse(result["exact_workload_match"])
        self.assertFalse(result["comparison_valid"])

    def test_draw_vertex_state_upload_matrix_and_vfpu_are_all_exact(self) -> None:
        replacements = (
            ("render_draws_total=800", "render_draws_total=801"),
            ("render_vertices_total=12000", "render_vertices_total=12001"),
            (
                "render_state_requested_total=500",
                "render_state_requested_total=501",
            ),
            ("render_upload_attempt_total=700", "render_upload_attempt_total=701"),
            (
                "render_matrix_recompute_total=20",
                "render_matrix_recompute_total=21",
            ),
            ("render_vfpu_sincos_total=30", "render_vfpu_sincos_total=31"),
        )
        for old, new in replacements:
            with self.subTest(field=old):
                baseline, candidate = matched_logs()
                result = self.compare(baseline, candidate.replace(old, new, 1))
                self.assertFalse(result["exact_workload_match"])
                self.assertFalse(result["comparison_valid"])

    def test_unowned_raw_vertex_delta_has_no_numeric_tolerance(self) -> None:
        baseline, candidate = matched_logs()
        candidate = candidate.replace(
            "render_vertices_total=12000",
            "render_vertices_total=12072",
            1,
        )
        result = self.compare(baseline, candidate)
        self.assertFalse(result["fps_overlay_vertex_reconciliation_match"])
        self.assertTrue(self.rule_failed(result, "fps_overlay_vertex_accounting"))
        self.assertTrue(self.rule_failed(result, "fps_overlay_raw_delta"))
        self.assertFalse(result["comparison_valid"])

    def test_nearby_or_multiple_of_six_overlay_is_not_a_general_allowance(self) -> None:
        baseline, candidate = matched_logs()
        candidate = candidate.replace(
            "render_vertices_total=12000",
            "render_vertices_total=12072",
            1,
        ).replace(
            "render_fps_overlay_vertices_total=0",
            "render_fps_overlay_vertices_total=66",
            1,
        ).replace(
            "render_fps_overlay_vertices_peak=0",
            "render_fps_overlay_vertices_peak=6",
            1,
        )
        result = self.compare(baseline, candidate)
        self.assertTrue(self.rule_failed(result, "fps_overlay_vertex_accounting"))
        self.assertTrue(self.rule_failed(result, "fps_overlay_raw_delta"))
        self.assertFalse(result["exact_workload_match"])

    def test_overlay_cannot_hide_game_vertex_total_or_peak_change(self) -> None:
        for old, new in (
            (
                "render_game_vertices_total=12000",
                "render_game_vertices_total=12001",
            ),
            (
                "render_game_vertices_peak=180",
                "render_game_vertices_peak=181",
            ),
        ):
            with self.subTest(field=old):
                baseline, candidate = matched_logs()
                result = self.compare(baseline, candidate.replace(old, new, 1))
                self.assertTrue(
                    self.rule_failed(result, "fps_overlay_game_vertices_exact")
                )
                self.assertFalse(result["exact_workload_match"])

    def test_missing_owner_counter_is_a_schema_error(self) -> None:
        baseline, candidate = matched_logs()
        candidate = candidate.replace(
            "render_fps_overlay_vertices_total=0 ", "", 1
        )
        with self.assertRaises(TOOL.stage_perf.ComparisonError):
            self.compare(baseline, candidate)

    def test_state_emitted_route_delta_is_not_silently_allowed(self) -> None:
        baseline, candidate = matched_logs()
        candidate = candidate.replace(
            "render_state_emitted_total=450",
            "render_state_emitted_total=459",
            1,
        )
        result = self.compare(baseline, candidate)
        self.assertFalse(result["exact_workload_match"])

    def test_any_fastpath_failure_is_no_go(self) -> None:
        baseline, candidate = matched_logs()
        candidate = replace_occurrence(
            candidate,
            "canonical_fallback_batches=0",
            "canonical_fallback_batches=1",
            2,
        )
        result = self.compare(baseline, candidate)
        self.assertTrue(self.rule_failed(result, "fastpath_failures_zero"))
        self.assertFalse(result["comparison_valid"])

    def test_origin_counters_must_all_be_zero(self) -> None:
        baseline, candidate = matched_logs()
        candidate = replace_occurrence(
            candidate, "owner_fallbacks=0", "owner_fallbacks=1", 1
        )
        result = self.compare(baseline, candidate)
        self.assertTrue(self.rule_failed(result, "fastpath_origin_zero"))

    def test_native_client_submit_algebra_mismatch_is_rejected(self) -> None:
        baseline, candidate = matched_logs()
        candidate = replace_occurrence(
            candidate,
            "native_submitted_quads=8",
            "native_submitted_quads=7",
            1,
        )
        result = self.compare(baseline, candidate)
        self.assertTrue(self.rule_failed(result, "fastpath_submit_accounting"))

    def test_max_run_and_arena_high_water_are_bounded(self) -> None:
        baseline, candidate = matched_logs()
        candidate = replace_occurrence(
            candidate,
            "arena_high_water_vertices=32",
            "arena_high_water_vertices=7000",
            1,
        )
        result = self.compare(baseline, candidate)
        self.assertTrue(self.rule_failed(result, "fastpath_arena_bounds"))

    def test_fastpath_record_must_align_with_general_sample(self) -> None:
        baseline, candidate = matched_logs()
        needle = "extra_flush=0 topology=4v_indexed\nSAMPLE phase="
        candidate = candidate.replace(
            needle,
            "extra_flush=0 topology=4v_indexed\nMARK phase=interposed\n"
            "SAMPLE phase=",
            1,
        )
        result = self.compare(baseline, candidate)
        self.assertTrue(self.rule_failed(result, "fastpath_record_alignment"))

    def test_surface_hash_mismatch_is_no_go(self) -> None:
        baseline, candidate = matched_logs()
        mismatched = self.write_bytes("surface8-bad.bin", b"different")
        result = self.compare(
            baseline,
            candidate,
            surfaces=[
                ("surface0", self.surface0_base, self.surface0_candidate),
                ("surface8", self.surface8_base, mismatched),
            ],
        )
        self.assertFalse(result["surface_identity_match"])
        self.assertFalse(result["comparison_valid"])

    def test_positive_mean_with_nonpositive_ci_lower_bound_is_no_go(self) -> None:
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

    def test_off_control_must_not_contain_product_telemetry(self) -> None:
        baseline, candidate = matched_logs()
        baseline = fastpath_record(
            kind="MARK",
            phase="stage_setup_complete",
            stage_frame=0,
            state=zero_state(),
        ) + baseline
        with self.assertRaises(TOOL.stage_perf.ComparisonError):
            self.compare(baseline, candidate)


class CommandLineTests(TemporaryComparison):
    def test_json_no_go_returns_one(self) -> None:
        baseline, candidate = matched_logs()
        baseline_path = self.write("baseline-cli.log", baseline)
        candidate_path = self.write("candidate-cli.log", candidate)
        completed = subprocess.run(
            [
                sys.executable,
                str(TOOL_PATH),
                str(baseline_path),
                str(candidate_path),
                "--surface-pair",
                "surface0",
                str(self.surface0_base),
                str(self.surface0_candidate),
                "--surface-pair",
                "surface8",
                str(self.surface8_base),
                str(self.surface8_candidate),
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
