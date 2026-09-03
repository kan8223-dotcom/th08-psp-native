#!/usr/bin/env python3
"""Summarize TH08 PSP DRAW_PRIO_GE V1 records (GE submissions per draw priority).

The draw-priority sub-profile samples one presented frame in sixteen; on those
frames the D3D8 compat layer's submission counter is attributed to the running
draw-chain callback.  Each record gives, per parent PERF_ATTR window,
``p<N>=draws/vertices`` for the sampled frames, so draws per present is
``draws / sampled``.  Direct-GE bullet submissions bypass the compat layer and
are not counted.
"""

from __future__ import annotations

import argparse
import pathlib
import sys

RECORD_PREFIX = "DRAW_PRIO_GE V1 "
PRIORITY_NAMES = {
    0: "Supervisor", 2: "Supervisor loading VMs", 3: "MusicRoom/TitleScreen", 4: "Ending",
    5: "GameManager", 6: "Background high", 7: "Background low (incl. Effect background)",
    8: "Enemy high", 9: "Player high", 10: "Player low", 11: "Enemy low", 12: "EffectManager",
    13: "BulletManager", 14: "Ascii high", 15: "Spellcard", 16: "FPS counter", 17: "GUI",
    18: "ResultScreen", 20: "Ascii low",
}


def parse_fields(body: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    for token in body.split():
        if "=" in token:
            key, value = token.split("=", 1)
            fields[key] = value
    return fields


def summarize(text: str) -> list[dict]:
    out = []
    for number, raw in enumerate(text.splitlines(), start=1):
        line = raw.strip()
        if not line.startswith(RECORD_PREFIX):
            continue
        f = parse_fields(line[len(RECORD_PREFIX):])
        try:
            first, last = (int(x) for x in f["sf"].split("-"))
            sampled = int(f["sampled"])
            chain_draws, chain_verts = (int(x) for x in f["chain"].split("/"))
            bins = {}
            waits = {}
            for key, value in f.items():
                if key.startswith("p") and key[1:].isdigit() or key == "po":
                    parts = [int(x) for x in value.split("/")]
                    if len(parts) not in (2, 4):
                        raise ValueError(f"{key} has {len(parts)} fields")
                    bins[key] = (parts[0], parts[1])
                    if len(parts) == 4:
                        waits[key] = (parts[2], parts[3])
            gw_frames = int(f["gwf"]) if "gwf" in f else None
            gw_total = tuple(int(x) for x in f["gwc"].split("/")) if "gwc" in f else None
        except (KeyError, ValueError) as error:
            raise ValueError(f"line {number}: malformed DRAW_PRIO_GE record ({error})") from error
        out.append({"line": number, "stage": int(f["st"]), "sf_first": first, "sf_last": last,
                    "sampled": sampled, "chain_draws": chain_draws, "chain_vertices": chain_verts,
                    "bins": bins, "waits": waits, "gw_frames": gw_frames, "gw_total": gw_total})
    return out


def render(records: list[dict]) -> str:
    lines = []
    for r in records:
        s = max(1, r["sampled"])
        lines.append(f"DRAW_PRIO_GE line {r['line']}: stage={r['stage']} sf={r['sf_first']}-{r['sf_last']} "
                     f"sampled={r['sampled']} chain per present: draws={r['chain_draws'] / s:.1f} "
                     f"vertices={r['chain_vertices'] / s:.0f}")
        if r["gw_total"] is not None:
            frames = max(1, r["gw_frames"] or 1)
            lines.append(f"  unscoped GE waits inside the draw chain (all {r['gw_frames']} presented frames): "
                         f"{r['gw_total'][0] / 1000.0:.3f} ms total, {r['gw_total'][1]} waits, "
                         f"{r['gw_total'][0] / frames / 1000.0:.3f} ms/present")
        ranked = sorted(r["bins"].items(), key=lambda kv: kv[1][0], reverse=True)
        for key, (draws, verts) in ranked:
            wait = r["waits"].get(key)
            if draws == 0 and not (wait and wait[1]):
                continue
            name = PRIORITY_NAMES.get(int(key[1:]), "other") if key != "po" else "other"
            wait_text = ""
            if wait is not None and r["gw_frames"]:
                wait_text = f"  gewait/present={wait[0] / max(1, r['gw_frames']) / 1000.0:6.3f}ms ({wait[1]} waits)"
            lines.append(f"  {key:<4s}{name:<44s} draws/present={draws / s:7.1f}  vertices/present={verts / s:8.0f}  "
                         f"vertices/draw={verts / draws if draws else 0.0:6.1f}{wait_text}")
    if records:
        total_s = sum(max(1, r["sampled"]) for r in records)
        agg: dict[str, list[int]] = {}
        for r in records:
            for key, (d, v) in r["bins"].items():
                agg.setdefault(key, [0, 0])
                agg[key][0] += d
                agg[key][1] += v
        lines.append("all windows, per present: " + ", ".join(
            f"{k}={v[0] / total_s:.1f}/{v[1] / total_s:.0f}" for k, v in sorted(agg.items(), key=lambda kv: kv[1][0], reverse=True) if v[0]))
    return "\n".join(lines)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("log", type=pathlib.Path)
    args = parser.parse_args(argv)
    print(render(summarize(args.log.read_text(encoding="utf-8", errors="replace"))))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
