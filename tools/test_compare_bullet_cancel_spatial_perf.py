#!/usr/bin/env python3

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from unittest import mock

import bullet_cancel_spatial_contract as contract
import compare_bullet_cancel_spatial_perf as tool


def sample(stage_frame: int, fps: float, frame: int) -> str:
    return (
        "SAMPLE phase=stage_relative_periodic "
        f"frame={frame} stage=5 stage_frame={stage_frame} replay=1 demo=1 "
        f"fps={fps:.3f} render_perf_valid=1 render_frames=300 "
        "enemy_exact=4 bullet_exact=20 laser_exact=1 item_array_exact=10 "
        "bullet_enum_frames=300 bullet_enum_slot_probes=1000 "
        "bullet_enum_word_probes=40 bullet_enum_visited=500 "
        "bullet_enum_fallback_frames=0 "
        "render_vertices_total=12000 render_vertices_peak=180 "
        "render_fps_overlay_vertices_total=0 render_fps_overlay_vertices_peak=0 "
        "render_game_vertices_total=12000 render_game_vertices_peak=180 "
        "render_draws_total=800 render_draws_peak=9\n"
    )


def telemetry(stage_frame: int, frame: int, *, calls: int = 100) -> str:
    return (
        f"{contract.TELEMETRY_PREFIX} kind=SAMPLE phase=stage_relative_periodic "
        f"frame={frame} stage=5 stage_frame={stage_frame} valid=1 mode=product "
        "counter_scope=stage_relative_interval cumulative=0 interval_consumed=1 "
        "owner=game_frame_thread marks_peek=0 conservative_coverage=1 "
        "canonical_slot_order=1 "
        f"calls={calls} indexed_queries=20 rejected_queries=70 fallbacks=10 "
        "rebuilds=1 circles=10 rects=0 full_candidates=1000 "
        "indexed_candidates=200 fallback_candidates=100 exact_tests=150 "
        "false_positives=5 occupancy_owner_fallbacks=10 "
        "unsupported_region_fallbacks=0 nonfinite_fallbacks=0 "
        "duplicate_pairs=2 duplicate_replays=2 "
        "duplicate_exact_tests_saved=2 duplicate_fallbacks=0\n"
    )


class ComparatorContracts(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)

    def tearDown(self) -> None:
        self.temp.cleanup()

    def write_logs(self, *, bad_calls: bool = False) -> tuple[Path, Path, list[tuple[str, Path, Path]]]:
        control = ["FEATURE SC_ONLY=1 BULLET_FASTPATH=1 PLAYER_SCAN_SIDECAR=1 BULLET_CANCEL_SPATIAL=0\n"]
        candidate = ["FEATURE SC_ONLY=1 BULLET_FASTPATH=1 PLAYER_SCAN_SIDECAR=1 BULLET_CANCEL_SPATIAL=1\n"]
        for index, (_, stage_frame) in enumerate(contract.EXPECTED_KEYS):
            control.append(sample(stage_frame, 40.0, 1000 + index))
            candidate.append(telemetry(stage_frame, 2000 + index,
                                       calls=99 if bad_calls and index == 0 else 100))
            candidate.append(sample(stage_frame, 50.0, 2000 + index))
        control_path = self.root / "control.log"
        candidate_path = self.root / "candidate.log"
        control_path.write_text("".join(control), encoding="utf-8")
        candidate_path.write_text("".join(candidate), encoding="utf-8")
        surface_off = self.root / "off.bin"
        surface_on = self.root / "on.bin"
        surface_off.write_bytes(b"identical-surface")
        surface_on.write_bytes(b"identical-surface")
        return control_path, candidate_path, [("stage5", surface_off, surface_on)]

    def test_accepts_reconciled_fixed_route(self) -> None:
        control, candidate, surfaces = self.write_logs()
        with mock.patch.object(contract, "REQUIRED_BOOTSTRAP_ITERATIONS", 100):
            result = tool.compare_logs(
                control, candidate, surface_pairs=surfaces,
                bootstrap_iterations=100,
            )
        self.assertTrue(result["comparison_valid"])
        self.assertTrue(result["acceptance_passed"])

    def test_accepts_real_runtime_split_boot_and_memory_logs(self) -> None:
        control, candidate, surfaces = self.write_logs()
        control_lines = control.read_text(encoding="utf-8").splitlines(True)
        candidate_lines = candidate.read_text(encoding="utf-8").splitlines(True)
        control_boot = self.root / "control-boot.log"
        candidate_boot = self.root / "candidate-boot.log"
        control_boot.write_text(control_lines[0], encoding="utf-8")
        candidate_boot.write_text(candidate_lines[0], encoding="utf-8")
        control.write_text("".join(control_lines[1:]), encoding="utf-8")
        candidate.write_text("".join(candidate_lines[1:]), encoding="utf-8")
        with mock.patch.object(contract, "REQUIRED_BOOTSTRAP_ITERATIONS", 100):
            result = tool.compare_logs(
                control,
                candidate,
                surface_pairs=surfaces,
                control_feature_path=control_boot,
                candidate_feature_path=candidate_boot,
                bootstrap_iterations=100,
            )
        self.assertTrue(result["comparison_valid"])
        self.assertTrue(result["acceptance_passed"])

    def test_rejects_unattributed_calls(self) -> None:
        control, candidate, surfaces = self.write_logs(bad_calls=True)
        result = tool.compare_logs(
            control, candidate, surface_pairs=surfaces,
            bootstrap_iterations=10,
        )
        self.assertFalse(result["comparison_valid"])
        self.assertIn("all_call_attribution", {v["rule"] for v in result["violations"]})

    def test_rejects_surface_difference(self) -> None:
        control, candidate, surfaces = self.write_logs()
        surfaces[0][2].write_bytes(b"different")
        result = tool.compare_logs(
            control, candidate, surface_pairs=surfaces,
            bootstrap_iterations=10,
        )
        self.assertFalse(result["comparison_valid"])
        self.assertIn("surface_identity", {v["rule"] for v in result["violations"]})


if __name__ == "__main__":
    unittest.main()
