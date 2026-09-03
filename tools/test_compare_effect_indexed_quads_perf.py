#!/usr/bin/env python3
"""Host contracts for compare_effect_indexed_quads_perf.py."""

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
TOOL_PATH = TOOLS / "compare_effect_indexed_quads_perf.py"
sys.path.insert(0, str(TOOLS))
SPEC = importlib.util.spec_from_file_location(
    "compare_effect_indexed_quads_perf", TOOL_PATH
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
    stage: int = 5,
    replay: int = 1,
    demo: int = 1,
    draws: int = 800,
    draw_peak: int = 9,
    game_vertices: int = 12_000,
    game_vertex_peak: int = 180,
    overlay_vertices: int = 0,
    overlay_peak: int = 0,
    raw_vertices: int | None = None,
    raw_vertex_peak: int | None = None,
    state_requested: int = 500,
    state_requested_peak: int = 7,
    state_emitted: int = 450,
    state_emitted_peak: int = 6,
    uploads: int = 700,
    upload_peak: int = 9,
    enemy_exact: int = 4,
    effect_drawn: int = 80,
    render_perf_valid: int = 1,
) -> str:
    if raw_vertices is None:
        raw_vertices = game_vertices + overlay_vertices
    if raw_vertex_peak is None:
        raw_vertex_peak = game_vertex_peak
    return (
        "SAMPLE phase=stage_relative_periodic "
        f"frame={stage_frame + frame_offset} stage={stage} stage_frame={stage_frame} "
        f"replay={replay} demo={demo} fps={fps:.3f} "
        "enemy_reported=4/480 enemy_reported_peak=7 enemy_exact="
        f"{enemy_exact} enemy_exact_sampled_peak=7 enemy_hi=8 "
        "bullet_reported=20/1536 bullet_reported_peak=30 bullet_exact=20 "
        "bullet_exact_sampled_peak=30 bullet_hi=40 "
        "bullet_enum_frames=300 bullet_enum_slot_probes=1000 "
        "bullet_enum_word_probes=2000 bullet_enum_visited=500 "
        "bullet_enum_fallback_frames=0 "
        "laser_exact=1/256 laser_exact_sampled_peak=2 laser_hi=3 "
        "item_reported=10/2096 item_reported_peak=14 item_array_exact=10 "
        "item_list_exact=10 item_exact_sampled_peak=14 item_hi=20 "
        f"render_arena_live=999 render_perf_valid={render_perf_valid} "
        "render_frames=300 "
        f"render_draws_total={draws} render_draws_peak={draw_peak} "
        f"render_vertices_total={raw_vertices} "
        f"render_vertices_peak={raw_vertex_peak} "
        f"render_fps_overlay_vertices_total={overlay_vertices} "
        f"render_fps_overlay_vertices_peak={overlay_peak} "
        f"render_game_vertices_total={game_vertices} "
        f"render_game_vertices_peak={game_vertex_peak} "
        f"render_state_requested_total={state_requested} "
        f"render_state_requested_peak={state_requested_peak} "
        f"render_state_emitted_total={state_emitted} "
        f"render_state_emitted_peak={state_emitted_peak} "
        "render_matrix_recompute_total=20 render_matrix_recompute_peak=2 "
        "render_vfpu_sincos_total=30 render_vfpu_sincos_peak=1 "
        "render_effect_active_total=90 render_effect_active_peak=4 "
        f"render_effect_drawn_total={effect_drawn} render_effect_drawn_peak=4 "
        "render_item_drawn_total=60 render_item_drawn_peak=5 "
        f"render_upload_attempt_total={uploads} "
        f"render_upload_attempt_peak={upload_peak} "
        "render_actual_upload_total=3 render_actual_upload_peak=1 "
        "render_upload_bytes_total=4096 render_upload_bytes_peak=4096 "
        "render_text_bytes_total=0 render_text_bytes_peak=0\n"
    )


