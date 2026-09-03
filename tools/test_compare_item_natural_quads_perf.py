#!/usr/bin/env python3
"""Host contracts for compare_item_natural_quads_perf.py."""

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
TOOL_PATH = TOOLS / "compare_item_natural_quads_perf.py"
sys.path.insert(0, str(TOOLS))
SPEC = importlib.util.spec_from_file_location(
    "compare_item_natural_quads_perf", TOOL_PATH
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
    replay: int = 1,
    demo: int = 1,
    draws: int = 800,
    vertices: int = 12_000,
    state_requested: int = 500,
    state_emitted: int = 450,
    uploads: int = 700,
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
        f"render_vertices_total={vertices} render_vertices_peak=180 "
        f"render_state_requested_total={state_requested} "
        "render_state_requested_peak=7 "
        f"render_state_emitted_total={state_emitted} "
        "render_state_emitted_peak=6 "
        "render_matrix_recompute_total=20 render_matrix_recompute_peak=2 "
        "render_vfpu_sincos_total=30 render_vfpu_sincos_peak=1 "
        "render_effect_active_total=90 render_effect_active_peak=4 "
        "render_effect_drawn_total=80 render_effect_drawn_peak=4 "
        "render_item_drawn_total=60 render_item_drawn_peak=5 "
        f"render_upload_attempt_total={uploads} render_upload_attempt_peak=9 "
        "render_actual_upload_total=3 render_actual_upload_peak=1 "
        "render_upload_bytes_total=4096 render_upload_bytes_peak=4096 "
        "render_text_bytes_total=0 render_text_bytes_peak=0\n"
    )


def natural_record(
    stage_frame: int,
    *,
    kind: str = "SAMPLE",
    phase: str = "stage_relative_periodic",
    frame_offset: int = 2000,
    passes: int = 300,
    canonical_batches: int = 2,
    candidates: int = 10,
    visible: int = 5,
    trigger_batches: int = 2,
    coalesced: int = 3,
    fallback_batches: int = 0,
    extra_split_batches: int = 0,
    extra_flushes: int = 0,
    abandoned_batches: int = 0,
    abandoned_quads: int = 0,
    begin_end_added: int = 0,
    client_submits: int | None = None,
    client_quads: int | None = None,
    native_fallbacks: int = 0,
) -> str:
    culled = candidates - visible
    trigger_quads = visible
    eligible = trigger_quads + coalesced
    max_batch = 4 if eligible else 0
    if client_submits is None:
        client_submits = trigger_batches
    if client_quads is None:
        client_quads = eligible
    native_submits = 0
    native_quads = 0
    return (
        f"{TOOL.contract.TELEMETRY_PREFIX} kind={kind} phase={phase} "
        f"mode={TOOL.contract.MODE} frame={stage_frame + frame_offset} "
        f"stage=5 stage_frame={stage_frame} "
        "counter_scope=stage_relative_interval cumulative=0 existing_flush=1 "
        f"begin_end_added={begin_end_added} topology=6v_to_4v_indexed "
        f"passes={passes} canonical_batches={canonical_batches} "
        f"item_time_candidates={candidates} visible_item_time={visible} "
        f"culled_item_time={culled} trigger_batches={trigger_batches} "
        f"trigger_quads={trigger_quads} coalesced_quads={coalesced} "
        f"eligible_quads={eligible} submitted_batches={trigger_batches} "
        f"submitted_quads={eligible} native_submits={native_submits} "
        f"native_submitted_quads={native_quads} "
        f"client_fallback_submits={client_submits} "
        f"client_fallback_quads={client_quads} "
        f"canonical_input_vertices={eligible * 6} "
        f"packed_output_vertices={eligible * 4} "
        f"duplicate_vertices_avoided={eligible * 2} "
        f"fallback_batches={fallback_batches} "
        "pointer_fallbacks=0 span_fallbacks=0 capacity_fallbacks=0 "
        "topology_fallbacks=0 state_fallbacks=0 extra_topology_batches=0 "
        "index_fallbacks=0 "
        f"native_fallbacks={native_fallbacks} "
        f"topology_checks={trigger_batches} topology_checked_quads={eligible} "
        f"extra_split_batches={extra_split_batches} extra_flushes={extra_flushes} "
        f"abandoned_batches={abandoned_batches} abandoned_quads={abandoned_quads} "
        f"max_batch_quads={max_batch}\n"
    )


