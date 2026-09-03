#!/usr/bin/env python3
"""Fixture tests for audit_item_natural_quads_static.py."""

from __future__ import annotations

import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools"
TOOL_PATH = TOOLS / "audit_item_natural_quads_static.py"
sys.path.insert(0, str(TOOLS))
SPEC = importlib.util.spec_from_file_location(
    "audit_item_natural_quads_static", TOOL_PATH
)
assert SPEC is not None and SPEC.loader is not None
TOOL = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = TOOL
SPEC.loader.exec_module(TOOL)


def telemetry_contract_text(*, omit: str | None = None) -> str:
    fields = [
        f'"{field}=%lu "' for field in TOOL.contract.INTERVAL_FIELDS if field != omit
    ]
    return "\n".join(
        [
            "void LogNatural() {",
            f'  // {TOOL.contract.TELEMETRY_PREFIX}',
            '  // existing_flush=1 begin_end_added=0 topology=6v_to_4v_indexed',
            *[f"  {field}" for field in fields],
            "}",
        ]
    ) + "\n"


class TemporaryTrees(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.directory = Path(self.temporary.name)
        self.baseline = self.directory / "baseline"
        self.candidate = self.directory / "candidate"
        self.baseline.mkdir()
        self.candidate.mkdir()
        baseline_anm = (
            "void AnmManager::FlushVertexBuffer() {\n"
            "  CanonicalExistingDraw();\n"
            "}\n"
        )
        self.write(self.baseline, "Makefile.psp", "BASE=1\n")
        self.write(self.candidate, "Makefile.psp", "BASE=1\n")
        self.write(self.baseline, "src/AnmManager.cpp", baseline_anm)
        self.write(self.candidate, "src/AnmManager.cpp", baseline_anm)
        self.write(self.baseline, "psp/memory_telemetry.cpp", "void Log() {}\n")
        self.write(self.candidate, "psp/memory_telemetry.cpp", "void Log() {}\n")

    def tearDown(self) -> None:
        self.temporary.cleanup()

    @staticmethod
    def write(root: Path, relative: str, text: str) -> None:
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def make_valid_candidate(self) -> None:
        self.write(
            self.candidate,
            "Makefile.psp",
            f"BASE=1\n{TOOL.contract.FEATURE_MACRO} ?= 0\n",
        )
        self.write(
            self.candidate,
            "src/AnmManager.cpp",
            "void AnmManager::FlushVertexBuffer() {\n"
            "  CanonicalExistingDraw();\n"
            "}\n"
            f"#if defined({TOOL.contract.FEATURE_MACRO})\n"
            "bool PackItemNaturalAtExistingBoundary() { return true; }\n"
            "#endif\n",
        )
        self.write(
            self.candidate,
            "psp/memory_telemetry.cpp",
            "void Log() {}\n" + telemetry_contract_text(),
        )

    def audit(self):
        return TOOL.audit_trees(self.baseline, self.candidate)


class StaticContractTests(TemporaryTrees):
    def test_valid_replacement_only_delta_passes(self) -> None:
        self.make_valid_candidate()
        result = self.audit()
        self.assertTrue(result["passed"], result["violations"])
        self.assertEqual(result["feature_macro"], TOOL.contract.FEATURE_MACRO)
        self.assertTrue(all(result["required_checks"].values()))

    def test_new_item_begin_or_end_bracket_is_rejected(self) -> None:
        for call in (
            "g_AnmManager->BeginPspItemNaturalQuadBatch();",
            "g_AnmManager->EndPspItemNaturalQuadBatch();",
        ):
            with self.subTest(call=call):
                self.make_valid_candidate()
                path = self.candidate / "src/AnmManager.cpp"
                path.write_text(path.read_text() + call + "\n", encoding="utf-8")
                result = self.audit()
                self.assertFalse(result["passed"])
                self.assertIn(
                    "no_added_item_begin_end",
                    {item["rule"] for item in result["violations"]},
                )

    def test_new_explicit_flush_call_is_rejected(self) -> None:
        self.make_valid_candidate()
        path = self.candidate / "src/AnmManager.cpp"
        path.write_text(
            path.read_text() + "void Bad() { this->FlushVertexBuffer(); }\n",
            encoding="utf-8",
        )
        result = self.audit()
        self.assertIn(
            "no_added_explicit_flush",
            {item["rule"] for item in result["violations"]},
        )

    def test_new_state_or_upload_boundary_is_rejected(self) -> None:
        for call in (
            "device->SetTextureStageState(0, 0, 0);",
            "RenderPerfNoteUploadAttempt();",
        ):
            with self.subTest(call=call):
                self.make_valid_candidate()
                path = self.candidate / "src/AnmManager.cpp"
                path.write_text(path.read_text() + call + "\n", encoding="utf-8")
                result = self.audit()
                self.assertIn(
                    "no_added_state_or_upload",
                    {item["rule"] for item in result["violations"]},
                )

    def test_draw_call_outside_renderer_is_rejected(self) -> None:
        self.make_valid_candidate()
        self.write(
            self.candidate,
            "src/ItemManager.cpp",
            "void Bad() { device->DrawIndexedPrimitive(0, 0, 0, 0, 0, 0); }\n",
        )
        result = self.audit()
        self.assertIn(
            "draw_replacement_renderer_only",
            {item["rule"] for item in result["violations"]},
        )

    def test_item_feature_cannot_spread_into_bullet_manager(self) -> None:
        self.make_valid_candidate()
        self.write(
            self.candidate,
            "src/BulletManager.cpp",
            f"#if {TOOL.contract.FEATURE_MACRO}\n#endif\n",
        )
        result = self.audit()
        self.assertIn(
            "bullet_manager_untouched",
            {item["rule"] for item in result["violations"]},
        )

    def test_complete_telemetry_schema_is_required(self) -> None:
        self.make_valid_candidate()
        self.write(
            self.candidate,
            "psp/memory_telemetry.cpp",
            "void Log() {}\n" + telemetry_contract_text(omit="extra_flushes"),
        )
        result = self.audit()
        self.assertFalse(result["required_checks"]["telemetry_interval_schema"])
        self.assertEqual(result["missing_telemetry_fields"], ["extra_flushes"])


class CommandLineTests(TemporaryTrees):
    def test_json_pass_returns_zero(self) -> None:
        self.make_valid_candidate()
        completed = subprocess.run(
            [
                sys.executable,
                str(TOOL_PATH),
                str(self.baseline),
                str(self.candidate),
                "--json",
            ],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        result = json.loads(completed.stdout)
        self.assertTrue(result["passed"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
