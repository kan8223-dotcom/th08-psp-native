#!/usr/bin/env python3
"""Unit tests for tools/analyze_psp_softfloat_log.py."""

from __future__ import annotations

import pathlib
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import analyze_psp_softfloat_log as analyzer  # noqa: E402

PH = " ".join(f"ph{i}=0/0/0" for i in range(23))
LINE = ("SOFTFLOAT V1 st=5 sf=1-601 df_direct=60000 df_internal=120000 libm=600 libmf=1200 "
        + PH.replace("ph3=0/0/0", "ph3=30000/300/600").replace("ph7=0/0/0", "ph7=24000/300/0").replace("ph22=0/0/0", "ph22=6000/0/600")
        + " add=10000 sub=5000 mul=30000 div=1000 cmp=2000 ext=8000 trunc=3000 fix=500 flt=500 "
        "sin=200 cos=200 atan2=100 atan=0 sqrt=100 floor=0 fmod=0 other=0 sinf=600 cosf=600 atan2f=0 sqrtf=0 floorf=0 fmodf=0\n")


class AnalyzeSoftfloatLogTests(unittest.TestCase):
    def test_summary(self) -> None:
        s = analyzer.summarize(LINE)
        self.assertEqual(len(s), 1)
        w = s[0]
        self.assertEqual(w["df_direct"], 60000)
        self.assertEqual(w["phases"]["PlayerUpdate"], (30000, 300, 600))
        self.assertEqual(w["phases"]["EffectUpdate"], (24000, 300, 0))
        self.assertEqual(w["phases"]["(no scope)"], (6000, 0, 600))
        self.assertEqual(w["ops"]["mul"], 30000)
        self.assertGreater(w["est_ms_per_tick_at_333"], 0.0)
        rendered = analyzer.render(s)
        self.assertIn("PlayerUpdate 50/0.5/1.0", rendered)
        self.assertIn("mul=50.0", rendered)

    def test_malformed_phase(self) -> None:
        with self.assertRaises(ValueError):
            analyzer.summarize(LINE.replace("ph3=30000/300/600", "ph3=30000/300"))

    def test_no_records(self) -> None:
        self.assertEqual(analyzer.summarize("PERF_ATTR V1 st=5\n"), [])
        self.assertIn("no SOFTFLOAT", analyzer.render([]))


if __name__ == "__main__":
    unittest.main()