def matched_logs(
    *, baseline_fps: float = 40.0, candidate_fps: float = 50.0
) -> tuple[str, str]:
    baseline_parts: list[str] = []
    candidate_parts: list[str] = []
    for _, stage_frame in TOOL.contract.EXPECTED_KEYS:
        baseline_parts.append(sample(stage_frame, baseline_fps, frame_offset=1000))
        candidate_parts.append(natural_record(stage_frame))
        candidate_parts.append(sample(stage_frame, candidate_fps, frame_offset=2000))
    candidate_parts.append(
        natural_record(
            TOOL.contract.EXPECTED_TEARDOWN_STAGE_FRAME,
            kind="MARK",
            phase="stage_teardown_complete",
            passes=119,
            canonical_batches=0,
            candidates=0,
            visible=0,
            trigger_batches=0,
            coalesced=0,
        )
    )
    return "".join(baseline_parts), "".join(candidate_parts)


def mutate_general_sample(text: str, stage_frame: int, replacements: dict[str, str]) -> str:
    lines = text.splitlines(keepends=True)
    found = False
    for index, line in enumerate(lines):
        if not line.startswith("SAMPLE ") or f"stage_frame={stage_frame} " not in line:
            continue
        found = True
        for old, new in replacements.items():
            if old not in line:
                raise AssertionError(f"{old!r} absent from sample {stage_frame}")
            line = line.replace(old, new, 1)
        lines[index] = line
        break
    if not found:
        raise AssertionError(f"general sample {stage_frame} not found")
    return "".join(lines)


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
            rule for rule in result["reconciliations"] if rule["rule"] == name
        )["passed"]


class ValidComparisonTests(TemporaryComparison):
    def test_later_stage3_demo_does_not_dilute_stage5_timing(self) -> None:
        baseline, candidate = matched_logs()
        for _, stage_frame in TOOL.contract.EXPECTED_KEYS:
            baseline += sample(
                stage_frame, 10.0, frame_offset=10_000
            ).replace(" stage=5 ", " stage=3 ", 1)
            candidate += sample(
                stage_frame, 100.0, frame_offset=20_000
            ).replace(" stage=5 ", " stage=3 ", 1)
        result = self.compare(baseline, candidate)
        self.assertTrue(result["comparison_valid"])
        self.assertEqual(result["sample_count"], 20)
        self.assertEqual({pair["stage"] for pair in result["pairs"]}, {5})
        self.assertAlmostEqual(result["timing"]["paired_improvement_ms_mean"], 5.0)

    def test_exact_boundaries_whole_batch_algebra_and_surfaces(self) -> None:
        baseline, candidate = matched_logs()
        result = self.compare(baseline, candidate)
        self.assertTrue(result["comparison_valid"])
        self.assertTrue(result["exact_workload_match"])
        self.assertTrue(result["natural_reconciliation_match"])
        self.assertTrue(result["surface_identity_match"])
        self.assertEqual(result["sample_count"], 20)
        self.assertEqual(result["natural_intervals"][0]["trigger_quads"], 5)
        self.assertEqual(result["natural_intervals"][0]["submitted_quads"], 8)
        self.assertTrue(result["performance_gate_passed"])
        self.assertFalse(result["bootstrap_iterations_valid"])
        self.assertFalse(result["acceptance_passed"])

    def test_one_million_positive_ci_is_go(self) -> None:
        baseline, candidate = matched_logs()
        with mock.patch.object(
            TOOL.stage_perf, "paired_bootstrap_ci", return_value=(4.9, 5.1)
        ):
            result = self.compare(
                baseline,
                candidate,
                iterations=TOOL.contract.REQUIRED_BOOTSTRAP_ITERATIONS,
            )
        self.assertTrue(result["acceptance_passed"])
        self.assertEqual(result["verdict"], "GO")

    def test_public_client_route_owns_every_existing_batch(self) -> None:
        baseline, candidate = matched_logs()
        result = self.compare(baseline, candidate)
        self.assertTrue(result["comparison_valid"])
        self.assertEqual(
            result["natural_intervals"][0]["client_fallback_submits"], 2
        )
        self.assertEqual(result["natural_intervals"][0]["client_fallback_quads"], 8)
        self.assertEqual(result["natural_intervals"][0]["native_submits"], 0)
        self.assertEqual(
            result["natural_intervals"][0]["native_submitted_quads"], 0
        )


