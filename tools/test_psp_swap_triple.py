#!/usr/bin/env python3
"""Source-contract tests for TH08_PSP_SWAP_TRIPLE (third colour buffer, no GE wait at Present)."""

from __future__ import annotations

import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SWITCH = "TH08_PSP_SWAP_TRIPLE"


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


class PspSwapTripleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.makefile = read("Makefile.psp")
        cls.header = read("psp/swap_triple.hpp")
        cls.module = read("psp/swap_triple.cpp")
        cls.hook = read("psp/ge_list_hook.cpp")
        cls.compat = read("src/modern/linux/d3d8_compat.cpp")
        cls.game_main = read("src/main.cpp")
        cls.psp_main = read("psp/main.cpp")

    def test_makefile_and_fingerprint(self) -> None:
        m = self.makefile
        self.assertIn(f"{SWITCH} ?= 0", m)
        self.assertIn(f"$(error {SWITCH}=1 requires TH08_PSP_SWAP_NOWAIT=1)", m)
        self.assertIn(f"$(error {SWITCH} and TH08_PSP_SWAP_ASYNC are mutually exclusive)", m)
        self.assertIn(f"$(error {SWITCH} must be 0 or 1)", m)
        self.assertIn("PSP_SWAP_TRIPLE_SRCS := psp/swap_triple.cpp", m)
        self.assertIn("$(PSP_SWAP_TRIPLE_SRCS) \\\n\t$(PSP_GE_LIST_HOOK_SRCS) \\", m)
        self.assertIn(f"CXXFLAGS += -D{SWITCH}=1", m)
        self.assertIn("psp/swap_triple.o: CXXFLAGS += -Ideps/pspgl-ge4/include", m)
        self.assertIn("ifneq ($(TH08_PSP_PERF_ENV)$(TH08_PSP_SWAP_TRIPLE),00)\n# GE queue observer / triple-buffer flip poll: every PSPGL list submission.\nLDFLAGS += -Wl,--wrap=sceGeListEnQueue\nendif", m)
        for stamp in ("swap-triple-0.stamp", "swap-triple-1.stamp"):
            self.assertIn(stamp, m)
        self.assertIn("psp/swap_async.o psp/ge_list_hook.o psp/perf_env.o psp/stage_pool_arena.o: \\\n\t$(SWAP_TRIPLE_CONFIG_STAMP)", m)
        self.assertIn('"SWAP_TRIPLE=%d "', self.psp_main)
        self.assertIn("        TH08_PSP_FEATURE_SWAP_TRIPLE,", self.psp_main)
        self.assertIn(f"#define {SWITCH}_ENABLED 0", self.header)

    def test_module_contract(self) -> None:
        s = self.module
        # Layout pins, as in swap_async.cpp.
        for off in ("pixfmt) == 4U", "pixelperline) == 12U", "flags) == 14U", "color_front) == 16U", "color_back) == 20U"):
            self.assertIn(f"static_assert(offsetof(struct pspgl_surface, {off}", s)
        init = function_body(s, "bool SwapTripleInitialize()")
        # Upper tier only: the lower tier's texture working set must stay intact (r124).
        self.assertIn("__pspgl_buffer_new(static_cast<GLsizeiptr>(bufferBytes), TH08_PSPGL_UPPER_STATIC)", init)
        self.assertIn("#define TH08_PSPGL_UPPER_STATIC 0x60000001u", s)
        self.assertIn("if (spareBase < kUpperEdramBase || spareBase + bufferBytes > kUpperEdramEnd)", init)
        self.assertIn("if (__pspgl_vidmem_avail() != availBefore)", init)
        self.assertIn("gSpare->flags |= BF_PINNED_FIXED;", init)
        self.assertIn('BootLog("SWAP_TRIPLE init=FAIL reason=upper_vidmem', init)
        # GE-read memory recycled on the old Present contract is fenced explicitly.
        fence = function_body(s, "void SwapTripleWaitPendingDone()")
        self.assertIn("sceGeListSync(gPendingQid, 0);", fence)
        drain = function_body(s, "void SwapTripleDrain()")
        self.assertIn("sceGeDrawSync(0);", drain)
        self.assertIn("sceKernelDelayThread(100);", function_body(s, "void WaitDisplayShows(const void *base)"))
        present = function_body(s, "void SwapTriplePresent(std::uint64_t *waitedUs)")
        # Never hand the GE the buffer on screen: wait for display identity first.
        self.assertIn("WaitDisplayShows(gPendingBuf->base);", present)
        self.assertLess(present.index("SwapDisplayShows(gPendingBuf->base)"), present.index("surface->color_back = next;"))
        # No GE sync on the normal path: the only sync is the forced one for a frame still pending.
        self.assertEqual(present.count("sceGeListSync(gPendingQid, 0);"), 1)
        self.assertIn("surface->flags = static_cast<unsigned char>(savedFlags & ~SURF_DISPLAYED);", present)
        self.assertIn("__pspgl_vidmem_setup_write_and_display_buffer(surface);", present)
        poll = function_body(s, "void SwapTriplePoll()")
        self.assertIn("TH08_SWAP_TRIPLE_PEEK(gPendingQid)", poll)
        self.assertIn("PSP_DISPLAY_SETBUF_NEXTFRAME", function_body(s, "void RequestFlip(unsigned long &counter)"))
        self.assertIn("th08::psp::SwapTripleNoteListEnqueued(qid);", self.hook)

    def test_wiring(self) -> None:
        c = self.compat
        self.assertIn('#include "swap_triple.hpp"', c)
        self.assertIn("th08::psp::SwapTripleInitialize() ? 1 : 0", c)
        present = function_body(c, "HRESULT Present(const RECT *, const RECT *, HWND, const RGNDATA *)")
        triple = present.index("if (th08::psp::SwapTripleActive())")
        self.assertLess(triple, present.index("SDL_GL_SwapWindow(window);"))
        self.assertIn("th08::psp::SwapTriplePresent(&tripleWaitUs);\n            (void)tripleWaitUs;\n            pspSwapFlipPending = false;", present)
        self.assertIn('#include "swap_triple.hpp"', self.game_main)
        render = function_body(self.game_main, "RenderResult GameWindow::Render()")
        self.assertLess(render.index("psp::SwapTriplePoll();"), render.index("RunCalcChain()"))
        # The Bullet direct-GE arena reset fences the previous frame; stage
        # arena transitions and device reset drain the GE.
        begin = function_body(c, "HRESULT BeginScene()")
        self.assertIn("th08::psp::SwapTripleWaitPendingDone();", begin)
        self.assertLess(begin.index("SwapTripleWaitPendingDone();"), begin.index("pspBulletDirectGeVertexCursor = 0U;"))
        self.assertIn("th08::psp::SwapTripleDrain();\n#endif\n        framebufferReady = ResetInternal(*parameters);", c)
        arena = read("psp/stage_pool_arena.cpp")
        self.assertEqual(arena.count("SwapTripleDrain();"), 3)


if __name__ == "__main__":
    unittest.main()
