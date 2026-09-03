#!/usr/bin/env python3
"""Unit tests for tools/analyze_psp_draw_prio_ge_log.py."""

from __future__ import annotations

import pathlib
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import analyze_psp_draw_prio_ge_log as analyzer  # noqa: E402

BINS = " ".join(f"p{i}=0/0" for i in range(22)) + " po=0/0"
LINE = ("DRAW_PRIO_GE V1 st=5 sf=1-601 sampled=12 chain=1800/21600 " + BINS.replace("p17=0/0", "p17=1200/14400").replace("p13=0/0", "p13=600/7200") + "\n")


BINS4 = " ".join(f"p{i}=0/0/0/0" for i in range(22)) + " po=0/0/0/0"
LINE4 = ("DRAW_PRIO_GE V1 st=5 sf=1-601 sampled=12 chain=1800/21600 gwf=200 gwc=40000/50 "
         + BINS4.replace("p17=0/0/0/0", "p17=1200/14400/30000/40").replace("p6=0/0/0/0", "p6=200/800/10000/10") + "\n")


class AnalyzeDrawPrioGeTests(unittest.TestCase):
    def test_four_field_bins_with_ge_waits(self) -> None:
        records = analyzer.summarize(LINE4)
        r = records[0]
        self.assertEqual(r["bins"]["p17"], (1200, 14400))
        self.assertEqual(r["waits"]["p17"], (30000, 40))
        self.assertEqual(r["gw_frames"], 200)
        self.assertEqual(r["gw_total"], (40000, 50))
        text = analyzer.render(records)
        self.assertIn("0.200 ms/present", text)
        self.assertIn("gewait/present= 0.150ms (40 waits)", text)

    def test_summary(self) -> None:
        records = analyzer.summarize("x\n" + LINE)
        self.assertEqual(len(records), 1)
        self.assertEqual(records[0]["bins"]["p17"], (1200, 14400))
        text = analyzer.render(records)
        self.assertIn("draws/present=  100.0", text)
        self.assertIn("GUI", text)
        self.assertIn("chain per present: draws=150.0", text)

    def test_malformed(self) -> None:
        with self.assertRaises(ValueError):
            analyzer.summarize(LINE.replace("sampled=12 ", ""))


if __name__ == "__main__":
    unittest.main()
