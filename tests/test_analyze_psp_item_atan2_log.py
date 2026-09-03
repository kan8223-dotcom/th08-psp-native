#!/usr/bin/env python3
"""Unit tests for tools/analyze_psp_item_atan2_log.py."""

from __future__ import annotations

import pathlib
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import analyze_psp_item_atan2_log as analyzer  # noqa: E402

CONFIG = "PERF_ATTR_CONFIG V1 enabled=1 period_stage_frames=600 clock=sceKernelGetSystemTimeWide cal_reads=1024 cal_us=1024 runtime_telemetry_independent=1 log_flush_per_sample=0\n"
AUDIT = (
    "ITEM_ATAN2 V1 st=5 sf=1-601 mode=audit calls=17520 accepted=17500 mismatch=0 "
    "r_zero=2 r_nonfinite=0 r_range=0 r_tiny=15 r_boundary=3 delta_repeat=876 "
    "angle_repeat=4380 invalid_slot=0 sampled=547 t_canon=32820/120/547 "
    "t_fast=1641/9/547 t_vel=8205/40/547 timer_reads=2188 cr=0 mm_recorded=0 "
    "mm0=00000000/00000000/00000000/00000000 mm1=00000000/00000000/00000000/00000000 "
    "mm2=00000000/00000000/00000000/00000000 mm3=00000000/00000000/00000000/00000000\n"
)


class AnalyzeItemAtan2LogTests(unittest.TestCase):
    def test_audit_summary(self) -> None:
        s = analyzer.summarize(CONFIG + AUDIT)
        self.assertEqual(len(s), 1)
        w = s[0]
        self.assertEqual(w["mode"], "audit")
        self.assertEqual(w["calls"], 17520)
        self.assertAlmostEqual(w["accept_rate"], 17500 / 17520)
        self.assertEqual(w["mismatch"], 0)
        self.assertEqual(w["reasons"]["r_tiny"], 15)
        self.assertAlmostEqual(w["delta_repeat_rate"], 876 / 17520)
        self.assertAlmostEqual(w["angle_repeat_rate"], 0.25)
        self.assertAlmostEqual(w["timing"]["t_canon"]["avg_us"], 60.0)
        self.assertAlmostEqual(w["timing"]["t_fast"]["avg_us"], 3.0)
        self.assertAlmostEqual(w["timing"]["t_vel"]["avg_us"], 15.0)
        self.assertEqual(w["timer_overhead_us_per_read"], 1.0)
        rendered = analyzer.render(s)
        self.assertIn("20.0x", rendered)
        self.assertIn("mismatch=0", rendered)

    def test_mismatch_records_are_rendered_and_exit_code(self) -> None:
        bad = AUDIT.replace("mismatch=0", "mismatch=2").replace(
            "mm_recorded=0", "mm_recorded=2"
        ).replace("mm0=00000000/00000000/00000000/00000000", "mm0=3f800000/40000000/3eed6338/3eed6339")
        s = analyzer.summarize(bad)
        self.assertEqual(s[0]["mismatch"], 2)
        self.assertEqual(s[0]["mismatch_records"][0], "3f800000/40000000/3eed6338/3eed6339")
        self.assertIn("!! line", analyzer.render(s))

    def test_product_mode_without_timing(self) -> None:
        prod = ("ITEM_ATAN2 V1 st=5 sf=1-601 mode=product calls=100 accepted=99 mismatch=0 "
                "r_zero=0 r_nonfinite=0 r_range=0 r_tiny=1 r_boundary=0 delta_repeat=0 angle_repeat=0 "
                "invalid_slot=0 sampled=0 t_canon=0/0/0 t_fast=0/0/0 t_vel=0/0/0 timer_reads=0 cr=0 mm_recorded=0\n")
        s = analyzer.summarize(prod)
        self.assertEqual(s[0]["mode"], "product")
        self.assertAlmostEqual(s[0]["fallback_rate"], 0.01)
        self.assertIn("product", analyzer.render(s))

    def test_malformed_triple(self) -> None:
        with self.assertRaises(ValueError):
            analyzer.summarize(AUDIT.replace("t_fast=1641/9/547", "t_fast=1641/9"))

    def test_no_records(self) -> None:
        self.assertEqual(analyzer.summarize(CONFIG), [])
        self.assertIn("no fast-path records", analyzer.render([]))

    def test_sincos_prefix(self) -> None:
        line = ("ITEM_SINCOS V1 st=5 sf=1-601 mode=audit calls=1000 accepted=998 mismatch=0 "
                "r_zero=0 r_nonfinite=0 r_range=0 r_tiny=1 r_boundary=1 sampled=31 "
                "t_canon=930/40/31 t_fast=155/6/31 timer_reads=93 cr=0 mm_recorded=0 "
                "mm0=00000000/00000000/00000000/00000000/00000000/00000000 "
                "mm1=00000000/00000000/00000000/00000000/00000000/00000000\n")
        s = analyzer.summarize(line, "ITEM_SINCOS V1 ")
        self.assertEqual(len(s), 1)
        self.assertEqual(s[0]["calls"], 1000)
        self.assertIsNone(s[0]["delta_repeat_rate"])
        self.assertAlmostEqual(s[0]["timing"]["t_canon"]["avg_us"], 30.0)
        self.assertNotIn("t_vel", s[0]["timing"])
        self.assertEqual(analyzer.summarize(line), [])
        self.assertIn("6.0x", analyzer.render(s))


if __name__ == "__main__":
    unittest.main()
