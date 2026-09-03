#!/usr/bin/env python3
"""Source-contract tests for TH08_PSP_SWAP_NOWAIT (present without the PSPGL VBlank wait)."""

from __future__ import annotations

import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SWITCH = "TH08_PSP_SWAP_NOWAIT"


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


class PspSwapNowaitTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.makefile = read("Makefile.psp")
        cls.header = read("psp/swap_nowait.hpp")
        cls.compat = read("src/modern/linux/d3d8_compat.cpp")
        cls.game_main = read("src/main.cpp")
        cls.psp_main = read("psp/main.cpp")

    def test_makefile_and_fingerprint(self) -> None:
        self.assertIn(f"{SWITCH} ?= 0", self.makefile)
        self.assertIn(f"$(error {SWITCH} must be 0 or 1)", self.makefile)
        self.assertIn(f"CXXFLAGS += -D{SWITCH}=1", self.makefile)
        self.assertIn("src/modern/linux/d3d8_compat.o src/main.o psp/main.o psp/swap_async.o: \\\n\t$(SWAP_NOWAIT_CONFIG_STAMP)", self.makefile)
        for stamp in ("swap-nowait-0.stamp", "swap-nowait-1.stamp"):
            self.assertIn(stamp, self.makefile)
        self.assertIn('"SWAP_NOWAIT=%d SWAP_ASYNC=%d "', self.psp_main)
        self.assertIn("        TH08_PSP_FEATURE_SWAP_NOWAIT,", self.psp_main)
        self.assertIn("#define TH08_PSP_SWAP_NOWAIT_ENABLED 0", self.header)

    def test_compat_device_contract(self) -> None:
        c = self.compat
        # Swap interval 0 only under the switch; the canonical 1 stays for OFF.
        self.assertIn("#if TH08_PSP_SWAP_NOWAIT_ENABLED\n", c)
        self.assertIn("        const int requestedSwapInterval = 0;\n#else\n        const int requestedSwapInterval = 1;\n#endif", c)
        self.assertIn('TH08_PSP_SWAP_NOWAIT_ENABLED ? "cadence_nowait" : "pspgl_vblank"', c)
        # Present records the vcount right after the swap, inside the swap scope.
        present = function_body(c, "HRESULT Present(const RECT *, const RECT *, HWND, const RGNDATA *)")
        swap = present.index("SDL_GL_SwapWindow(window);")
        record = present.index("pspSwapVcount = sceDisplayGetVcount();", swap)
        self.assertLess(swap, record)
        self.assertEqual(present.count("pspSwapFlipPending = true;"), 2)  # async and synchronous paths
        # The guard waits for the flip VBlank in the Swap wait context and is reached
        # before every GE write path.
        guard = function_body(c, "void PspWaitForPendingFlip()")
        # No wait context: the guard stays inside DrawFrame and PERF_ENV reports it (fw=).
        self.assertNotIn("PerfAttributionWaitContextScope", guard)
        self.assertIn("PerfEnvNoteFlipWait(", guard)
        self.assertIn("while (!th08::psp::SwapDisplayShows(frontBase))", guard)
        self.assertNotIn("sceDisplayGetVcount() - pspSwapVcount) < 1U", guard)
        self.assertIn("sceDisplayWaitVblankStart();", guard)
        self.assertIn("pspSwapFlipPending = false;", guard)
        for signature in ("HRESULT BeginScene()",
                          "HRESULT Draw(D3DPRIMITIVETYPE type, UINT primitiveCount, const BYTE *data, UINT stride)",
                          "HRESULT DrawIndexed(D3DPRIMITIVETYPE type, UINT minVertexIndex,"):
            body = function_body(c, signature)
            self.assertIn("#if TH08_PSP_SWAP_NOWAIT_ENABLED\n        PspWaitForPendingFlip();\n#endif", body, signature)
        # Clear keeps the unconditional guard unless the colour-only child switch
        # (TH08_PSP_FLIP_GUARD_COLOR_ONLY, tools/test_psp_flip_guard_color_only.py)
        # limits it to TARGET/STENCIL clears.
        clear = function_body(c, "HRESULT Clear(DWORD, const D3DRECT *, DWORD flags, D3DCOLOR color, float depth, DWORD)")
        self.assertIn("#if TH08_PSP_SWAP_NOWAIT_ENABLED\n#if TH08_PSP_FLIP_GUARD_COLOR_ONLY_ENABLED\n", clear)
        self.assertIn("#else\n        PspWaitForPendingFlip();\n#endif\n#endif", clear)
        self.assertEqual(c.count("PspWaitForPendingFlip();"), 5)

    def test_cadence_owns_every_interval(self) -> None:
        body = function_body(self.game_main, "static void WaitForPspRenderCadence(u8 simulatedTicksCovered)")
        self.assertIn("#if TH08_PSP_SWAP_NOWAIT_ENABLED", body)
        self.assertIn("if (!gPspHasPresented || simulatedTicksCovered == 0)", body)
        self.assertIn("static_cast<u32>(simulatedTicksCovered);\n#else", body)
        self.assertIn("static_cast<u32>(simulatedTicksCovered - 1U);\n#endif", body)
        self.assertIn("sceDisplayWaitVblankStart();", body)
        self.assertIn('#include "swap_nowait.hpp"', self.game_main)


if __name__ == "__main__":
    unittest.main()
