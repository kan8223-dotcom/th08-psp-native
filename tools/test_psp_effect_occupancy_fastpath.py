#!/usr/bin/env python3
"""Focused safety gate for the PSP Effect occupancy sidecar.

The optimization is allowed to have stale-positive bits, but never a false
negative.  These tests bind the differential lifetime model to every TH08
Effect activation writer and audit external writers as deactivate-only.
"""

from __future__ import annotations

import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
EFFECT_CPP_PATH = ROOT / "src" / "EffectManager.cpp"
EFFECT_CPP = EFFECT_CPP_PATH.read_text(encoding="utf-8")
EFFECT_HPP = (ROOT / "src" / "EclManager.hpp").read_text(encoding="utf-8")
MAKEFILE = (ROOT / "Makefile.psp").read_text(encoding="utf-8")
PSP_MAIN = (ROOT / "psp" / "main.cpp").read_text(encoding="utf-8")
FEATURE = "TH08_PSP_EFFECT_OCCUPANCY_FASTPATH"


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    open_brace = source.index("{", start)
    depth = 0
    for index in range(open_brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[open_brace : index + 1]
    raise AssertionError(f"unterminated function: {signature}")


class EffectOccupancyProductionBindingTests(unittest.TestCase):
    def test_psp_only_gate_and_abi_external_storage(self) -> None:
        gate = (
            f"#if defined(PSP) && defined({FEATURE}) && \\\n"
            f"    {FEATURE}"
        )
        self.assertGreaterEqual(EFFECT_CPP.count(gate), 9)
        self.assertIn('#include "PspEffectOccupancy.hpp"', EFFECT_CPP)
        self.assertIn(
            "static psp::PspEffectOccupancyBits g_PspEffectOccupancy;",
            EFFECT_CPP,
        )
        self.assertNotIn("PspEffectOccupancy", EFFECT_HPP)

    def test_build_toggle_is_default_off_stamped_and_identified(self) -> None:
        self.assertIn(f"{FEATURE} ?= 0", MAKEFILE)
        self.assertIn(f"-D{FEATURE}=1", MAKEFILE)
        self.assertIn("effect-occupancy-fastpath-0.stamp", MAKEFILE)
        self.assertIn("effect-occupancy-fastpath-1.stamp", MAKEFILE)
        self.assertIn(
            "src/EffectManager.o psp/main.o: "
            "$(EFFECT_OCCUPANCY_FASTPATH_CONFIG_STAMP)",
            MAKEFILE,
        )
        self.assertIn("EFFECT_OCCUPANCY_FASTPATH=%d", PSP_MAIN)
        self.assertIn("TH08_PSP_FEATURE_EFFECT_OCCUPANCY_FASTPATH", PSP_MAIN)

    def test_reset_and_all_five_activation_writers_publish(self) -> None:
        reset = function_body(EFFECT_CPP, "void EffectManager::ResetEffects()")
        self.assertLess(reset.index("memset(this, 0, 0x8B05C);"),
                        reset.index("g_PspEffectOccupancy.Reset();"))

        signatures = (
            "Effect *EffectManager::SpawnEffect(",
            "Effect *EffectManager::SpawnEffectWithVelocity(",
            "Effect *EffectManager::SpawnEffectInFixedSlot(",
            "Effect *EffectManager::SpawnEffectInFixedSlotWithVelocity(",
            "Effect *EffectManager::SpawnEffectInSecondaryPool(",
        )
        for signature in signatures:
            body = function_body(EFFECT_CPP, signature)
            self.assertEqual(body.count("effect->active = 1;"), 1, signature)
            self.assertEqual(
                body.count("PspMarkEffectOccupied(this, effect);"), 1, signature
            )
            self.assertLess(
                body.index("effect->active = 1;"),
                body.index("PspMarkEffectOccupied(this, effect);"),
                signature,
            )
            self.assertLess(
                body.index("PspMarkEffectOccupied(this, effect);"),
                body.index("g_EffectTemplates[id]"),
                signature,
            )
            # Initializer failure is a pending-cleanup state, not a reason to
            # clear the bit inside the producer.
            self.assertNotIn("g_PspEffectOccupancy.Forget", body)

        self.assertEqual(EFFECT_CPP.count("effect->active = 1;"), 5)
        self.assertEqual(
            EFFECT_CPP.count("PspMarkEffectOccupied(this, effect);"), 5
        )

    def test_onupdate_gate_order_and_delayed_cleanup_clear(self) -> None:
        update = function_body(
            EFFECT_CPP,
            "ChainCallbackResult EffectManager::OnUpdate(EffectManager *effectManager)",
        )
        test_pos = update.index("g_PspEffectOccupancy.Test(static_cast<u32>(i))")
        inactive_pos = update.index("if (effect->active == 0)")
        forget_pos = update.index(
            "g_PspEffectOccupancy.Forget(static_cast<u32>(i));"
        )
        active_count_pos = update.index("effectManager->activeCount++;")
        self.assertLess(test_pos, inactive_pos)
        self.assertLess(inactive_pos, forget_pos)
        self.assertLess(forget_pos, active_count_pos)
        self.assertEqual(update.count("g_PspEffectOccupancy.Forget"), 1)
        self.assertEqual(update.count("effect->active = 0;"), 2)

        # The test happens per ascending slot, rather than over a snapshot, so
        # a callback's Mark(higherIndex) is visible later in the same frame.
        loop = update[update.index("for (i = 0; i < 653;") :]
        self.assertIn("for (i = 0; i < 653; i++, effect++)", loop)
        self.assertNotIn("PspEffectOccupancyBits occupancySnapshot", loop)

    def test_external_effect_writers_are_deactivate_only(self) -> None:
        writes: list[tuple[str, str]] = []
        patterns = (
            re.compile(
                r"(?:this->)?[A-Za-z_]\w*[Ee]ffect\w*->active\s*=\s*([^;]+);"
            ),
            re.compile(r"GetFixedSlotEffect\([^)]*\)->active\s*=\s*([^;]+);"),
            re.compile(
                r"g_EffectManager\.effects\[[^]]+\]\.active\s*=\s*([^;]+);"
            ),
        )
        for path in sorted((ROOT / "src").glob("**/*")):
            if path.suffix not in {".cpp", ".hpp"} or path == EFFECT_CPP_PATH:
                continue
            source = path.read_text(encoding="utf-8")
            for pattern in patterns:
                for match in pattern.finditer(source):
                    writes.append((str(path.relative_to(ROOT)), match.group(1).strip()))

        self.assertGreaterEqual(len(writes), 3)
        unsafe = [(path, value) for path, value in writes
                  if value not in {"0", "false"}]
        self.assertEqual(unsafe, [], f"external Effect activation bypass: {unsafe}")
        self.assertIn(("src/Spellcard.cpp", "0"), writes)
        self.assertGreaterEqual(writes.count(("src/EclExIns.cpp", "0")), 2)


class EffectOccupancyDifferentialHarnessTests(unittest.TestCase):
    def test_same_frame_order_cleanup_and_sentinel_model(self) -> None:
        compiler = os.environ.get("CXX", "c++")
        source = ROOT / "tools" / "psp_effect_occupancy_harness.cpp"
        with tempfile.TemporaryDirectory(prefix="th08-effect-occupancy-") as temp:
            executable = Path(temp) / "effect_occupancy_harness"
            compile_result = subprocess.run(
                [compiler, "-std=c++17", "-O2", "-I", str(ROOT),
                 str(source), "-o", str(executable)],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            self.assertEqual(
                compile_result.returncode,
                0,
                compile_result.stdout + compile_result.stderr,
            )
            run_result = subprocess.run(
                [str(executable)],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            self.assertEqual(
                run_result.returncode,
                0,
                run_result.stdout + run_result.stderr,
            )
            self.assertIn("PSP_EFFECT_OCCUPANCY_HARNESS PASS", run_result.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
