#!/usr/bin/env python3
"""Host tests for compare_sprite_topology_perf.py."""

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
TOOL_PATH = TOOLS / "compare_sprite_topology_perf.py"
sys.path.insert(0, str(TOOLS))
SPEC = importlib.util.spec_from_file_location("compare_sprite_topology_perf", TOOL_PATH)
assert SPEC is not None and SPEC.loader is not None
TOOL = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = TOOL
SPEC.loader.exec_module(TOOL)


def sample(
    stage_frame: int,
    fps: float,
    *,
    vertices: int = 12_000,
    vertices_peak: int = 180,
    draws: int = 800,
    state_requested: int = 500,
    state_emitted: int = 4_000,
    upload_attempts: int = 700,
    item_drawn: int = 60,
) -> str:
    return (
        "SAMPLE phase=stage_relative_periodic "
        f"frame={stage_frame + 1000} stage=5 stage_frame={stage_frame} "
        f"replay=1 demo=1 fps={fps:.3f} "
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
        f"render_state_requested_total={state_requested} render_state_requested_peak=7 "
        f"render_state_emitted_total={state_emitted} render_state_emitted_peak=45 "
        "render_matrix_recompute_total=20 render_matrix_recompute_peak=2 "
        "render_vfpu_sincos_total=30 render_vfpu_sincos_peak=1 "
        "render_effect_active_total=90 render_effect_active_peak=4 "
        "render_effect_drawn_total=80 render_effect_drawn_peak=4 "
        f"render_item_drawn_total={item_drawn} render_item_drawn_peak=5 "
        f"render_upload_attempt_total={upload_attempts} render_upload_attempt_peak=9 "
        "render_actual_upload_total=3 render_actual_upload_peak=1 "
        "render_upload_bytes_total=4096 render_upload_bytes_peak=4096\n"
    )


def item_record(
    stage_frame: int,
    *,
    kind: str = "SAMPLE",
    phase: str = "stage_relative_periodic",
    multiplier: int = 1,
    backend_fallbacks: int = 0,
) -> str:
    passes = 300 * multiplier
    candidates = 12 * multiplier
    visible = 10 * multiplier
    culled = 2 * multiplier
    pairs = 10 * multiplier
    runs = 2 * multiplier
    saved = 40 * multiplier
    return (
        f"ITEM_TIME_DRAW_PAIR kind={kind} phase={phase} mode=product "
        f"frame={stage_frame + 1000} stage=5 stage_frame={stage_frame} "
        f"passes={passes} candidates={candidates} canonical_draws=0 "
        f"visible={visible} culled={culled} eligible_pairs={pairs} "
        f"compatible_runs={runs} submitted_runs={runs} submitted_pairs={pairs} "
        "endpoint_matches=0 endpoint_mismatches=0 canonical_fallbacks=0 "
        "owner_fallbacks=0 load_fallbacks=0 script_fallbacks=0 "
        "sprite_fallbacks=0 visibility_fallbacks=0 rotation_fallbacks=0 "
        "scale_fallbacks=0 nonfinite_fallbacks=0 texture_fallbacks=0 "
        "state_fallbacks=0 axis_fallbacks=0 endpoint_fallbacks=0 "
        f"capacity_fallbacks=0 backend_fallbacks={backend_fallbacks} "
        f"vertices_saved={saved} frontend_bytes_saved={saved * 28} "
        f"backend_bytes_saved={saved * 24} max_run=5 peak_candidates=3 "
        "peak_visible=3 peak_eligible=3 peak_runs=1 peak_stage_frame=100 "
        f"cache_hits={candidates - 1} cache_revalidations=1 "
        "cache_generation_changes=0 cache_validation_failures=0 "
        "semantic_hash=811c9dc5\n"
    )


def item_origin() -> str:
    values = {field: 0 for field in TOOL.ITEM_ADDITIVE_FIELDS}
    fields = " ".join(f"{field}={values[field]}" for field in sorted(values))
    return (
        "ITEM_TIME_DRAW_PAIR kind=MARK phase=stage_pool_arena_ready mode=product "
        f"frame=900 stage=5 stage_frame=0 {fields}\n"
    )


