#!/usr/bin/env python3
"""Static source-delta gate for ITEM_TIME natural-batch 4V.

The audit compares an Item-OFF source tree with an Item-ON source tree.  It is
not a substitute for the runtime comparator; it prevents the two architectural
regressions that made r052/r053 slow before a build is run: a new Item
Begin/End bracket and a new explicit FlushVertexBuffer call.  It also rejects
new state/upload calls and draw calls outside the two renderer files where an
exclusive replacement submit may live.
"""

from __future__ import annotations

import argparse
import difflib
import json
import re
import sys
from pathlib import Path
from typing import Any

import item_natural_quads_contract as contract


SCHEMA = "th08_item_natural_quads_static_audit_v1"
TRACKED_PREFIXES = ("src", "psp")
TRACKED_SUFFIXES = (".cpp", ".hpp", ".h", ".c", ".mk")
APPROVED_DRAW_FILES = frozenset(
    {"src/AnmManager.cpp", "src/modern/linux/d3d8_compat.cpp"}
)

BEGIN_END_PATTERN = re.compile(
    r"\b(?:Begin|End)PspItem[A-Za-z0-9_]*Batch\s*\("
)
EXPLICIT_FLUSH_PATTERN = re.compile(
    r"(?:->|\.)\s*FlushVertexBuffer\s*\(|\bFlushPspItem[A-Za-z0-9_]*Batch\s*\("
)
STATE_OR_UPLOAD_PATTERN = re.compile(
    r"\b(?:SetTextureStageState|SetRenderState|SetTexture|SetVertexShader|"
    r"RenderPerfNoteStateRequested|RenderPerfNoteUploadAttempt|LockRect|"
    r"UnlockRect)\s*\("
)
DRAW_PATTERN = re.compile(
    r"\b(?:DrawPrimitive|DrawIndexedPrimitive|glDrawArrays|glDrawElements|"
    r"RenderPerfNoteDraw|__pspgl_[A-Za-z0-9_]*draw[A-Za-z0-9_]*)\s*\("
)


def _tracked_files(root: Path) -> dict[str, list[str]]:
    result: dict[str, list[str]] = {}
    makefile = root / "Makefile.psp"
    if makefile.is_file():
        result["Makefile.psp"] = makefile.read_text(
            encoding="utf-8", errors="replace"
        ).splitlines()
    for prefix in TRACKED_PREFIXES:
        directory = root / prefix
        if not directory.is_dir():
            continue
        for path in sorted(directory.rglob("*")):
            if not path.is_file() or path.suffix not in TRACKED_SUFFIXES:
                continue
            relative = path.relative_to(root).as_posix()
            result[relative] = path.read_text(
                encoding="utf-8", errors="replace"
            ).splitlines()
    return result


def _added_lines(
    baseline: dict[str, list[str]], candidate: dict[str, list[str]]
) -> list[dict[str, Any]]:
    additions: list[dict[str, Any]] = []
    for relative in sorted(set(baseline) | set(candidate)):
        before = baseline.get(relative, [])
        after = candidate.get(relative, [])
        matcher = difflib.SequenceMatcher(None, before, after, autojunk=False)
        for tag, _, _, candidate_start, candidate_end in matcher.get_opcodes():
            if tag not in ("insert", "replace"):
                continue
            for index in range(candidate_start, candidate_end):
                additions.append(
                    {
                        "path": relative,
                        "line": index + 1,
                        "text": after[index],
                    }
                )
    return additions


def _whole_text(sources: dict[str, list[str]], relative: str) -> str:
    return "\n".join(sources.get(relative, []))


