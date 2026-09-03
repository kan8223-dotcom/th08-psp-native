#!/usr/bin/env python3
"""Host differential/boundary and integration contracts for the PSP gate."""

from __future__ import annotations

import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class BulletCancelSpatialContracts(unittest.TestCase):
    def test_randomized_conservative_grid_harness(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "bullet_cancel_spatial"
            subprocess.run(
                [
                    "g++", "-std=c++17", "-O2", "-Wall", "-Wextra",
                    "-Werror", "-Isrc",
                    "tools/psp_bullet_cancel_spatial_harness.cpp",
                    "-o", str(output),
                ],
                cwd=ROOT,
                check=True,
            )
            result = subprocess.run(
                [str(output)], cwd=ROOT, check=True,
                text=True, capture_output=True,
            )
            self.assertIn("PASS psp_bullet_cancel_spatial", result.stdout)

    def test_default_off_stamp_and_dependencies(self) -> None:
        makefile = (ROOT / "Makefile.psp").read_text(encoding="utf-8")
        self.assertIn("TH08_PSP_BULLET_CANCEL_SPATIAL ?= 0", makefile)
        self.assertIn("bullet-cancel-spatial-$(TH08_PSP_BULLET_CANCEL_SPATIAL).stamp", makefile)
        self.assertIn("requires TH08_PSP_PLAYER_SCAN_SIDECAR=1", makefile)
        self.assertIn("requires TH08_PSP_BULLET_FASTPATH=1", makefile)
        self.assertIn("$(BULLET_CANCEL_SPATIAL_CONFIG_STAMP)", makefile)

        player = (ROOT / "src/Player.cpp").read_text(encoding="utf-8")
        # Merely declaring an OFF-only static is insufficient: GCC can omit
        # it before linking.  Both candidate-only reservations must therefore
        # be forced into the OFF image as well.
        self.assertRegex(
            player,
            r"g_PspBulletCancelDuplicateCache\s+__attribute__\(\(used\)\)",
        )
        self.assertRegex(
            player,
            r"g_PspBulletCancelSpatialState\s+__attribute__\(\(used\)\)",
        )

    def test_general_hot_path_and_fail_closed_contract(self) -> None:
        player = (ROOT / "src/Player.cpp").read_text(encoding="utf-8")
        bullet = (ROOT / "src/BulletManager.cpp").read_text(encoding="utf-8")
        check_start = player.index("i32 Player::CheckBulletCancelCollision")
        check_end = player.index("// FUNCTION: th08 0x44a230", check_start)
        check = player[check_start:check_end]
        self.assertIn("PspBulletCancelSpatialValidatePosition(position)", check)
        self.assertIn("coverage.Query", check)
        self.assertIn("return 0;", check)
        self.assertIn("PspNextScanBit", check)
        self.assertIn("++spatialExactTests", check)
        self.assertIn("PspBulletCancelSpatialNoteQuery", check)
        self.assertNotIn("PspPrepareBulletCancelSpatial", bullet)

    def test_duplicate_second_call_replays_only_original_side_effects(self) -> None:
        player = (ROOT / "src/Player.cpp").read_text(encoding="utf-8")
        bullet = (ROOT / "src/BulletManager.cpp").read_text(encoding="utf-8")
        self.assertIn("PspArmNextBulletCancelDuplicateCollision();", bullet)
        self.assertIn("PspReplayLastBulletCancelCollision", bullet)
        self.assertIn("player->bulletCancelItemType = slot->collisionValue;", player)
        self.assertIn("slot->hitAccumulator++;", player)
        self.assertIn("cache.position == position && cache.size == size", player)

    def test_telemetry_schema_and_boot_fingerprint(self) -> None:
        telemetry = (ROOT / "psp/memory_telemetry.cpp").read_text(encoding="utf-8")
        main = (ROOT / "psp/main.cpp").read_text(encoding="utf-8")
        for field in (
            "calls=%llu", "indexed_queries=%llu", "rejected_queries=%llu",
            "full_candidates=%llu", "indexed_candidates=%llu",
            "fallback_candidates=%llu", "exact_tests=%llu",
            "false_positives=%llu", "fallbacks=%llu",
            "duplicate_pairs=%llu", "duplicate_replays=%llu",
            "duplicate_exact_tests_saved=%llu", "duplicate_fallbacks=%llu",
        ):
            self.assertIn(field, telemetry)
        self.assertIn("BULLET_CANCEL_SPATIAL=%d", main)
        self.assertIn("TH08_PSP_FEATURE_BULLET_CANCEL_SPATIAL", main)


if __name__ == "__main__":
    unittest.main()
