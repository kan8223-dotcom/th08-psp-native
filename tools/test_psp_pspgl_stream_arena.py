#!/usr/bin/env python3
"""Source-contract tests for TH08_PSP_PSPGL_STREAM_ARENA frame leases."""

from __future__ import annotations

import hashlib
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SWITCH = "TH08_PSP_PSPGL_STREAM_ARENA"


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


class PspglStreamArenaTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.makefile = read("Makefile.psp")
        cls.header = read("psp/pspgl_stream_arena.hpp")
        cls.compat = read("src/modern/linux/d3d8_compat.cpp")
        cls.psp_main = read("psp/main.cpp")
        cls.patch = read("deps/pspgl-ge4/pspgl-th08-stream-arena-v1.patch")

    def test_makefile_archive_and_fingerprint(self) -> None:
        m = self.makefile
        self.assertIn(f"{SWITCH} ?= 0", m)
        self.assertIn(f"$(error {SWITCH} must be 0 or 1)", m)
        self.assertIn(f"$(error {SWITCH} and TH08_PSP_SWAP_ASYNC are mutually exclusive)", m)
        self.assertIn(f"$(error {SWITCH} and TH08_PSP_SWAP_TRIPLE are mutually exclusive)", m)
        self.assertIn(f"CXXFLAGS += -D{SWITCH}=1", m)
        self.assertIn("PSPGL_GE4_STREAM_ARENA_ARCHIVE := deps/pspgl-ge4/libGL_th08_ge4_streamarena_v1.a", m)
        self.assertIn("ifeq ($(TH08_PSP_PSPGL_STREAM_ARENA),1)\nifneq ($(PSPGL_GE4_SELECTED_ARCHIVE),$(PSPGL_GE4_ARCHIVE))", m)
        self.assertIn("PSPGL_GE4_SELECTED_ARCHIVE := $(PSPGL_GE4_STREAM_ARENA_ARCHIVE)", m)
        for sym in ("__pspgl_th08_stream_arena_install", "__pspgl_th08_stream_arena_begin_frame", "__pspgl_th08_stream_arena_marker"):
            self.assertIn(sym + "$$'", m)
        self.assertIn("grep -Fxq '$(PSPGL_STREAM_ARENA_MARKER)'", m)
        for stamp in ("pspgl-stream-arena-0.stamp", "pspgl-stream-arena-1.stamp"):
            self.assertIn(stamp, m)
        self.assertIn("src/modern/linux/d3d8_compat.o psp/main.o: \\\n\t$(PSPGL_STREAM_ARENA_CONFIG_STAMP)", m)
        self.assertIn('"PSPGL_STREAM_ARENA=%d "', self.psp_main)
        self.assertIn("        TH08_PSP_FEATURE_PSPGL_STREAM_ARENA,", self.psp_main)
        self.assertIn(f"#define {SWITCH}_ENABLED 0", self.header)
        archive = ROOT / "deps/pspgl-ge4/libGL_th08_ge4_streamarena_v1.a"
        self.assertEqual(archive.stat().st_size, 1680912)
        self.assertEqual(hashlib.sha256(archive.read_bytes()).hexdigest(), "a66904358c6449317ca2ca06a511c432a7b697db9021af3b0a4acf6b4319eb96")
        self.assertEqual(hashlib.sha256((ROOT / "deps/pspgl-ge4/pspgl-th08-stream-arena-v1.patch").read_bytes()).hexdigest(),
                         "917b92e380c2582865144e2069e3869fc6472065a6b0e96fe3377a7975ebadfa")

    def test_patch_contract(self) -> None:
        p = self.patch
        self.assertIn("+\tcase GL_STREAM_DRAW_ARB:\n+\t\tif (th08_stream_half[0] != NULL) {", p)
        self.assertIn("+\t\telse if (!(data->flags & BF_UNMANAGED))\n \t\t\tfree(data->base);", p)
        self.assertIn("buf->flags = require_upper ? BF_PINNED_FIXED : (stream_arena ? BF_UNMANAGED : 0);", p)
        self.assertIn("th08_stream_overflow_count++;", p)

    def test_compat_wiring(self) -> None:
        c = self.compat
        self.assertIn('#include "pspgl_stream_arena.hpp"', c)
        self.assertNotIn("TH08_PSP_GE_FRAME_PARITY_ENABLED", c)
        self.assertIn("constexpr size_t kPspglStreamArenaLeaseBytes =\n    kPspglStreamArenaHalfBytes;", c)
        self.assertIn('kPspglStreamArenaLeaseBytes, 64U, "pspgl stream frame lease"', c)
        self.assertIn("__pspgl_th08_stream_arena_install(\n                pspStreamArenaLease, kPspglStreamArenaHalfBytes) == 0", c)
        begin = function_body(c, "HRESULT BeginScene()")
        self.assertIn("#if TH08_PSP_SWAP_TRIPLE_ENABLED\n            // Triple buffering removed the GE sync from Present", begin)
        self.assertIn("BeginPspglStreamArenaFrame();", begin)
        self.assertIn("__pspgl_th08_stream_arena_begin_frame(0U);", c)
        self.assertNotIn('directGeArenaBytes * 2U, 64U, "bullet direct GE vertices x2"));', c)
        self.assertIn("th08::psp::RenderResourceArenaTryFree(\n                    pspBulletDirectGeVerticesBase);", c)
        present = function_body(c, "HRESULT Present(const RECT *, const RECT *, HWND, const RGNDATA *)")
        self.assertIn("presentCount == 1UL || (presentCount % 600UL) == 0UL", present)
        release = function_body(c, "void ReleasePspglStreamArenaFrame(bool report)")
        self.assertIn("__pspgl_th08_stream_arena_stats(&allocs, &overflows, &peakBytes);", release)
        self.assertIn("__pspgl_th08_stream_arena_install(NULL, 0U);", release)
        self.assertIn("RenderResourceArenaTryFree(pspStreamArenaLease);", release)
        self.assertIn("RenderResourceArenaFreeResult::NotOwned", release)
        self.assertIn("pspStreamArenaOrphanAddress", release)
        self.assertIn("pspStreamArenaPresent = ~0UL;", release)


if __name__ == "__main__":
    unittest.main()
