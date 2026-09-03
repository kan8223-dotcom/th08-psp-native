#!/usr/bin/env python3
"""Summarize TH08 PSP TRIG_DF V1 records (ZunMath trig double-float fast paths).

One record per parent PERF_ATTR window.  Sites: fa = Float3::FromAngleMagnitude,
rv = Float3::FromRotatedVec2 (two half-calls per invocation), sm =
X87CompatibleSinMul/CosMul, su = X87CompatibleSin/Cos, at = X87CompatibleAtan2;
each is calls/accepted/mismatch.  The promotion gate is mismatch == 0 for every
site in every audit window; the acceptance ratio says how much of the canonical
soft-double/libm work the product switch will remove.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import sys

RECORD_PREFIX = "TRIG_DF V1 "
SITES = (("fa", "FromAngleMagnitude"), ("rv", "FromRotatedVec2"), ("sm", "SinCosMul"),
         ("su", "SinCosUnit"), ("at", "Atan2"))
REASONS = ("sc_zero", "sc_nonfinite", "sc_range", "sc_tiny", "sc_boundary",
           "at_zero", "at_nonfinite", "at_range", "at_tiny", "at_boundary")


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
            record = {"line": number, "stage": int(f["st"]), "sf_first": first, "sf_last": last,
                      "mode": f["mode"], "sites": {}}
            for key, name in SITES:
                calls, accepted, mismatch = (int(x) for x in f[key].split("/"))
                record["sites"][name] = {"calls": calls, "accepted": accepted, "mismatch": mismatch}
            for key in REASONS:
                record[key] = int(f[key])
            record["mm_recorded"] = int(f["mm_recorded"])
        except (KeyError, ValueError) as error:
            raise ValueError(f"line {number}: malformed TRIG_DF record ({error})") from error
        if record["mode"] not in ("audit", "product"):
            raise ValueError(f"line {number}: unknown mode {record['mode']!r}")
        ticks = max(1, last - first)
        record["ticks"] = ticks
        record["calls"] = sum(s["calls"] for s in record["sites"].values())
        record["accepted"] = sum(s["accepted"] for s in record["sites"].values())
        record["mismatch"] = sum(s["mismatch"] for s in record["sites"].values())
        record["calls_per_tick"] = record["calls"] / ticks
        record["accept_pct"] = 100.0 * record["accepted"] / record["calls"] if record["calls"] else 0.0
        out.append(record)
    return out


def gate(records: list[dict]) -> tuple[str, str]:
    if not records:
        return "NO_DATA", "no TRIG_DF records"
    audit = [r for r in records if r["mode"] == "audit"]
    if not audit:
        return "PRODUCT_ONLY", "records are product-mode; no shadow comparison present"
    bad = [r for r in audit if r["mismatch"]]
    if bad:
        return "FAIL", f"{len(bad)} window(s) with mismatches (first at line {bad[0]['line']})"
    calls = sum(r["calls"] for r in audit)
    if calls == 0:
        return "EMPTY", "audit windows compared zero calls"
    return "PASS", f"{len(audit)} audit window(s), {calls} calls compared, zero mismatches"


def render(records: list[dict]) -> str:
    lines = ["window   stage  sf            mode     calls/tick  accept%  mismatch  " +
             "  ".join(f"{k}=calls/acc/mis" for k, _n in SITES)]
    for i, r in enumerate(records, start=1):
        cells = "  ".join(f"{k}={r['sites'][n]['calls']}/{r['sites'][n]['accepted']}/{r['sites'][n]['mismatch']}" for k, n in SITES)
        lines.append(f"{i:<8d} {r['stage']:<6d} {r['sf_first']}-{r['sf_last']:<8d} {r['mode']:<8s} "
                     f"{r['calls_per_tick']:<11.1f} {r['accept_pct']:<8.2f} {r['mismatch']:<9d} {cells}")
    if records:
        ticks = sum(r["ticks"] for r in records)
        tot = {n: [sum(r["sites"][n][k] for r in records) for k in ("calls", "accepted", "mismatch")] for _k, n in SITES}
        lines.append("total per tick: " + ", ".join(f"{n} {v[0] / ticks:.1f} calls ({100.0 * v[1] / v[0] if v[0] else 0.0:.2f}% accepted, {v[2]} mismatch)" for n, v in tot.items()))
        lines.append("reasons: " + " ".join(f"{k}={sum(r[k] for r in records)}" for k in REASONS))
    verdict, reason = gate(records)
    lines.append(f"gate: {verdict} ({reason})")
    return "\n".join(lines)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("log", type=pathlib.Path)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)
    records = summarize(args.log.read_text(encoding="utf-8", errors="replace"))
    if args.json:
        verdict, reason = gate(records)
        print(json.dumps({"records": records, "gate": verdict, "reason": reason}, indent=2))
    else:
        print(render(records))
    return 0 if gate(records)[0] in ("PASS", "PRODUCT_ONLY") else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
