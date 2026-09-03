#!/usr/bin/env python3
"""Source-contract tests for TH08_PSP_BULLET_COLLISION_GATE (product negative gate)."""

from __future__ import annotations

import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SWITCH = "TH08_PSP_BULLET_COLLISION_GATE"


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    raise AssertionError(f"unterminated function: {signature}")


class PspBulletCollisionGateTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.makefile = read("Makefile.psp")
        cls.bullets = read("src/BulletManager.cpp")
        cls.player = read("src/Player.cpp")
        cls.module = read("psp/bullet_gate_stats.cpp")
        cls.perf = read("psp/perf_attribution.cpp")
        cls.psp_main = read("psp/main.cpp")

    def test_makefile_and_fingerprint(self) -> None:
        self.assertIn(f"{SWITCH} ?= 0", self.makefile)
        self.assertIn(f"$(error {SWITCH}=1 requires TH08_PSP_PLAYER_SCAN_SIDECAR=1)", self.makefile)
        self.assertIn(f"$(error {SWITCH} and {SWITCH}_AUDIT are mutually exclusive)", self.makefile)
        self.assertIn(f"$(error {SWITCH} must be 0 or 1)", self.makefile)
        self.assertIn("PSP_BULLET_GATE_SRCS := psp/bullet_gate_stats.cpp", self.makefile)
        self.assertIn(f"CXXFLAGS += -D{SWITCH}=1", self.makefile)
        self.assertIn("src/BulletManager.o psp/bullet_gate_stats.o psp/perf_attribution.o psp/main.o: \\\n\t$(BULLET_COLLISION_GATE_CONFIG_STAMP)", self.makefile)
        self.assertIn("BULLET_COLLISION_GATE_AUDIT=%d BULLET_COLLISION_GATE=%d", self.psp_main)
        self.assertIn("        TH08_PSP_FEATURE_BULLET_COLLISION_GATE,", self.psp_main)
        for hook in ("BulletGateStatsResetWindow(gWindowActive);", "BulletGateStatsEmitWindow(gStage, gBaselineStageFrame, stageFrame);", "BulletGateStatsCancelWindow();"):
            self.assertIn(hook, self.perf)

    def test_gate_reproduces_the_canonical_clear_side_effect(self) -> None:
        # Both canonical first calls set bulletCancelItemType = 6 before anything else
        # and return 0 (no other side effect) when strictly separated.
        for fn in ("i32 Player::CheckGrazeCollision(Float3 *position, Float3 *size)", "i32 Player::CheckBulletCollision(Float3 *position, Float3 *size)"):
            body = function_body(self.player, fn)
            self.assertIn("this->bulletCancelItemType = 6;", body)
            self.assertLess(body.index("this->bulletCancelItemType = 6;"), body.index("CheckBulletCancelCollision"))
        b = self.bullets
        self.assertIn("#error TH08_PSP_BULLET_COLLISION_GATE and its audit are mutually exclusive", b)
        self.assertIn("#error TH08_PSP_BULLET_COLLISION_GATE requires TH08_PSP_PLAYER_SCAN_SIDECAR", b)
        update = function_body(b, "ChainCallbackResult BulletManager::OnUpdate(BulletManager *bulletManager)")
        gate = update.index("#if defined(TH08_PSP_BULLET_COLLISION_GATE_PRODUCT_ENABLED)\n                // Negative gate")
        canonical = update.index("collisionResult = g_Player.CheckGrazeCollision(&bullet->position,")
        self.assertLess(gate, canonical)
        block = update[gate:canonical]
        for line in ("PspPlayerBulletCollisionAuditBoundsMatch(&g_Player,", "th08::psp::PspBulletCollisionDefinitelyClear(",
                     "collisionAuditSnapshot.knownEmpty != 0U,", "collisionAuditSnapshotBoundsValid,",
                     "if (th08::psp::PspBulletCollisionGateIsClear(gateDecision))", "g_Player.bulletCancelItemType = 6;",
                     "goto executeBulletScript;", "BulletGateOutcome::FallbackBoundsMutated"):
            self.assertIn(line, block, line)
        # The snapshot is captured for the product too, but the audit-only frame note stays audit-only.
        self.assertIn("#if defined(TH08_PSP_BULLET_COLLISION_GATE_AUDIT_ENABLED) || \\\n    defined(TH08_PSP_BULLET_COLLISION_GATE_PRODUCT_ENABLED)\n    collisionAuditSnapshot =", b)
        self.assertIn("#if defined(TH08_PSP_BULLET_COLLISION_GATE_AUDIT_ENABLED)\n    PspBulletCollisionGateAuditNoteFrame(collisionAuditSnapshot);\n#endif", b)
        self.assertEqual(self.module.count("BootLog("), 1)
        fmt = "".join(re.findall(r'"([^"]*)"', re.search(r'BootLog\(\s*((?:"[^"]*"\s*)+),', self.module).group(1)))
        self.assertEqual(len(re.findall(r"%(?:0\d)?(?:ll|l)?[dusx]", fmt)), 12)


if __name__ == "__main__":
    unittest.main()
