import importlib.util
import pathlib
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools" / "analyze_psp_draw_priority_log.py"
SPEC = importlib.util.spec_from_file_location("draw_priority_analyzer", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


def record_line(*, sampled=2, presented=32, uf="0x00", cr=0, ov=0):
    bins = []
    for priority in range(22):
        if priority == 17:
            bins.append("p17=4000/2500/2")
        elif priority == 7:
            bins.append("p7=2000/1200/2")
        else:
            bins.append(f"p{priority}=0/0/0")
    bins.append("po=0/0/0")
    return (
        "DRAW_PRIO V1 st=5 sf=1-601 presented="
        f"{presented} cadence_mode=2 sampled={sampled} "
        "chain=7000/4000/2 cb=6000/4 residual=1000 "
        "effect_bg=500/300/2 p7x=1500 timer_reads=20 "
        f"cr={cr} ov={ov} uf={uf} " + " ".join(bins)
    )


class DrawPriorityAnalyzerTests(unittest.TestCase):
    def test_parse_rank_projection_and_parent_crosscheck(self):
        text = "\n".join(
            (
                "BUILD id=r087-test",
                "SYSTEM memory cpu_clock_mhz=333 clock_cpu=333 clock_bus=166",
                "PERF_ATTR V1 st=5 sf=1-601 cadence_mode=2 drawc=120000/1/32 do=80000",
                record_line(),
            )
        )
        records = MODULE.parse_records(text)
        self.assertEqual(len(records), 1)
        self.assertEqual(MODULE.invalid_record_lines(records), [])
        perf = MODULE.parse_perf_records(text)[("5", "1-601", "2")]
        summary = MODULE.summarize_record(records[0], perf)
        self.assertIn("p17 GUI", summary)
        self.assertIn("est=   64.000ms", summary)
        self.assertIn("parent_draw_other=80.000ms", summary)

    def test_invalid_counter_or_sample_count_is_rejected(self):
        records = MODULE.parse_records(record_line(sampled=3, cr=1))
        self.assertEqual(MODULE.invalid_record_lines(records), [1])

    def test_cli_reports_missing_records(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "empty.log"
            path.write_text("BUILD id=none\n", encoding="utf-8")
            old_argv = MODULE.sys.argv
            try:
                MODULE.sys.argv = [str(MODULE_PATH), str(path)]
                self.assertEqual(MODULE.main(), 2)
            finally:
                MODULE.sys.argv = old_argv


if __name__ == "__main__":
    unittest.main()