def audit_trees(baseline_root: Path, candidate_root: Path) -> dict[str, Any]:
    baseline = _tracked_files(baseline_root)
    candidate = _tracked_files(candidate_root)
    additions = _added_lines(baseline, candidate)
    violations: list[dict[str, Any]] = []

    def reject(rule: str, addition: dict[str, Any], reason: str) -> None:
        violations.append({"rule": rule, "reason": reason, **addition})

    for addition in additions:
        text = addition["text"]
        if BEGIN_END_PATTERN.search(text):
            reject(
                "no_added_item_begin_end",
                addition,
                "natural batching must not add an Item Begin/End bracket",
            )
        if EXPLICIT_FLUSH_PATTERN.search(text):
            reject(
                "no_added_explicit_flush",
                addition,
                "natural batching must consume an existing flush, never call one",
            )
        if STATE_OR_UPLOAD_PATTERN.search(text):
            reject(
                "no_added_state_or_upload",
                addition,
                "natural batching inherits the existing state/upload boundary",
            )
        if DRAW_PATTERN.search(text) and addition["path"] not in APPROVED_DRAW_FILES:
            reject(
                "draw_replacement_renderer_only",
                addition,
                "an exclusive replacement draw may exist only in the renderer",
            )
        if (
            addition["path"] == "src/BulletManager.cpp"
            and (
                contract.FEATURE_MACRO in text
                or "ItemNatural" in text
                or "ITEM_NATURAL" in text
            )
        ):
            reject(
                "bullet_manager_untouched",
                addition,
                "ITEM_TIME natural batching must not alter the Bullet pass",
            )

    makefile = _whole_text(candidate, "Makefile.psp")
    anm = _whole_text(candidate, "src/AnmManager.cpp")
    telemetry = _whole_text(candidate, "psp/memory_telemetry.cpp")
    required_checks = {
        "feature_makefile": contract.FEATURE_MACRO in makefile,
        "feature_flush_site": contract.FEATURE_MACRO in anm
        and "FlushVertexBuffer" in anm,
        "telemetry_prefix": contract.TELEMETRY_PREFIX in telemetry,
        "telemetry_existing_flush": "existing_flush=1" in telemetry,
        "telemetry_no_begin_end": "begin_end_added=0" in telemetry,
        "telemetry_topology": "topology=6v_to_4v_indexed" in telemetry,
    }
    missing_schema_fields = [
        field
        for field in contract.INTERVAL_FIELDS
        if f"{field}=" not in telemetry
    ]
    if missing_schema_fields:
        required_checks["telemetry_interval_schema"] = False
    else:
        required_checks["telemetry_interval_schema"] = True

    for name, passed in required_checks.items():
        if passed:
            continue
        violations.append(
            {
                "rule": name,
                "reason": (
                    f"missing telemetry fields {missing_schema_fields}"
                    if name == "telemetry_interval_schema"
                    else "required natural-batch source contract is missing"
                ),
                "path": None,
                "line": None,
                "text": None,
            }
        )

    passed = not violations
    return {
        "schema": SCHEMA,
        "feature_macro": contract.FEATURE_MACRO,
        "telemetry_prefix": contract.TELEMETRY_PREFIX,
        "baseline_root": str(baseline_root),
        "candidate_root": str(candidate_root),
        "tracked_baseline_files": len(baseline),
        "tracked_candidate_files": len(candidate),
        "added_lines": len(additions),
        "approved_draw_files": sorted(APPROVED_DRAW_FILES),
        "required_checks": required_checks,
        "missing_telemetry_fields": missing_schema_fields,
        "violations": violations,
        "passed": passed,
        "verdict": "PASS" if passed else "FAIL",
    }


def render_text(result: dict[str, Any]) -> str:
    lines = [
        f"schema={result['schema']}",
        f"feature_macro={result['feature_macro']}",
        f"telemetry_prefix={result['telemetry_prefix']}",
        f"baseline_root={result['baseline_root']}",
        f"candidate_root={result['candidate_root']}",
        f"added_lines={result['added_lines']}",
        f"passed={str(result['passed']).lower()} verdict={result['verdict']}",
    ]
    for name, passed in result["required_checks"].items():
        lines.append(f"REQUIRED name={name} passed={str(passed).lower()}")
    for violation in result["violations"]:
        lines.append(
            f"VIOLATION rule={violation['rule']} path={violation['path']} "
            f"line={violation['line']} reason={violation['reason']} "
            f"text={violation['text']!r}"
        )
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Audit ITEM_TIME natural-quad source delta before building"
    )
    parser.add_argument("baseline_root", type=Path)
    parser.add_argument("candidate_root", type=Path)
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)
    try:
        result = audit_trees(args.baseline_root, args.candidate_root)
    except OSError as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 2
    output = (
        json.dumps(result, ensure_ascii=False, indent=2, sort_keys=True) + "\n"
        if args.json
        else render_text(result)
    )
    if args.output is None:
        sys.stdout.write(output)
    else:
        try:
            args.output.write_text(output, encoding="utf-8")
        except OSError as error:
            print(f"ERROR: {args.output}: cannot write output: {error}", file=sys.stderr)
            return 2
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
