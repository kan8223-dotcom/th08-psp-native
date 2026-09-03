#!/usr/bin/env python3
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class EnemyActiveBitmapAuditTest(unittest.TestCase):
    def test_core_differential_harness(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "enemy_bitmap_test"
            subprocess.run(
                ["c++", "-std=c++17", "-Ipsp",
                 "tools/test_enemy_active_bitmap_core.cpp", "-o", str(binary)],
                cwd=ROOT, check=True,
            )
            subprocess.run([str(binary)], check=True)

    def test_default_off_and_audit_only_contract(self):
        makefile = (ROOT / "Makefile.psp").read_text()
        self.assertIn("TH08_PSP_ENEMY_ACTIVE_BITMAP_AUDIT ?= 0", makefile)
        self.assertIn("-DTH08_PSP_ENEMY_ACTIVE_BITMAP_AUDIT=1", makefile)

    def test_canonical_loop_is_retained(self):
        source = (ROOT / "src/EnemyManagerUpdate.cpp").read_text()
        loop = "for (enemyIndex = 0; enemyIndex < 480; ++enemyIndex, ++enemy)"
        self.assertIn(loop, source)
        self.assertLess(source.index("TH08_PSP_ENEMY_BITMAP_BEGIN_FRAME"),
                        source.index(loop))
        self.assertIn("TH08_PSP_ENEMY_BITMAP_OBSERVE", source)
        self.assertLess(source.index("TH08_PSP_ENEMY_BITMAP_END_FRAME"),
                        source.index("g_GameManager.IsTampered()"))

    def test_recursive_spawn_is_reserved_before_ecl(self):
        source = (ROOT / "src/EnemyTimeline.cpp").read_text()
        self.assertEqual(source.count("TH08_PSP_ENEMY_BITMAP_TRACK(this, i);"), 2)
        for marker in ("Enemy *EnemyManager::SpawnEnemy1", "Enemy *EnemyManager::SpawnEnemy2"):
            block = source[source.index(marker):]
            self.assertLess(block.index("TH08_PSP_ENEMY_BITMAP_TRACK(this, i);"),
                            block.index("g_EclManager.CallEclSub"))

    def test_sidecar_is_external_to_game_abi(self):
        manager = (ROOT / "src/EnemyManager.hpp").read_text()
        player = (ROOT / "src/Player.hpp").read_text()
        self.assertNotIn("EnemyActiveBitmap", manager)
        self.assertNotIn("EnemyActiveBitmap", player)

    def test_mismatch_repairs_are_audit_only_and_fail_loud(self):
        source = (ROOT / "psp/enemy_active_bitmap_audit.cpp").read_text()
        self.assertIn('FailLoudOnce("false_negative"', source)
        self.assertIn('FailLoudOnce("stale_positive"', source)
        self.assertIn("canonical_authoritative=1 product_skip=0", source)
        self.assertIn("FlushBootLog();", source)
        self.assertIn("kMaxPeriodicLogsPerManagerLifetime = 12", source)
        self.assertIn("periodicLogs <", source)


if __name__ == "__main__":
    unittest.main()
