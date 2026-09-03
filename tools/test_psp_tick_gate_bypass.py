#!/usr/bin/env python3
"""Source-contract tests for TH08_PSP_TICK_GATE_BYPASS (no spinning on the retail 1/60 gate)."""

from __future__ import annotations

import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SWITCH = "TH08_PSP_TICK_GATE_BYPASS"


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


class PspTickGateBypassTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.makefile = read("Makefile.psp")
        cls.header = read("psp/tick_gate_bypass.hpp")
        cls.game_main = read("src/main.cpp")
        cls.psp_main = read("psp/main.cpp")

    def test_makefile_and_fingerprint(self) -> None:
        m = self.makefile
        self.assertIn(f"{SWITCH} ?= 0", m)
        self.assertIn(f"$(error {SWITCH} must be 0 or 1)", m)
        self.assertIn(f"CXXFLAGS += -D{SWITCH}=1", m)
        for stamp in ("tick-gate-bypass-0.stamp", "tick-gate-bypass-1.stamp"):
            self.assertIn(stamp, m)
        self.assertIn("src/main.o psp/main.o: \\\n\t$(TICK_GATE_BYPASS_CONFIG_STAMP)", m)
        self.assertIn('"TICK_GATE_BYPASS=%d "', self.psp_main)
        self.assertIn("        TH08_PSP_FEATURE_TICK_GATE_BYPASS,", self.psp_main)
        self.assertIn(f"#if defined(PSP) && defined({SWITCH}) && {SWITCH}\n#define {SWITCH}_ENABLED 1", self.header)
        self.assertIn(f"#define {SWITCH}_ENABLED 0", self.header)

    def test_gate_is_only_bypassed_under_the_switch(self) -> None:
        self.assertIn('#include "tick_gate_bypass.hpp"', self.game_main)
        render = function_body(self.game_main, "RenderResult GameWindow::Render()")
        self.assertIn(f"#if {SWITCH}_ENABLED\n", render)
        self.assertIn("    if (true)\n#else\n    if (this->lastFrameTime < this->curTimestamp)", render)
        # The retail catch-up bookkeeping and the PSP cadence wait stay in place.
        self.assertIn("this->lastFrameTime += (1.0f / 60);", render)
        self.assertIn("WaitForPspRenderCadence(cadenceResult.simulatedTicksCovered);", render)
        self.assertEqual(render.count("if (true)"), 1)


if __name__ == "__main__":
    unittest.main()