def effect_record(
    stage_frame: int,
    *,
    frame_offset: int = 2000,
    kind: str = "SAMPLE",
    phase: str = "stage_relative_periodic",
    stage: int = 5,
    valid: int = 1,
    passes: int = 900,
    flushes: int = 2,
    batches: int = 2,
    quads: int = 8,
    canonical: int | None = None,
    indexed: int | None = None,
    saved: int | None = None,
    saved_bytes: int | None = None,
    fallbacks: int = 0,
    fallback_quads: int = 0,
    owner_conflicts: int = 0,
    abandoned_passes: int = 0,
    abandoned_quads: int = 0,
    max_batch: int = 4,
) -> str:
    if canonical is None:
        canonical = quads * 6
    if indexed is None:
        indexed = quads * 4
    if saved is None:
        saved = quads * 2
    if saved_bytes is None:
        saved_bytes = saved * 28
    return (
        f"{TOOL.contract.TELEMETRY_PREFIX} kind={kind} phase={phase} "
        f"frame={stage_frame + frame_offset} stage={stage} "
        f"stage_frame={stage_frame} valid={valid} mode=product "
        "counter_scope=stage_relative_interval cumulative=0 "
        "interval_consumed=1 owner=game_frame_thread marks_peek=0 "
        "ordinary_effect_only=1 radial_trail_excluded=1 "
        "topology=6v_to_4v_indexed render_perf_vertices=logical_6v "
        "vertex_stride_bytes=28 "
        f"passes={passes} flushes={flushes} batches={batches} "
        f"successful_ordinary_quads={quads} "
        f"canonical_input_vertices={canonical} "
        f"indexed_output_vertices={indexed} vertices_saved={saved} "
        f"bytes_saved={saved_bytes} fallbacks={fallbacks} "
        f"fallback_quads={fallback_quads} owner_conflicts={owner_conflicts} "
        f"abandoned_passes={abandoned_passes} "
        f"abandoned_quads={abandoned_quads} max_batch_quads={max_batch}\n"
    )


def matched_logs(
    *, baseline_fps: float = 40.0, candidate_fps: float = 50.0
) -> tuple[str, str]:
    baseline_parts: list[str] = []
    candidate_parts: list[str] = []
    for _, stage_frame in TOOL.contract.EXPECTED_KEYS:
        baseline_parts.append(
            sample(stage_frame, baseline_fps, frame_offset=1000)
        )
        candidate_parts.append(effect_record(stage_frame))
        # Exercise the permitted Begin/End edge-split accounting rather than
        # hiding behind an all-zero physical-boundary delta fixture.
        candidate_parts.append(
            sample(
                stage_frame,
                candidate_fps,
                frame_offset=2000,
                draws=802,
                draw_peak=10,
                state_requested=506,
                state_requested_peak=10,
                state_emitted=462,
                state_emitted_peak=12,
                uploads=702,
                upload_peak=10,
            )
        )
    return "".join(baseline_parts), "".join(candidate_parts)


def mutate_general_sample(
    text: str, stage_frame: int, replacements: dict[str, str]
) -> str:
    lines = text.splitlines(keepends=True)
    for index, line in enumerate(lines):
        if not line.startswith("SAMPLE ") or f"stage_frame={stage_frame} " not in line:
            continue
        for old, new in replacements.items():
            if old not in line:
                raise AssertionError(f"{old!r} absent from SAMPLE {stage_frame}")
            line = line.replace(old, new, 1)
        lines[index] = line
        return "".join(lines)
    raise AssertionError(f"SAMPLE {stage_frame} absent")


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
    def surfaces(self) -> list[tuple[str, Path, Path]]:
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
        surfaces: list[tuple[str, Path, Path]] | None = None,
    ) -> dict:
        return TOOL.compare_logs(
            self.write("baseline.log", baseline),
            self.write("candidate.log", candidate),
            surface_pairs=self.surfaces if surfaces is None else surfaces,
            bootstrap_iterations=iterations,
            bootstrap_seed=123,
        )

    @staticmethod
    def rule_failed(result: dict, name: str) -> bool:
        return not next(
            rule for rule in result["reconciliations"] if rule["rule"] == name
        )["passed"]


