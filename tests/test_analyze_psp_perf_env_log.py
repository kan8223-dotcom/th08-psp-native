#!/usr/bin/env python3
"""Unit tests for tools/analyze_psp_perf_env_log.py."""

from __future__ import annotations

import pathlib
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import analyze_psp_perf_env_log as analyzer  # noqa: E402

GW = " ".join(f"gw{i}=0/0/0" for i in range(23))
LINE = ("PERF_ENV V1 st=5 sf=1-601 wall=10000000 clk0=333/166 clk1=333/166 recal_us=700 run_valid=1 run_us=7000000 "
        "intr_preempt=1200 thread_preempt=3000 fw=1000/50/200 mo0=12000/40/600 mo1=600/2/600 mo2=1800/5/600 mo3=3000/30/600 gw=60000/900/40 " + GW.replace("gw2=0/0/0", "gw2=60000/900/40") + "\n")
CONFIG = "PERF_ATTR_CONFIG V1 enabled=1 period_stage_frames=600 clock=sceKernelGetSystemTimeWide cal_reads=1024 cal_us=663 runtime_telemetry_independent=1 log_flush_per_sample=0\n"


class AnalyzePerfEnvTests(unittest.TestCase):
    def test_summary(self) -> None:
        boot_cal, records = analyzer.summarize(CONFIG + "x\n" + LINE)
        self.assertEqual(boot_cal, 663)
        self.assertEqual(len(records), 1)
        r = records[0]
        self.assertEqual(r["clk0"], (333, 166))
        self.assertAlmostEqual(r["run_share"], 0.7)
        self.assertEqual(r["phases"]["DrawChain"], (60000, 900, 40))
        text = analyzer.render(boot_cal, records)
        self.assertIn("DrawChain 0.100ms/40", text)
        self.assertIn("clock states seen: 333/166", text)
        self.assertIn("recal/boot cal ratio per window: 1.06", text)
        self.assertIn("SoundPlayer::ProcessQueues 0.020", text)
        self.assertIn("flip-guard waits, all windows: 0.002 ms/tick", text)

    def test_extended_fields(self) -> None:
        extended = LINE.replace("mo3=3000/30/600 ", "mo3=3000/30/600 mo4=600/3/700 mo5=100/2/100 mo6=1200/900/300 mo7=60/1/600 gq=9000/2400000/8700/600000/300/1500000/4200000/300 ")
        _, records = analyzer.summarize(extended)
        self.assertEqual(records[0]["mo"][6], (1200, 900, 300))
        self.assertEqual(records[0]["gq"][7], 300)
        text = analyzer.render(663, records)
        self.assertIn("post-present 0.002", text)
        self.assertIn("GE busy  13.00.. 14.00 ms/frame", text)
        self.assertIn("GE busy per frame, all windows: 13.00..14.00 ms", text)
        # Old records without the fields still parse.
        _, old = analyzer.summarize(LINE)
        self.assertEqual(old[0]["gq"], (0,) * 8)

    def test_malformed(self) -> None:
        with self.assertRaises(ValueError):
            analyzer.summarize(LINE.replace("gw7=0/0/0 ", ""))


if __name__ == "__main__":
    unittest.main()
