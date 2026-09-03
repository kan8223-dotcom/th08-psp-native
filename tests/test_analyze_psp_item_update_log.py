#!/usr/bin/env python3
"""Unit tests for tools/analyze_psp_item_update_log.py."""

from __future__ import annotations

import pathlib
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import analyze_psp_item_update_log as analyzer  # noqa: E402


CONFIG = (
    "PERF_ATTR_CONFIG V1 enabled=1 period_stage_frames=600 "
    "clock=sceKernelGetSystemTimeWide cal_reads=1024 cal_us=1024 "
    "runtime_telemetry_independent=1 log_flush_per_sample=0\n"
)
PERF = (
    "PERF_ATTR V1 st=5 sf=1-601 sim_frames=600 sim_hz=60 rendered_frames=200 "
    "render_target_fps=20 cadence_mode=2 replay=1 demo=0 wall=15000000 "
    "wo=1 tc=1024/1024 tr=1 te=1 cr=0 calc=1/1/600 pu=1/1/600 eu=1/1/600 "
    "fxu=1/1/600 bui=1/1/600 iu=1200000/9000/600 bux=1 co=1 "
    "drawf=1/1/200 drawc=1/1/200 dfo=1 uf=0x00\n"
)
ITEM = (
    "ITEM_UPD V1 st=5 sf=1-601 ticks=600 rot=32 "
    "sample_rule=list_position_rotating_1of32 items=120000/400/600 "
    "sampled=3750 timer_reads=20480 cr=0 ov=0 uf=0x00 "
    "whole=37500/90/3750 auto=10000/20/2000 coll=5000/10/3600 "
    "script=7500/30/3000 resid=15000 "
    "c_visit=120000 c_auto=64000 c_coll=115200 c_pick=600 c_off=60 "
    "c_script=96000 s_default=60000 s_auto=48000 s_spread=6000 "
    "s_rising=3000 s_apex=3000 s_other=0\n"
)


class AnalyzeItemUpdateLogTests(unittest.TestCase):
    def test_window_summary_numbers(self) -> None:
        summaries = analyzer.summarize(CONFIG + PERF + ITEM)
        self.assertEqual(len(summaries), 1)
        s = summaries[0]
        self.assertEqual(s["stage"], 5)
        self.assertEqual(s["sf"], "1-601")
        self.assertEqual(s["ticks"], 600)
        self.assertAlmostEqual(s["items_avg"], 200.0)
        self.assertEqual(s["items_max"], 400)
        self.assertEqual(s["sampled"], 3750)
        self.assertAlmostEqual(s["whole_avg_us"], 10.0)
        self.assertAlmostEqual(s["projected_ms_per_tick"], 2.0)
        self.assertAlmostEqual(s["iu_ms_per_tick"], 2.0)
        self.assertAlmostEqual(s["iu_max_ms"], 9.0)
        # 20480 reads x 1024us/1024 = 20480us over 600 ticks.
        self.assertAlmostEqual(s["overhead_ms_per_tick"], 20480 / 600 / 1000.0)
        self.assertAlmostEqual(s["sections"]["auto"]["share_of_whole"], 10000 / 37500)
        self.assertAlmostEqual(s["sections"]["auto"]["avg_us_per_call"], 5.0)
        self.assertAlmostEqual(s["sections"]["coll"]["avg_us_per_call"], 5000 / 3600)
        self.assertAlmostEqual(s["sections"]["script"]["avg_us_per_call"], 2.5)
        self.assertAlmostEqual(s["residual_share"], 15000 / 37500)
        self.assertAlmostEqual(s["residual_us_per_item"], 4.0)
        self.assertAlmostEqual(s["counts_per_tick"]["c_auto"], 64000 / 600)
        self.assertAlmostEqual(s["counts_per_tick"]["c_pick"], 1.0)
        self.assertAlmostEqual(s["state_share"]["s_auto"], 0.4)

    def test_missing_parent_window_and_calibration(self) -> None:
        summaries = analyzer.summarize(ITEM)
        self.assertEqual(len(summaries), 1)
        self.assertIsNone(summaries[0]["iu_ms_per_tick"])
        self.assertIsNone(summaries[0]["overhead_ms_per_tick"])
        rendered = analyzer.render(summaries)
        self.assertIn("n/a", rendered)
        self.assertIn("st5 sf=1-601", rendered)

    def test_malformed_triple_is_rejected(self) -> None:
        broken = ITEM.replace("whole=37500/90/3750", "whole=37500/90")
        with self.assertRaises(ValueError):
            analyzer.summarize(broken)

    def test_zero_sample_window_does_not_divide_by_zero(self) -> None:
        empty = ITEM.replace("sampled=3750", "sampled=0").replace(
            "whole=37500/90/3750", "whole=0/0/0"
        ).replace("items=120000/400/600", "items=0/0/600")
        summaries = analyzer.summarize(empty)
        self.assertEqual(summaries[0]["whole_avg_us"], 0.0)
        self.assertEqual(summaries[0]["projected_ms_per_tick"], 0.0)
        self.assertIn("st5", analyzer.render(summaries))

    def test_anomaly_flags_are_rendered(self) -> None:
        flagged = ITEM.replace("cr=0 ov=0 uf=0x00", "cr=2 ov=0 uf=0x01")
        rendered = analyzer.render(analyzer.summarize(flagged))
        self.assertIn("!! line", rendered)
        self.assertIn("cr=2", rendered)

    def test_no_records(self) -> None:
        self.assertEqual(analyzer.summarize(CONFIG + PERF), [])
        self.assertIn("no ITEM_UPD V1 records", analyzer.render([]))


if __name__ == "__main__":
    unittest.main()
