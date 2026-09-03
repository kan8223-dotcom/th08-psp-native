#!/usr/bin/env python3
"""Unit tests for tools/analyze_psp_trig_df_log.py."""

from __future__ import annotations

import pathlib
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import analyze_psp_trig_df_log as analyzer  # noqa: E402

LINE = ("TRIG_DF V1 st=5 sf=1-601 mode=audit fa=6000/5999/0 rv=0/0/0 sm=120/120/0 su=60/60/0 at=9000/8998/0 "
        "sc_zero=0 sc_nonfinite=0 sc_range=0 sc_tiny=0 sc_boundary=1 at_zero=1 at_nonfinite=0 at_range=0 at_tiny=0 at_boundary=1 "
        "mm_recorded=0 mm0=0/00000000/00000000/00000000/00000000/00000000/00000000 mm1=0/00000000/00000000/00000000/00000000/00000000/00000000\n")


class AnalyzeTrigDfLogTests(unittest.TestCase):
    def test_pass(self) -> None:
        records = analyzer.summarize("x\n" + LINE)
        self.assertEqual(len(records), 1)
        r = records[0]
        self.assertEqual(r["calls"], 15180)
        self.assertEqual(r["sites"]["Atan2"]["accepted"], 8998)
        self.assertAlmostEqual(r["calls_per_tick"], 25.3)
        self.assertEqual(analyzer.gate(records)[0], "PASS")
        text = analyzer.render(records)
        self.assertIn("gate: PASS", text)
        self.assertIn("sc_boundary=1", text)

    def test_fail_and_modes(self) -> None:
        self.assertEqual(analyzer.gate(analyzer.summarize(LINE.replace("at=9000/8998/0", "at=9000/8998/2")))[0], "FAIL")
        self.assertEqual(analyzer.gate(analyzer.summarize(LINE.replace("mode=audit", "mode=product")))[0], "PRODUCT_ONLY")
        self.assertEqual(analyzer.gate([])[0], "NO_DATA")

    def test_malformed(self) -> None:
        with self.assertRaises(ValueError):
            analyzer.summarize(LINE.replace("rv=0/0/0 ", ""))


if __name__ == "__main__":
    unittest.main()
