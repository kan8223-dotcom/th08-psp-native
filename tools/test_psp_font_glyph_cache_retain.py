#!/usr/bin/env python3
"""Host-model and source-contract gates for PSP glyph-cache retention."""

from __future__ import annotations

import pathlib
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
HEADER = ROOT / "psp" / "font_glyph_cache_policy.hpp"
HARNESS = ROOT / "tools" / "psp_font_glyph_cache_policy_harness.cpp"
COMPAT = ROOT / "src" / "modern" / "linux" / "linux_compat.cpp"
MAIN = ROOT / "psp" / "main.cpp"
MAKEFILE = ROOT / "Makefile.psp"


class PspFontGlyphCacheRetainTests(unittest.TestCase):
    def test_policy_model(self) -> None:
        with tempfile.TemporaryDirectory(prefix="th08-font-cache-") as temp:
            binary = pathlib.Path(temp) / "font-cache-policy"
            subprocess.run(
                [
                    "g++",
                    "-std=c++17",
                    "-O2",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(ROOT),
                    str(HARNESS),
                    "-o",
                    str(binary),
                ],
                check=True,
                cwd=ROOT,
            )
            result = subprocess.run(
                [str(binary)], check=True, text=True, capture_output=True
            )
            self.assertIn("font-glyph-cache-policy: PASS", result.stdout)

    def test_build_flag_is_default_off_and_stamped(self) -> None:
        makefile = MAKEFILE.read_text(encoding="utf-8")
        self.assertIn("TH08_PSP_FONT_GLYPH_CACHE_RETAIN ?= 0", makefile)
        self.assertIn("-DTH08_PSP_FONT_GLYPH_CACHE_RETAIN=1", makefile)
        self.assertIn("-DTH08_PSP_FONT_GLYPH_CACHE_RETAIN=0", makefile)
        self.assertIn(
            "TH08_PSP_FONT_GLYPH_CACHE_RETAIN must be 0 or 1", makefile
        )
        self.assertIn("font-glyph-cache-retain-0.stamp", makefile)
        self.assertIn("font-glyph-cache-retain-1.stamp", makefile)
        self.assertIn("$(FONT_GLYPH_CACHE_RETAIN_CONFIG_STAMP)", makefile)

    def test_release_retains_only_exact_runtime_key(self) -> None:
        source = COMPAT.read_text(encoding="utf-8")
        start = source.index("void ReleasePspGdiFontDescriptor")
        end = source.index("void ShutdownPspGdiText", start)
        release = source[start:end]
        policy_call = release.index("ShouldRetainPspFontGlyphCache")
        owner_clear = release.index("font->font = NULL", policy_call)
        self.assertLess(policy_call, owner_clear)
        self.assertIn("g_pspGdiText.hardwareModel", release)
        self.assertIn("g_pspGdiText.currentPointSize, font->font, font->pointSize", release)
        self.assertIn("g_pspGdiText.currentPointSize > 0 && !retainGlyphCache", release)
        self.assertIn("TTF_SetFontSize(g_pspGdiText.face", release)

    def test_size_owner_and_teardown_contracts_remain_fail_safe(self) -> None:
        source = COMPAT.read_text(encoding="utf-8")
        configure_start = source.index("bool ConfigurePspGdiFont")
        release_start = source.index("void ReleasePspGdiFontDescriptor")
        configure = source[configure_start:release_start]
        shutdown_start = source.index("void ShutdownPspGdiText")
        shutdown_end = source.index("void PutGdiTextPixel", shutdown_start)
        shutdown = source[shutdown_start:shutdown_end]
        self.assertIn("g_pspGdiText.currentPointSize != font->pointSize", configure)
        self.assertIn("font->font != g_pspGdiText.face", configure)
        self.assertIn("TTF_SetFontSize(g_pspGdiText.face, font->pointSize)", configure)
        self.assertIn("g_pspGdiText.liveDescriptors != 0", shutdown)
        self.assertIn("TTF_CloseFont(g_pspGdiText.face)", shutdown)
        self.assertIn("g_pspGdiText = {};", shutdown)

    def test_model_query_is_opt_in_and_fail_safe(self) -> None:
        source = COMPAT.read_text(encoding="utf-8")
        query = source.index("const int hardwareModel = kuKernelGetModel();")
        guard = source.rfind("#if TH08_PSP_FONT_GLYPH_CACHE_RETAIN", 0, query)
        fallback = source.index("const int hardwareModel = -1;", query)
        endif = source.index("#endif", fallback)
        self.assertGreaterEqual(guard, 0)
        self.assertLess(query, fallback)
        self.assertLess(fallback, endif)
        header = HEADER.read_text(encoding="utf-8")
        self.assertIn("return hardwareModel > 0;", header)
        self.assertIn("runtimeOwner == descriptorOwner", header)
        self.assertIn("runtimePointSize == descriptorPointSize", header)

    def test_visual_text_and_cp932_paths_are_unchanged(self) -> None:
        source = COMPAT.read_text(encoding="utf-8")
        text_out = source[source.index("BOOL TextOutA") :]
        self.assertIn("ConfigurePspGdiFont(dc->font)", text_out)
        self.assertIn("ConvertCp932ToUtf8", text_out)
        self.assertIn("TTF_RenderUTF8_Blended", text_out)
        self.assertIn("PutGdiTextPixel", text_out)
        main = MAIN.read_text(encoding="utf-8")
        self.assertIn("FONT_GLYPH_CACHE_RETAIN=%d", main)
        self.assertIn("TH08_PSP_FEATURE_FONT_GLYPH_CACHE_RETAIN", main)


if __name__ == "__main__":
    unittest.main()
