import importlib.util
import pathlib
import sys
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "analyze_psp_perf_attr_log",
    ROOT / "tools" / "analyze_psp_perf_attr_log.py",
)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


def log(cpu: int, wall: int, sf: str = "1-601", build: str = "r085") -> str:
    return "\n".join(
        (
            f"BUILD id={build}",
            f"SYSTEM clock_cpu={cpu} clock_bus={cpu // 2}",
            "RENDER_CADENCE init_mode=2 mode=2",
            "PERF_ATTR V1 st=5 "
            f"sf={sf} sim_frames=600 rendered_frames=200 "
            f"cadence_mode=2 replay=1 demo=0 wall={wall} wo=100 "
            "calc=400/4/600 pu=10/1/600 eu=20/1/600 "
            "fxu=30/1/600 bux=40 iu=50 co=60 "
            "drawf=200/2/200 drawc=180/2/200 pd=5/1/400 "
            "ed=6/1/400 fxd=7/3/2/2 bd_i=8/1/200 id=9/1/200 "
            "bdx=10 do=11 pres=300/3/200 ge=70 vbs=80 vbc=90 "
            "cr=0 uf=0x00",
        )
    )


class PerfAttributionComparisonTests(unittest.TestCase):
    def test_same_windows_report_duration_ratio(self):
        baseline_text = log(333, 1_200)
        candidate_text = log(443, 900)
        summary = MODULE.summarize_comparison(
            MODULE.parse_records(baseline_text),
            MODULE.parse_records(candidate_text),
            MODULE.parse_identity(baseline_text),
            MODULE.parse_identity(candidate_text),
        )
        self.assertIn("ideal_cpu_duration_ratio=0.751693", summary)
        self.assertIn("wall_ratio=0.750000", summary)
        self.assertIn("speedup=1.333333x", summary)

    def test_different_window_is_rejected(self):
        baseline_text = log(333, 1_200)
        candidate_text = log(443, 900, sf="601-1201")
        with self.assertRaisesRegex(ValueError, "identity mismatch"):
            MODULE.summarize_comparison(
                MODULE.parse_records(baseline_text),
                MODULE.parse_records(candidate_text),
                MODULE.parse_identity(baseline_text),
                MODULE.parse_identity(candidate_text),
            )

    def test_different_build_is_rejected(self):
        baseline_text = log(333, 1_200)
        candidate_text = log(443, 900, build="other")
        with self.assertRaisesRegex(ValueError, "build mismatch"):
            MODULE.summarize_comparison(
                MODULE.parse_records(baseline_text),
                MODULE.parse_records(candidate_text),
                MODULE.parse_identity(baseline_text),
                MODULE.parse_identity(candidate_text),
            )


if __name__ == "__main__":
    unittest.main()