class HardRejectionTests(TemporaryComparison):
    def test_route_must_be_exact_stage5_twenty_windows(self) -> None:
        baseline, candidate = matched_logs()
        baseline = "\n".join(baseline.splitlines()[:-1]) + "\n"
        with self.assertRaises(TOOL.stage_perf.ComparisonError):
            self.compare(baseline, candidate)

    def test_candidate_visible_culled_quad_algebra_is_strict(self) -> None:
        baseline, candidate = matched_logs()
        candidate = candidate.replace("eligible_quads=8", "eligible_quads=7", 1)
        result = self.compare(baseline, candidate)
        self.assertTrue(self.rule_failed(result, "natural_whole_batch_accounting"))

    def test_fallback_split_flush_and_abandon_are_each_no_go(self) -> None:
        fields = (
            "fallback_batches",
            "pointer_fallbacks",
            "span_fallbacks",
            "capacity_fallbacks",
            "topology_fallbacks",
            "state_fallbacks",
            "extra_topology_batches",
            "index_fallbacks",
            "extra_split_batches",
            "extra_flushes",
            "abandoned_batches",
            "abandoned_quads",
        )
        for field in fields:
            with self.subTest(field=field):
                baseline, candidate = matched_logs()
                candidate = candidate.replace(f"{field}=0", f"{field}=1", 1)
                result = self.compare(baseline, candidate)
                self.assertTrue(self.rule_failed(result, "natural_failures_zero"))

    def test_specific_unsafe_fallback_must_reconcile_and_is_still_no_go(self) -> None:
        baseline, candidate = matched_logs()
        candidate = candidate.replace("fallback_batches=0", "fallback_batches=1", 1)
        candidate = candidate.replace("span_fallbacks=0", "span_fallbacks=1", 1)
        result = self.compare(baseline, candidate)
        self.assertTrue(self.rule_failed(result, "natural_failures_zero"))
        # The reason accounting itself is sound; the nonzero unsafe path is
        # nevertheless an unconditional rejection.
        self.assertFalse(self.rule_failed(result, "natural_backend_partition"))

    def test_every_native_route_counter_is_forbidden(self) -> None:
        for field in (
            "native_submits",
            "native_submitted_quads",
            "native_fallbacks",
        ):
            with self.subTest(field=field):
                baseline, candidate = matched_logs()
                candidate = candidate.replace(f"{field}=0", f"{field}=1", 1)
                result = self.compare(baseline, candidate)
                self.assertTrue(
                    self.rule_failed(result, "natural_public_client_only")
                )

    def test_begin_end_contract_marker_must_remain_zero(self) -> None:
        baseline, candidate = matched_logs()
        candidate = candidate.replace("begin_end_added=0", "begin_end_added=1", 1)
        result = self.compare(baseline, candidate)
        self.assertTrue(self.rule_failed(result, "natural_metadata"))

    def test_rendered_index_vertices_and_manager_workload_are_exact(self) -> None:
        baseline, candidate = matched_logs()
        candidate = mutate_general_sample(
            candidate,
            301,
            {
                "render_vertices_total=12000": "render_vertices_total=11998",
                "item_array_exact=10": "item_array_exact=9",
            },
        )
        result = self.compare(baseline, candidate)
        self.assertFalse(result["exact_workload_match"])
        self.assertFalse(result["comparison_valid"])

    def test_r052_r053_known_extra_flush_signature_is_explicitly_rejected(self) -> None:
        baseline, candidate = matched_logs()
        signatures = {
            2401: (5, 15),
            2701: (3, 9),
            3001: (21, 63),
        }
        for stage_frame, (extra_draws, extra_states) in signatures.items():
            candidate = mutate_general_sample(
                candidate,
                stage_frame,
                {
                    "render_draws_total=800": f"render_draws_total={800 + extra_draws}",
                    "render_upload_attempt_total=700": (
                        f"render_upload_attempt_total={700 + extra_draws}"
                    ),
                    "render_state_requested_total=500": (
                        f"render_state_requested_total={500 + extra_states}"
                    ),
                },
            )
        result = self.compare(baseline, candidate)
        self.assertTrue(
            self.rule_failed(result, "natural_existing_boundary_exact")
        )
        rule = next(
            rule
            for rule in result["reconciliations"]
            if rule["rule"] == "natural_existing_boundary_exact"
        )
        self.assertEqual(rule["violation_count"], 3)
        self.assertFalse(result["comparison_valid"])

    def test_surface_mismatch_is_no_go(self) -> None:
        baseline, candidate = matched_logs()
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
        self.assertFalse(result["comparison_valid"])

    def test_positive_mean_but_nonpositive_ci_lower_is_no_go(self) -> None:
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
        self.assertFalse(result["performance_gate_passed"])
        self.assertFalse(result["acceptance_passed"])

    def test_off_control_cannot_contain_natural_product_telemetry(self) -> None:
        baseline, candidate = matched_logs()
        baseline = natural_record(0, kind="MARK", phase="setup") + baseline
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


if __name__ == "__main__":
    unittest.main(verbosity=2)