def bullet_record(stage_frame: int, *, fail_closed: int = 0) -> str:
    return (
        "BULLET_MIXED_QUADS kind=SAMPLE phase=stage_relative_periodic mode=product "
        f"frame={stage_frame + 1000} stage=5 stage_frame={stage_frame} "
        "passes=300 owner_conflict_passes=0 state_runs=2 batches=2 "
        "candidates=8 eligible_prefix_quads=5 general_quads=3 "
        "sticky_general_quads=1 nonfinite_fallbacks=0 axis_fallbacks=2 "
        "area_or_mirror_fallbacks=0 z_or_w_fallbacks=0 uv_fallbacks=0 "
        "diffuse_fallbacks=0 submitted_batches=2 submitted_pair_quads=5 "
        "submitted_general_quads=3 backend_fallback_batches=0 "
        f"fail_closed_batches={fail_closed} missing_run_batches=0 "
        "invalid_range_batches=0 canonical_recovery_draw_failures=0 "
        "canonical_recovery_quads=0 potential_frontend_vertices_saved=10 "
        "potential_ge_vertices_saved=20 submitted_frontend_vertices_saved=10 "
        "submitted_ge_vertices_saved=20 max_pair_prefix=4 max_general_suffix=2\n"
    )


class TemporaryLogs(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.directory = Path(self.temporary.name)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def write(self, name: str, text: str | bytes) -> Path:
        path = self.directory / name
        if isinstance(text, bytes):
            path.write_bytes(text)
        else:
            path.write_text(text, encoding="utf-8")
        return path

    def compare(
        self,
        baseline: str,
        candidate: str,
        *mechanisms: str,
        identity=None,
        windows=None,
    ):
        return TOOL.compare_logs(
            self.write("baseline.log", baseline),
            self.write("candidate.log", candidate),
            mechanisms=frozenset(mechanisms),
            bootstrap_iterations=50,
            bootstrap_seed=123,
            identity_pairs=identity or [],
            windows=frozenset(windows) if windows else None,
        )


class ValidComparisonTests(TemporaryLogs):
    def test_item_product_exactly_reconciles_6v_to_2v(self) -> None:
        baseline = sample(301, 40.0)
        candidate = item_origin() + sample(
            301,
            50.0,
            vertices=11_960,
            vertices_peak=160,
            draws=802,
            state_requested=506,
            state_emitted=4_010,
            upload_attempts=702,
        ) + item_record(301)
        result = self.compare(baseline, candidate, "item")
        self.assertTrue(result["comparison_valid"])
        self.assertFalse(result["general_strict_workload_match"])
        self.assertTrue(result["logical_workload_match"])
        self.assertEqual(result["vertex_reconciliations"][0]["observed_vertices_saved"], 40)
        self.assertEqual(result["mechanism_submitted_2v_quads"]["item"], 10)

    def test_bullet_product_exactly_reconciles_4v_indexed_to_mixed(self) -> None:
        baseline = sample(301, 40.0)
        candidate = sample(301, 50.0, vertices=11_980, vertices_peak=170) + bullet_record(301)
        result = self.compare(baseline, candidate, "bullet")
        self.assertTrue(result["comparison_valid"])
        self.assertEqual(result["vertex_reconciliations"][0]["expected_vertices_saved"], 20)
        self.assertEqual(result["mechanism_submitted_2v_quads"]["bullet"], 5)

    def test_combined_savings_are_additive(self) -> None:
        baseline = sample(301, 40.0)
        candidate = (
            item_origin()
            + sample(301, 50.0, vertices=11_940, vertices_peak=150)
            + item_record(301)
            + bullet_record(301)
        )
        result = self.compare(baseline, candidate, "item", "bullet")
        self.assertTrue(result["comparison_valid"])
        self.assertEqual(result["vertex_reconciliations"][0]["expected_vertices_saved"], 60)

    def test_item_cumulative_telemetry_is_differenced_per_window(self) -> None:
        baseline = sample(301, 40.0) + sample(601, 40.0)
        candidate = (
            item_origin()
            + sample(301, 50.0, vertices=11_960, vertices_peak=160)
            + item_record(301, multiplier=1)
            + sample(601, 50.0, vertices=11_960, vertices_peak=160)
            + item_record(601, multiplier=2)
        )
        result = self.compare(baseline, candidate, "item")
        self.assertTrue(result["comparison_valid"])
        self.assertEqual(
            [item["expected_vertices_saved"] for item in result["vertex_reconciliations"]],
            [40, 40],
        )

    def test_explicit_window_uses_preceding_cumulative_item_sample(self) -> None:
        baseline = sample(301, 40.0, vertices=12_072) + sample(601, 40.0)
        candidate = (
            item_origin()
            + sample(301, 50.0, vertices=12_000)
            + item_record(301, multiplier=1)
            + sample(601, 50.0, vertices=11_960, vertices_peak=160)
            + item_record(601, multiplier=2)
        )
        result = self.compare(
            baseline,
            candidate,
            "item",
            windows={(5, 601)},
        )
        self.assertTrue(result["comparison_valid"])
        self.assertEqual(result["sample_count"], 1)
        self.assertEqual(result["selected_windows"], [{"stage": 5, "stage_frame": 601}])
        self.assertEqual(result["vertex_reconciliations"][0]["expected_vertices_saved"], 40)


class RejectionTests(TemporaryLogs):
    def valid_item_candidate(self, **sample_changes: int) -> str:
        return item_origin() + sample(
            301, 50.0, vertices=11_960, vertices_peak=160, **sample_changes
        ) + item_record(301)

    def test_unrelated_logical_render_change_is_rejected(self) -> None:
        result = self.compare(
            sample(301, 40.0),
            self.valid_item_candidate(item_drawn=61),
            "item",
        )
        self.assertFalse(result["comparison_valid"])
        self.assertFalse(result["logical_workload_match"])

    def test_wrong_physical_vertex_delta_is_rejected(self) -> None:
        candidate = item_origin() + sample(
            301, 50.0, vertices=11_961, vertices_peak=160
        ) + item_record(301)
        result = self.compare(sample(301, 40.0), candidate, "item")
        self.assertFalse(result["comparison_valid"])
        rule = next(r for r in result["reconciliations"] if r["rule"] == "submitted_vertex_delta_exact")
        self.assertFalse(rule["passed"])

    def test_unexplained_draw_state_upload_delta_is_rejected(self) -> None:
        candidate = item_origin() + sample(
            301,
            50.0,
            vertices=11_960,
            vertices_peak=160,
            draws=805,
            state_requested=515,
            state_emitted=4_020,
            upload_attempts=705,
        ) + item_record(301)
        result = self.compare(sample(301, 40.0), candidate, "item")
        self.assertFalse(result["comparison_valid"])
        rule = next(r for r in result["reconciliations"] if r["rule"] == "draw_split_bound")
        self.assertFalse(rule["passed"])

    def test_item_backend_fallback_is_rejected(self) -> None:
        candidate = item_origin() + sample(
            301, 50.0, vertices=11_960, vertices_peak=160
        ) + item_record(301, backend_fallbacks=1)
        result = self.compare(sample(301, 40.0), candidate, "item")
        self.assertFalse(result["comparison_valid"])
        rule = next(r for r in result["reconciliations"] if r["rule"] == "item_failures_zero")
        self.assertFalse(rule["passed"])

    def test_bullet_fail_close_is_rejected(self) -> None:
        candidate = sample(301, 50.0, vertices=11_980, vertices_peak=170) + bullet_record(
            301, fail_closed=1
        )
        result = self.compare(sample(301, 40.0), candidate, "bullet")
        self.assertFalse(result["comparison_valid"])

    def test_missing_selected_diagnostics_is_an_input_error(self) -> None:
        with self.assertRaises(TOOL.stage_perf.ComparisonError):
            self.compare(sample(301, 40.0), sample(301, 50.0), "item")

    def test_unknown_explicit_window_is_an_input_error(self) -> None:
        with self.assertRaises(TOOL.stage_perf.ComparisonError):
            self.compare(
                sample(301, 40.0),
                self.valid_item_candidate(),
                "item",
                windows={(5, 601)},
            )

    def test_unselected_mechanism_is_an_input_error(self) -> None:
        candidate = self.valid_item_candidate() + bullet_record(301)
        with self.assertRaises(TOOL.stage_perf.ComparisonError):
            self.compare(sample(301, 40.0), candidate, "item")

    def test_control_diagnostics_are_an_input_error(self) -> None:
        baseline = sample(301, 40.0) + bullet_record(301)
        candidate = sample(301, 50.0, vertices=11_980, vertices_peak=170) + bullet_record(301)
        with self.assertRaises(TOOL.stage_perf.ComparisonError):
            self.compare(baseline, candidate, "bullet")

    def test_identity_mismatch_is_rejected(self) -> None:
        baseline_artifact = self.write("base.bin", b"pixels-a")
        candidate_artifact = self.write("candidate.bin", b"pixels-b")
        baseline = sample(301, 40.0)
        candidate = self.valid_item_candidate()
        result = self.compare(
            baseline,
            candidate,
            "item",
            identity=[("surface0", baseline_artifact, candidate_artifact)],
        )
        self.assertFalse(result["comparison_valid"])
        self.assertFalse(result["artifact_identity_match"])


class CommandLineTests(TemporaryLogs):
    def test_valid_json_returns_zero(self) -> None:
        baseline = self.write("baseline.log", sample(301, 40.0))
        candidate = self.write(
            "candidate.log",
            item_origin()
            + sample(301, 50.0, vertices=11_960, vertices_peak=160)
            + item_record(301),
        )
        completed = subprocess.run(
            [
                sys.executable,
                str(TOOL_PATH),
                str(baseline),
                str(candidate),
                "--mechanism",
                "item",
                "--bootstrap-iterations",
                "20",
                "--json",
            ],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        result = json.loads(completed.stdout)
        self.assertEqual(result["schema"], TOOL.SCHEMA)
        self.assertTrue(result["comparison_valid"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
