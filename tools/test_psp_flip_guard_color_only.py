#!/usr/bin/env python3
"""Source-contract tests for TH08_PSP_FLIP_GUARD_COLOR_ONLY (flip guard only before colour writes)."""

from __future__ import annotations

import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SWITCH = "TH08_PSP_FLIP_GUARD_COLOR_ONLY"
PARENT = "TH08_PSP_SWAP_NOWAIT"


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


class PspFlipGuardColorOnlyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.makefile = read("Makefile.psp")
        cls.header = read("psp/swap_nowait.hpp")
        cls.compat = read("src/modern/linux/d3d8_compat.cpp")
        cls.psp_main = read("psp/main.cpp")
        cls.game_manager = read("src/GameManager.cpp")

    def test_makefile_and_fingerprint(self) -> None:
        m = self.makefile
        self.assertIn(f"{SWITCH} ?= 0", m)
        self.assertIn(f"$(error {SWITCH} must be 0 or 1)", m)
        self.assertIn(f"$(error {SWITCH}=1 requires {PARENT}=1)", m)
        self.assertIn(f"CXXFLAGS += -D{SWITCH}=1", m)
        for stamp in ("flip-guard-color-only-0.stamp", "flip-guard-color-only-1.stamp"):
            self.assertIn(stamp, m)
        self.assertIn("src/modern/linux/d3d8_compat.o psp/main.o: \\\n\t$(FLIP_GUARD_COLOR_ONLY_CONFIG_STAMP)", m)
        self.assertIn('"FLIP_GUARD_COLOR_ONLY=%d "', self.psp_main)
        self.assertIn("        TH08_PSP_FEATURE_FLIP_GUARD_COLOR_ONLY,", self.psp_main)
        # The child macro folds the parent in: never enabled without SWAP_NOWAIT.
        self.assertIn(f"#if TH08_PSP_SWAP_NOWAIT_ENABLED && defined({SWITCH}) && \\\n    {SWITCH}\n#define {SWITCH}_ENABLED 1", self.header)
        self.assertIn(f"#define {SWITCH}_ENABLED 0", self.header)

    def test_clear_guards_colour_writes_only(self) -> None:
        clear = function_body(self.compat, "HRESULT Clear(DWORD, const D3DRECT *, DWORD flags, D3DCOLOR color, float depth, DWORD)")
        self.assertIn(
            f"#if TH08_PSP_SWAP_NOWAIT_ENABLED\n#if {SWITCH}_ENABLED\n",
            clear)
        self.assertIn(
            "        if ((flags & (D3DCLEAR_TARGET | D3DCLEAR_STENCIL)) != 0)\n            PspWaitForPendingFlip();\n#else\n        PspWaitForPendingFlip();\n#endif\n#endif",
            clear)
        # The guard itself is untouched: still the display-identity wait.
        guard = function_body(self.compat, "void PspWaitForPendingFlip()")
        self.assertIn("while (!th08::psp::SwapDisplayShows(frontBase))", guard)
        # Every other GE write path keeps the unconditional guard.
        for signature in ("HRESULT BeginScene()",
                          "HRESULT Draw(D3DPRIMITIVETYPE type, UINT primitiveCount, const BYTE *data, UINT stride)",
                          "HRESULT DrawIndexed(D3DPRIMITIVETYPE type, UINT minVertexIndex,"):
            body = function_body(self.compat, signature)
            self.assertIn("#if TH08_PSP_SWAP_NOWAIT_ENABLED\n        PspWaitForPendingFlip();\n#endif", body, signature)
            self.assertNotIn(f"{SWITCH}_ENABLED", body, signature)

    def test_calc_chain_depth_only_clear_is_the_target(self) -> None:
        # GameManager's per-tick clear (calc chain) is depth-only; that is the
        # call the guard used to stall on right after every Present.
        self.assertIn(
            "    g_Supervisor.d3dDevice->Clear(\n        0, NULL, D3DCLEAR_ZBUFFER, g_Background.skyFog.color.d3dColor, 1.0f, 0);",
            self.game_manager)


if __name__ == "__main__":
    unittest.main()
