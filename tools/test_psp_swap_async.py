#!/usr/bin/env python3
"""Source-contract tests for TH08_PSP_SWAP_ASYNC (flip thread on top of SWAP_NOWAIT)."""

from __future__ import annotations

import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SWITCH = "TH08_PSP_SWAP_ASYNC"


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


class PspSwapAsyncTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.makefile = read("Makefile.psp")
        cls.header = read("psp/swap_async.hpp")
        cls.module = read("psp/swap_async.cpp")
        cls.compat = read("src/modern/linux/d3d8_compat.cpp")
        cls.psp_main = read("psp/main.cpp")
        cls.vendored = [p.name for p in (ROOT / "deps/pspgl-ge4/include").glob("*.h")]

    def test_makefile_and_fingerprint(self) -> None:
        self.assertIn(f"{SWITCH} ?= 0", self.makefile)
        self.assertIn(f"$(error {SWITCH}=1 requires TH08_PSP_SWAP_NOWAIT=1)", self.makefile)
        self.assertIn(f"$(error {SWITCH} must be 0 or 1)", self.makefile)
        self.assertIn("PSP_SWAP_ASYNC_SRCS := psp/swap_async.cpp", self.makefile)
        self.assertIn("ifeq ($(TH08_PSP_SWAP_NOWAIT)$(TH08_PSP_SWAP_ASYNC),00)\nPSP_SWAP_ASYNC_SRCS :=", self.makefile)
        self.assertIn("$(PSP_SWAP_ASYNC_SRCS)", self.makefile)
        self.assertIn(f"CXXFLAGS += -D{SWITCH}=1", self.makefile)
        self.assertIn("psp/swap_async.o: CXXFLAGS += -Ideps/pspgl-ge4/include", self.makefile)
        self.assertIn("src/modern/linux/d3d8_compat.o psp/swap_async.o psp/perf_env.o psp/main.o: \\\n\t$(SWAP_ASYNC_CONFIG_STAMP)", self.makefile)
        self.assertIn('"SWAP_NOWAIT=%d SWAP_ASYNC=%d "', self.psp_main)
        self.assertIn("        TH08_PSP_FEATURE_SWAP_ASYNC,", self.psp_main)
        self.assertIn("#define TH08_PSP_SWAP_ASYNC_ENABLED 0", self.header)
        for name in ("pspgl_internal.h", "pspgl_buffers.h", "guconsts.h", "pspgl_hash.h", "pspgl_misc.h"):
            self.assertIn(name, self.vendored)

    def test_module_contract(self) -> None:
        m = self.module
        # Layout proof against the frozen archive (eglSwapBuffers swaps +16/+20,
        # vidmem reads +12/+14, context swap_interval at +2208).
        for line in ("static_assert(offsetof(struct pspgl_surface, pixelperline) == 12U",
                     "static_assert(offsetof(struct pspgl_surface, flags) == 14U",
                     "static_assert(offsetof(struct pspgl_surface, color_front) == 16U",
                     "static_assert(offsetof(struct pspgl_surface, color_back) == 20U",
                     "static_assert(offsetof(struct pspgl_context, swap_interval) == 2208U"):
            self.assertIn(line, m, line)
        thread = function_body(m, "int FlipThread(SceSize, void *)")
        self.assertIn("sceGeDrawSync(0);", thread)
        self.assertIn("PSP_DISPLAY_SETBUF_NEXTFRAME", thread)
        self.assertLess(thread.index("sceGeDrawSync(0);"), thread.index("sceDisplaySetFrameBuf("))
        self.assertLess(thread.index("sceDisplaySetFrameBuf("), thread.index("gFlipVcount = sceDisplayGetVcount();"))
        self.assertLess(thread.index("gFlipVcount = sceDisplayGetVcount();"), thread.index("sceKernelSetEventFlag(gEventFlag, kFlipIssued);"))
        present = function_body(m, "void SwapAsyncPresent()")
        self.assertIn("surface->flags = static_cast<unsigned char>(savedFlags & ~SURF_DISPLAYED);", present)
        self.assertIn("eglSwapBuffers(", present)
        self.assertIn("surface->flags = savedFlags;", present)
        self.assertLess(present.index("eglSwapBuffers("), present.index("gRequest.base = surface->color_front->base;"))
        wait = function_body(m, "void SwapAsyncWaitFlipComplete(std::uint64_t *waitedUs)")
        self.assertIn("kFlipIssued", wait)
        self.assertIn("while (!SwapDisplayShows(gRequest.base))", wait)
        self.assertIn("sceDisplayGetFrameBuf(&top, &width, &format, PSP_DISPLAY_SETBUF_IMMEDIATE)", m)
        self.assertIn("#define TH08_PSP_SWAP_QUERY_ENABLED 1", self.header)
        init = function_body(m, "bool SwapAsyncInitialize()")
        self.assertIn("mainPriority > 17 ? mainPriority - 1 : mainPriority", init)
        self.assertNotIn("malloc", m)

    def test_compat_wiring(self) -> None:
        c = self.compat
        present = function_body(c, "HRESULT Present(const RECT *, const RECT *, HWND, const RGNDATA *)")
        self.assertIn("#if TH08_PSP_SWAP_ASYNC_ENABLED\n        if (th08::psp::SwapAsyncActive())\n        {\n            th08::psp::SwapAsyncPresent();", present)
        guard = function_body(c, "void PspWaitForPendingFlip()")
        self.assertIn("th08::psp::SwapAsyncWaitFlipComplete(&waitedUs);", guard)
        self.assertIn('TH08_PSP_BOOT_CHECKPOINT("swap_async", "after",', c)
        self.assertIn("th08::psp::SwapAsyncInitialize() ? 1 : 0);", c)


if __name__ == "__main__":
    unittest.main()