class ValidComparisonTests(TemporaryComparison):
    def test_later_stage3_demo_is_excluded_from_fixed_stage5_timing(self) -> None:
        baseline, candidate = matched_logs()
        for stage_frame in TOOL.stage_perf.FIXED_STAGE_SAMPLE_FRAMES:
            baseline += sample(
                stage_frame, 10.0, stage=3, frame_offset=10_000
            )
            candidate += effect_record(
                stage_frame, stage=3, frame_offset=20_000
            )
            candidate += sample(
                stage_frame,
                100.0,
                stage=3,
                frame_offset=20_000,
                draws=802,
                draw_peak=10,
                state_requested=506,
                state_requested_peak=10,
                state_emitted=462,
                state_emitted_peak=12,
                uploads=702,
                upload_peak=10,
            )
        result = self.compare(baseline, candidate)
        self.assertTrue(result["comparison_valid"])
        self.assertEqual(result["sample_count"], 20)
        self.assertEqual({pair["stage"] for pair in result["pairs"]}, {5})
        self.assertAlmostEqual(result["timing"]["paired_improvement_ms_mean"], 5.0)

    def test_twenty_windows_reconcile_logical_6v_and_physical_4v(self) -> None:
        baseline, candidate = matched_logs()
        result = self.compare(baseline, candidate)
        self.assertTrue(result["comparison_valid"])
        self.assertTrue(result["exact_workload_match"])
        self.assertTrue(result["fps_overlay_vertex_reconciliation_match"])
        self.assertTrue(result["effect_reconciliation_match"])
        self.assertTrue(result["surface_identity_match"])
        self.assertEqual(result["sample_count"], 20)
        self.assertEqual(result["effect_intervals"][0]["successful_ordinary_quads"], 8)
        self.assertEqual(result["effect_intervals"][0]["vertices_saved"], 16)
        self.assertEqual(result["effect_intervals"][0]["bytes_saved"], 448)
        self.assertTrue(result["performance_gate_passed"])
        self.assertFalse(result["bootstrap_iterations_valid"])
        self.assertFalse(result["acceptance_passed"])

    def test_owned_overlay_drift_preserves_exact_game_vertices(self) -> None:
        baseline, candidate = matched_logs()
        candidate = mutate_general_sample(
            candidate,
            301,
            {
                "render_vertices_total=12000": "render_vertices_total=12072",
                "render_fps_overlay_vertices_total=0": "render_fps_overlay_vertices_total=72",
                "render_fps_overlay_vertices_peak=0": "render_fps_overlay_vertices_peak=6",
            },
        )
        result = self.compare(baseline, candidate)
        self.assertTrue(result["comparison_valid"])
        first = result["fps_overlay_vertex_reconciliations"][0]
        self.assertTrue(first["match"])
        self.assertEqual(first["raw_total_delta"], 72)
        self.assertEqual(first["overlay_total_delta"], 72)
        self.assertTrue(first["game_vertices_exact"])

    def test_an_unused_window_is_valid_when_global_product_is_exercised(self) -> None:
        baseline, candidate = matched_logs()
        replacements = {
            "flushes=2": "flushes=0",
            "batches=2": "batches=0",
            "successful_ordinary_quads=8": "successful_ordinary_quads=0",
            "canonical_input_vertices=48": "canonical_input_vertices=0",
            "indexed_output_vertices=32": "indexed_output_vertices=0",
            "vertices_saved=16": "vertices_saved=0",
            "bytes_saved=448": "bytes_saved=0",
            "max_batch_quads=4": "max_batch_quads=0",
        }
        for old, new in replacements.items():
            candidate = candidate.replace(old, new, 1)
        result = self.compare(baseline, candidate)
        self.assertTrue(result["comparison_valid"])

    def test_registered_bootstrap_and_positive_ci_are_required_for_go(self) -> None:
        baseline, candidate = matched_logs()
        with mock.patch.object(
            TOOL.stage_perf, "paired_bootstrap_ci", return_value=(4.9, 5.1)
        ):
            result = self.compare(
                baseline,
                candidate,
                iterations=TOOL.contract.REQUIRED_BOOTSTRAP_ITERATIONS,
            )
        self.assertTrue(result["bootstrap_iterations_valid"])
        self.assertTrue(result["performance_gate_passed"])
        self.assertTrue(result["acceptance_passed"])
        self.assertEqual(result["verdict"], "GO")


