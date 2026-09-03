#!/usr/bin/env python3
"""Unit tests for tools/analyze_psp_audio_cursor_log.py."""

from __future__ import annotations

import pathlib
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import analyze_psp_audio_cursor_log as analyzer  # noqa: E402

LINE = ("AUDIO_CURSOR V1 st=5 sf=1-601 mode=audit mixes=1200 eligible=1150 inelig_rate=50 "
        "inelig_cursor=0 frames=480000 mismatch=0 wrap_mismatch=0 cursor_mismatch=0 stop_mismatch=0 "
        "r44100=800 r22050=350 r11025=0 rother=50 max_frames=1024\n")


class AnalyzeAudioCursorLogTests(unittest.TestCase):
    def test_pass_gate(self) -> None:
        records = analyzer.summarize("noise\n" + LINE + LINE.replace("sf=1-601", "sf=601-1201"))
        self.assertEqual(len(records), 2)
        self.assertEqual(records[0]["frames"], 480000)
        self.assertAlmostEqual(records[0]["mixes_per_tick"], 2.0)
        self.assertAlmostEqual(records[0]["eligible_pct"], 1150 / 12)
        self.assertEqual(analyzer.gate(records)[0], "PASS")
        text = analyzer.render(records)
        self.assertIn("gate: PASS", text)
        self.assertIn("frames=960000", text)

    def test_fail_gate(self) -> None:
        records = analyzer.summarize(LINE.replace("cursor_mismatch=0", "cursor_mismatch=3"))
        self.assertEqual(records[0]["mismatch_total"], 3)
        self.assertEqual(analyzer.gate(records)[0], "FAIL")

    def test_product_only_and_no_data(self) -> None:
        self.assertEqual(analyzer.gate(analyzer.summarize(LINE.replace("mode=audit", "mode=product")))[0], "PRODUCT_ONLY")
        self.assertEqual(analyzer.gate([])[0], "NO_DATA")
        self.assertEqual(analyzer.gate(analyzer.summarize(LINE.replace("frames=480000", "frames=0")))[0], "EMPTY")

    def test_malformed(self) -> None:
        with self.assertRaises(ValueError):
            analyzer.summarize(LINE.replace("frames=480000 ", ""))
        with self.assertRaises(ValueError):
            analyzer.summarize(LINE.replace("mode=audit", "mode=shadow"))


if __name__ == "__main__":
    unittest.main()
