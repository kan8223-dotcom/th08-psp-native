#!/usr/bin/env python3
"""Source-contract tests for TH08_PSP_EFFECT_OCCUPANCY_AUDIT (shadow audit of the Effect occupancy sidecar)."""

from __future__ import annotations

import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
AUDIT = "TH08_PSP_EFFECT_OCCUPANCY_AUDIT"
PRODUCT = "TH08_PSP_EFFECT_OCCUPANCY_FASTPATH"


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


class PspEffectOccupancyAuditTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.makefile = read("Makefile.psp")
        cls.effects = read("src/EffectManager.cpp")
        cls.module = read("psp/effect_occupancy_audit.cpp")
        cls.perf = read("psp/perf_attribution.cpp")
        cls.psp_main = read("psp/main.cpp")

    def test_makefile_and_fingerprint(self) -> None:
        self.assertIn(f"{AUDIT} ?= 0", self.makefile)
        self.assertIn(f"$(error {AUDIT}=1 requires TH08_PSP_PERF_ATTRIBUTION=1)", self.makefile)
        self.assertIn(f"$(error {AUDIT} and {PRODUCT} are mutually exclusive)", self.makefile)
        self.assertIn(f"CXXFLAGS += -D{AUDIT}=1", self.makefile)
        self.assertIn("PSP_EFFECT_OCCUPANCY_AUDIT_SRCS := psp/effect_occupancy_audit.cpp", self.makefile)
        self.assertIn("src/EffectManager.o psp/effect_occupancy_audit.o psp/perf_attribution.o psp/main.o: \\\n\t$(EFFECT_OCCUPANCY_AUDIT_CONFIG_STAMP)", self.makefile)
        self.assertIn("EFFECT_OCCUPANCY_FASTPATH=%d EFFECT_OCCUPANCY_AUDIT=%d", self.psp_main)
        self.assertIn("        TH08_PSP_FEATURE_EFFECT_OCCUPANCY_AUDIT,", self.psp_main)
        for hook in ("EffectOccupancyStatsResetWindow(gWindowActive);", "EffectOccupancyStatsEmitWindow(gStage, gBaselineStageFrame, stageFrame);", "EffectOccupancyStatsCancelWindow();"):
            self.assertIn(hook, self.perf)

    def test_sidecar_maintained_for_both_and_skip_only_in_product(self) -> None:
        e = self.effects
        self.assertIn("#define TH08_PSP_EFFECT_OCCUPANCY_SIDECAR_ENABLED \\\n    (TH08_PSP_EFFECT_OCCUPANCY_FASTPATH_ENABLED || TH08_PSP_EFFECT_OCCUPANCY_AUDIT_ENABLED)", e)
        self.assertIn('#error "EFFECT_OCCUPANCY fastpath and audit switches are mutually exclusive"', e)
        # Mark (activation paths), Reset and Forget live under the shared sidecar guard.
        self.assertGreaterEqual(e.count("#if TH08_PSP_EFFECT_OCCUPANCY_SIDECAR_ENABLED"), 8)
        update = function_body(e, "ChainCallbackResult EffectManager::OnUpdate(EffectManager *effectManager)")
        skip = update.index("if (!g_PspEffectOccupancy.Test(static_cast<u32>(i)))")
        self.assertIn("#if TH08_PSP_EFFECT_OCCUPANCY_FASTPATH_ENABLED", update[:skip])
        self.assertIn("#elif TH08_PSP_EFFECT_OCCUPANCY_AUDIT_ENABLED\n        // Shadow only", update)
        self.assertIn("const bool pspOccupancyTested =\n            g_PspEffectOccupancy.Test(static_cast<u32>(i));", update)
        inactive = update.index("if (effect->active == 0)")
        note_inactive = update.index("EffectOccupancyAuditNoteInactive(", inactive)
        forget = update.index("g_PspEffectOccupancy.Forget(static_cast<u32>(i));", inactive)
        self.assertLess(note_inactive, forget)
        self.assertIn("pspOccupancyTested, effect->vertices != NULL);", update)
        note_active = update.index("EffectOccupancyAuditNoteActive(pspOccupancyTested);")
        self.assertLess(note_active, update.index("effectManager->activeCount++;"))
        self.assertEqual(self.module.count("BootLog("), 1)
        fmt = "".join(re.findall(r'"([^"]*)"', re.search(r'BootLog\(\s*((?:"[^"]*"\s*)+),', self.module).group(1)))
        self.assertEqual(len(re.findall(r"%(?:0\d)?(?:ll|l)?[dusx]", fmt)), 11)


if __name__ == "__main__":
    unittest.main()
