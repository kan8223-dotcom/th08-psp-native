#!/usr/bin/env python3
"""Compare native HUD vertex/VM streams and simulation checkpoints by frame."""
import argparse
import json
from pathlib import Path
import re


def read(path):
    text = Path(path).read_text(errors="replace")
    frames = {}
    for stage, frame, glyphs, digest in re.findall(
            r"HUD_STREAM st=(\d+) f=(\d+) call=\d+ glyphs=(\d+) hash=([0-9a-f]+)", text):
        if int(frame) == 0 or int(glyphs) == 0:
            continue
        key = (int(stage), int(frame))
        value = (int(glyphs), digest)
        if key in frames and frames[key] != value:
            raise ValueError(f"Conflicting duplicate {key} in {path}")
        frames[key] = value
    checkpoints = dict(re.findall(r"^(SIM_CHECK calls=\d+) (.+)$", text, re.M))
    stats = re.findall(r"^HUD_TEXT stats (.+)$", text, re.M)
    counters = {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", stats[-1])}
    return frames, checkpoints, counters


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("control")
    parser.add_argument("candidate")
    parser.add_argument("--min-frames", type=int, default=4800)
    args = parser.parse_args()
    off, sim_off, stats_off = read(args.control)
    on, sim_on, stats_on = read(args.candidate)
    common = sorted(off.keys() & on.keys())
    mismatches = [key for key in common if off[key] != on[key]]
    sim_common = sim_off.keys() & sim_on.keys()
    sim_mismatch = [key for key in sim_common if sim_off[key] != sim_on[key]]
    assert len(common) >= args.min_frames, f"Only {len(common)} comparable frames"
    assert not mismatches, f"HUD stream mismatch: {mismatches[:10]}"
    assert len(sim_common) >= 8, f"Need 8 SIM_CHECK checkpoints, found {len(sim_common)}"
    assert not sim_mismatch, f"SIM_CHECK mismatch: {sim_mismatch}"
    assert stats_off["fallback"] == stats_on["fallback"] == 0
    assert stats_off["merged"] == 0 and stats_on["merged"] > 0
    assert stats_on["submits"] < stats_off["submits"]
    print(json.dumps({
        "result": "PASS", "common_frames": len(common), "first": common[0], "last": common[-1],
        "compared_glyphs": sum(off[key][0] for key in common), "stream_mismatches": len(mismatches),
        "sim_checkpoints": len(sim_common), "sim_mismatches": len(sim_mismatch),
        "control": stats_off, "candidate": stats_on,
        "scope": "Accepted native HUD vertices and per-string VM fields; excludes FPS overlays and canonical fallbacks",
    }, indent=2))


if __name__ == "__main__":
    main()
