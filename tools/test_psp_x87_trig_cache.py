#!/usr/bin/env python3
"""Exact-value and integration gates for the PSP x87 trig memoization."""

from __future__ import annotations

import pathlib
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
HEADER = ROOT / "psp" / "x87_trig_cache.hpp"
SOURCE = ROOT / "psp" / "x87_trig_cache.cpp"
HARNESS = ROOT / "tools" / "psp_x87_trig_cache_harness.cpp"
ZUN_MATH = ROOT / "src" / "ZunMath.hpp"
MAKEFILE = ROOT / "Makefile.psp"
MAIN = ROOT / "psp" / "main.cpp"
TELEMETRY = ROOT / "psp" / "memory_telemetry.cpp"
RADIAL_TRIG = ROOT / "psp" / "radial_trig_reuse.hpp"


class X87TrigCacheTests(unittest.TestCase):
    def test_exact_harness(self) -> None:
        with tempfile.TemporaryDirectory(prefix="th08-x87-cache-") as temp:
            binary = pathlib.Path(temp) / "x87-cache"
            subprocess.run(
                [
                    "g++",
                    "-std=c++17",
                    "-O2",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-DPSP=1",
                    "-DTH08_MODERN_PORT=1",
                    "-DTH08_PSP_X87_TRIG_CACHE=1",
                    "-I",
                    str(ROOT),
                    "-I",
                    str(ROOT / "psp"),
                    str(SOURCE),
                    str(HARNESS),
                    "-lm",
                    "-o",
                    str(binary),
                ],
                check=True,
                cwd=ROOT,
            )
            result = subprocess.run(
                [str(binary)], check=True, text=True, capture_output=True
            )
            self.assertIn("x87-trig-cache: PASS", result.stdout)

    def test_disabled_object_has_no_cache_storage(self) -> None:
        with tempfile.TemporaryDirectory(prefix="th08-x87-cache-off-") as temp:
            obj = pathlib.Path(temp) / "x87-cache-off.o"
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
                    "-c",
                    str(SOURCE),
                    "-o",
                    str(obj),
                ],
                check=True,
                cwd=ROOT,
            )
            symbols = subprocess.run(
                ["nm", "-S", str(obj)],
                check=True,
                text=True,
                capture_output=True,
            ).stdout
            self.assertNotIn("gEntries", symbols)
            self.assertNotIn("gStats", symbols)
            self.assertNotIn("X87TrigCacheSin64", symbols)

    def test_cache_is_fixed_and_canonical_on_miss(self) -> None:
        text = SOURCE.read_text(encoding="utf-8")
        self.assertNotIn("malloc", text)
        self.assertNotIn("new ", text)
        self.assertNotIn("vfpu", text.lower())
        self.assertNotIn("sce", text)
        self.assertIn("std::sin(static_cast<double>(angle))", text)
        self.assertIn("std::cos(static_cast<double>(angle))", text)
        self.assertIn("gEntries[kX87TrigCacheEntryCount]", text)

    def test_public_contract_documents_collision_safety(self) -> None:
        text = HEADER.read_text(encoding="utf-8")
        self.assertIn("Collisions only cause a canonical libm", text)
        self.assertIn("kX87TrigCacheEntryCount = 512U", text)
        self.assertIn("X87TrigCacheStorageBytes", text)
        self.assertIn("X87TrigCacheTake", text)

    def test_gameplay_wrappers_keep_final_rounding_boundary(self) -> None:
        text = ZUN_MATH.read_text(encoding="utf-8")
        self.assertIn("return psp::X87TrigCacheSin64(angle);", text)
        self.assertIn("return psp::X87TrigCacheCos64(angle);", text)
        self.assertIn(
            "X87CompatibleSin64(angle) * static_cast<f64>(magnitude) +",
            text,
        )
        self.assertIn(
            "X87CompatibleCos64(angle) * static_cast<f64>(magnitude) *",
            text,
        )
        # No wrapper may round the cached binary64 value to f32 before its
        # retail multiply/add chain.
        self.assertNotIn(
            "X87CompatibleSin(angle) * static_cast<f64>(magnitude)", text
        )
        self.assertNotIn(
            "X87CompatibleCos(angle) * static_cast<f64>(magnitude)", text
        )

    def test_psp_build_and_single_owner_telemetry_contract(self) -> None:
        makefile = MAKEFILE.read_text(encoding="utf-8")
        main = MAIN.read_text(encoding="utf-8")
        telemetry = TELEMETRY.read_text(encoding="utf-8")
        radial = RADIAL_TRIG.read_text(encoding="utf-8")
        header = HEADER.read_text(encoding="utf-8")
        self.assertIn("psp/x87_trig_cache.cpp", makefile)
        self.assertIn("TH08_PSP_X87_TRIG_CACHE ?= 0", makefile)
        self.assertIn("x87-trig-cache-0.stamp", makefile)
        self.assertIn("x87-trig-cache-1.stamp", makefile)
        self.assertIn("-DTH08_PSP_X87_TRIG_CACHE=1", makefile)
        self.assertIn("$(X87_TRIG_CACHE_CONFIG_STAMP)", makefile)
        self.assertIn("X87_TRIG_CACHE=%d", main)
        self.assertIn("TH08_PSP_FEATURE_X87_TRIG_CACHE", main)
        self.assertIn("owner=game_frame_thread marks_peek=0", telemetry)
        self.assertNotIn("X87TrigCachePeek()", telemetry)
        self.assertIn("Ownership is the PSP game-frame thread", header)
        self.assertIn("return X87TrigCacheSin64(angle);", radial)
        self.assertIn("return X87TrigCacheCos64(angle);", radial)


if __name__ == "__main__":
    unittest.main()
