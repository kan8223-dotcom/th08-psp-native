#!/usr/bin/env python3
"""Gate r231 presentation-only VFPU logs; SIM_CHECK is not a full-state audit."""
import argparse
import json
import re
from pathlib import Path


def fields(line):
    return dict(re.findall(r"(\w+)=([^\s]+)", line))


def read_log(path):
    lines = Path(path).read_text(errors="replace").splitlines()
    sim = {}
    for line in lines:
        if line.startswith("SIM_CHECK "):
            f = fields(line)
            key = int(f["calls"])
            assert key not in sim, f"{path}: duplicate checkpoint/session"
            sim[key] = (int(f["bullets"]), f["rng"], int(f["score"]))
    return lines, sim


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("control")
    p.add_argument("audit")
    p.add_argument("candidates", nargs="*")
    p.add_argument("--min-calls", type=int, default=6000)
    a = p.parse_args()
    _, control = read_log(a.control)
    expected = set(range(600, a.min_calls + 1, 600))
    assert expected <= control.keys(), "control lacks required checkpoints"
    report = {"control": a.control, "checks": []}
    for path in [a.audit] + a.candidates:
        lines, sim = read_log(path)
        assert expected <= sim.keys(), f"{path}: missing required checkpoints"
        shared = sorted(control.keys() & sim.keys())
        assert all(control[k] == sim[k] for k in shared), f"{path}: SIM_CHECK mismatch"
        record = {"path": path, "matched_checkpoints": len(shared), "last_calls": max(shared)}
        stats = [fields(s) for s in lines if s.startswith("QUAD_VFPU stats ")]
        if stats:
            assert all(int(s["mismatch"]) == 0 for s in stats), f"{path}: vertex mismatch"
            for s in stats:
                assert int(s["calls"]) == int(s["accepted"]) + int(s["fallback"])
            record["quad_last"] = stats[-1]
        if path == a.audit:
            tests = [fields(s) for s in lines if s.startswith("QUAD_VFPU selftest ")]
            assert len(tests) == 1 and int(tests[0]["compared"]) >= 12288
            assert int(tests[0]["mismatch"]) == 0, "selftest mismatch"
            assert stats and int(stats[-1]["compared"]) >= 100000, "insufficient vertex audit"
            assert all(s["accepted"] == s["compared"] for s in stats), "audit fallback mismatch"
            record["selftest"] = tests[0]
        phases = [fields(s) for s in lines if s.startswith("BULLET_UPDATE_SUB ")]
        assert len(phases) >= a.min_calls // 600, f"{path}: missing phase records"
        record["phase_records"] = len(phases)
        report["checks"].append(record)
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