class HardRejectionTests(TemporaryComparison):
    def test_route_requires_exactly_twenty_fixed_stage5_samples(self) -> None:
        baseline, candidate = matched_logs()
        baseline = "".join(baseline.splitlines(keepends=True)[:-1])
        with self.assertRaises(TOOL.stage_perf.ComparisonError):
            self.compare(baseline, candidate)

    def test_effect_route_requires_exactly_twenty_records(self) -> None:
        baseline, candidate = matched_logs()
        candidate_lines = candidate.splitlines(keepends=True)
        candidate = "".join(candidate_lines[2:])
        with self.assertRaises(TOOL.stage_perf.ComparisonError):
            self.compare(baseline, candidate)

    def test_off_control_cannot_contain_product_telemetry(self) -> None:
        baseline, candidate = matched_logs()
        baseline = effect_record(301, frame_offset=1000) + baseline
        with self.assertRaises(TOOL.stage_perf.ComparisonError):
            self.compare(baseline, candidate)

    def test_effect_record_must_precede_same_frame_sample(self) -> None:
        baseline, candidate = matched_logs()
        candidate = candidate.replace("frame=2301 stage=5", "frame=2302 stage=5", 1)
        result = self.compare(baseline, candidate)
        self.assertTrue(self.rule_failed(result, "effect_record_alignment"))
        self.assertFalse(result["comparison_valid"])

    def test_physical_savings_are_two_vertices_per_successful_quad(self) -> None:
        baseline, candidate = matched_logs()
        candidate = candidate.replace("vertices_saved=16", "vertices_saved=15", 1)
        result = self.compare(baseline, candidate)
        self.assertTrue(self.rule_failed(result, "effect_6v_4v_reconciliation"))
        self.assertFalse(result["comparison_valid"])

    def test_renderperf_game_vertices_remain_logical_6v_not_physical_4v(self) -> None:
        baseline, candidate = matched_logs()
        candidate = mutate_general_sample(
            candidate,
            301,
            {
                "render_vertices_total=12000": "render_vertices_total=11984",
                "render_game_vertices_total=12000": "render_game_vertices_total=11984",
            },
        )
        result = self.compare(baseline, candidate)
        self.assertFalse(result["exact_workload_match"])
        self.assertFalse(result["fps_overlay_vertex_reconciliation_match"])
        self.assertTrue(self.rule_failed(result, "logical_vertex_workload_exact"))
        self.assertFalse(result["comparison_valid"])

    def test_unowned_raw_vertex_delta_is_rejected(self) -> None:
        baseline, candidate = matched_logs()
        candidate = mutate_general_sample(
            candidate,
            301,
            {"render_vertices_total=12000": "render_vertices_total=12006"},
        )
        result = self.compare(baseline, candidate)
        self.assertFalse(result["fps_overlay_vertex_reconciliation_match"])
        self.assertFalse(result["comparison_valid"])

    def test_manager_and_unrelated_render_workload_are_strict(self) -> None:
        baseline, candidate = matched_logs()
        candidate = mutate_general_sample(
            candidate,
            301,
            {
                "enemy_exact=4": "enemy_exact=5",
                "render_effect_drawn_total=80": "render_effect_drawn_total=81",
            },
        )
        result = self.compare(baseline, candidate)
        self.assertFalse(result["exact_workload_match"])
        fields = {item["field"]: item for item in result["workload_differences"]}
        self.assertFalse(fields["enemy_exact"]["match"])
        self.assertFalse(fields["render_effect_drawn_total"]["match"])

    def test_every_failure_counter_is_an_immediate_no_go(self) -> None:
        for field in TOOL.contract.FAILURE_FIELDS:
            with self.subTest(field=field):
                baseline, candidate = matched_logs()
                candidate = candidate.replace(f"{field}=0", f"{field}=1", 1)
                result = self.compare(baseline, candidate)
                self.assertTrue(self.rule_failed(result, "effect_failures_zero"))
                self.assertFalse(result["comparison_valid"])

    def test_negative_counter_is_rejected(self) -> None:
        baseline, candidate = matched_logs()
        candidate = candidate.replace("bytes_saved=448", "bytes_saved=-1", 1)
        result = self.compare(baseline, candidate)
        self.assertTrue(self.rule_failed(result, "effect_counters_nonnegative"))

    def test_batch_high_water_cannot_exceed_index_capacity(self) -> None:
        baseline, candidate = matched_logs()
        candidate = candidate.replace(
            "max_batch_quads=4",
            f"max_batch_quads={TOOL.contract.MAX_BATCH_QUADS + 1}",
            1,
        ).replace(
            "successful_ordinary_quads=8",
            f"successful_ordinary_quads={TOOL.contract.MAX_BATCH_QUADS + 1}",
            1,
        ).replace(
            "canonical_input_vertices=48",
            f"canonical_input_vertices={(TOOL.contract.MAX_BATCH_QUADS + 1) * 6}",
            1,
        ).replace(
            "indexed_output_vertices=32",
            f"indexed_output_vertices={(TOOL.contract.MAX_BATCH_QUADS + 1) * 4}",
            1,
        ).replace(
            "vertices_saved=16",
            f"vertices_saved={(TOOL.contract.MAX_BATCH_QUADS + 1) * 2}",
            1,
        ).replace(
            "bytes_saved=448",
            f"bytes_saved={(TOOL.contract.MAX_BATCH_QUADS + 1) * 2 * 28}",
            1,
        )
        result = self.compare(baseline, candidate)
        self.assertTrue(self.rule_failed(result, "effect_success_shape"))

    def test_boundary_total_and_peak_holes_are_closed(self) -> None:
        cases = (
            (
                {"render_draws_total=802": "render_draws_total=2601"},
                "effect_boundary_draw_bound",
            ),
            (
                {"render_state_requested_peak=10": "render_state_requested_peak=30"},
                "effect_boundary_frontend_accounting",
            ),
            (
                {"render_upload_attempt_peak=10": "render_upload_attempt_peak=20"},
                "effect_boundary_frontend_accounting",
            ),
            (
                {"render_state_emitted_peak=12": "render_state_emitted_peak=100"},
                "effect_backend_state_bound",
            ),
        )
        for replacements, rule in cases:
            with self.subTest(rule=rule, replacements=replacements):
                baseline, candidate = matched_logs()
                candidate = mutate_general_sample(candidate, 301, replacements)
                result = self.compare(baseline, candidate)
                self.assertTrue(self.rule_failed(result, rule))
                self.assertFalse(result["comparison_valid"])

    def test_metadata_and_required_fields_are_fail_closed(self) -> None:
        baseline, candidate = matched_logs()
        bad_metadata = candidate.replace("vertex_stride_bytes=28", "vertex_stride_bytes=24", 1)
        result = self.compare(baseline, bad_metadata)
        self.assertTrue(self.rule_failed(result, "effect_metadata"))

        missing = candidate.replace(" vertices_saved=16", "", 1)
        with self.assertRaises(TOOL.stage_perf.ComparisonError):
            self.compare(baseline, missing)

    def test_surface_pairs_are_mandatory_unique_and_byte_exact(self) -> None:
        baseline, candidate = matched_logs()
        with self.assertRaises(TOOL.stage_perf.ComparisonError):
            self.compare(baseline, candidate, surfaces=[])
        with self.assertRaises(TOOL.stage_perf.ComparisonError):
            self.compare(
                baseline,
                candidate,
                surfaces=[
                    ("same", self.surface0_base, self.surface0_candidate),
                    ("same", self.surface8_base, self.surface8_candidate),
                ],
            )
        bad = self.write_bytes("surface8-bad.bin", b"different")
        result = self.compare(
            baseline,
            candidate,
            surfaces=[
                ("surface0", self.surface0_base, self.surface0_candidate),
                ("surface8", self.surface8_base, bad),
            ],
        )
        self.assertFalse(result["surface_identity_match"])
        self.assertTrue(self.rule_failed(result, "surface_artifact_identity"))
        self.assertNotEqual(
            result["surfaces"][1]["baseline_sha256"],
            result["surfaces"][1]["candidate_sha256"],
        )

    def test_positive_mean_with_nonpositive_ci_is_no_go(self) -> None:
        baseline, candidate = matched_logs()
        with mock.patch.object(
            TOOL.stage_perf, "paired_bootstrap_ci", return_value=(-0.01, 0.10)
        ):
            result = self.compare(
                baseline,
                candidate,
                iterations=TOOL.contract.REQUIRED_BOOTSTRAP_ITERATIONS,
            )
        self.assertTrue(result["comparison_valid"])
        self.assertTrue(result["bootstrap_iterations_valid"])
        self.assertFalse(result["performance_gate_passed"])
        self.assertFalse(result["acceptance_passed"])


class CommandLineTests(TemporaryComparison):
    def test_json_low_iteration_run_is_valid_but_returns_no_go(self) -> None:
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
        self.assertTrue(result["performance_gate_passed"])
        self.assertFalse(result["bootstrap_iterations_valid"])
        self.assertFalse(result["acceptance_passed"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
