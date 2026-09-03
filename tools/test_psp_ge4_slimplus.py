#!/usr/bin/env python3
"""Static and byte-level gates for the PSP Slim+ GE4 aperture path."""

from __future__ import annotations

import importlib.util
import subprocess
import sys
import tempfile
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
BUILDER_PATH = ROOT / "tools" / "build_ge4_slimplus_wrapper.py"
BASE_PATH = ROOT / "deps" / "ge4wrap_texv1.prx"

spec = importlib.util.spec_from_file_location("build_ge4_slimplus_wrapper", BUILDER_PATH)
assert spec is not None and spec.loader is not None
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)


class Ge4SlimPlusTests(unittest.TestCase):
    def test_frozen_base_and_derived_identity(self) -> None:
        base = BASE_PATH.read_bytes()
        self.assertEqual(len(base), builder.BASE_SIZE)
        self.assertEqual(builder.sha256(base), builder.BASE_SHA256)
        derived = builder.derive(base)
        self.assertEqual(builder.sha256(derived), builder.CANDIDATE_SHA256)
        start = builder.MODEL_GATE_OFFSET
        end = start + len(builder.MODEL3_GATE)
        self.assertEqual(base[start:end], builder.MODEL3_GATE)
        self.assertEqual(derived[start:end], builder.SLIMPLUS_GATE)
        self.assertEqual(derived[:start], base[:start])
        self.assertEqual(derived[end:], base[end:])

    def test_builder_refuses_non_authoritative_input(self) -> None:
        base = bytearray(BASE_PATH.read_bytes())
        base[-1] ^= 1
        with self.assertRaises(ValueError):
            builder.derive(bytes(base))

    def test_atomic_cli_output_matches_byte_contract(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "frozen.prx"
            output = Path(temporary) / "ge4wrap_texv1.prx"
            source.write_bytes(BASE_PATH.read_bytes())
            expected = builder.derive(BASE_PATH.read_bytes())
            result = subprocess.run(
                [
                    sys.executable,
                    str(BUILDER_PATH),
                    "--input",
                    str(source),
                    "--output",
                    str(output),
                ],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(output.read_bytes(), expected)

    def test_cli_refuses_in_place_overwrite_without_changing_source(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "frozen.prx"
            original = BASE_PATH.read_bytes()
            source.write_bytes(original)
            result = subprocess.run(
                [
                    sys.executable,
                    str(BUILDER_PATH),
                    "--input",
                    str(source),
                    "--output",
                    str(source),
                ],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(source.read_bytes(), original)

    def test_game_bridge_retains_all_non_model_runtime_gates(self) -> None:
        source = (ROOT / "psp" / "ge4_bridge.cpp").read_text(encoding="utf-8")
        self.assertIn("model < kMinimumModel", source)
        self.assertIn("base != kExpectedEdramBase", source)
        self.assertIn("hwSize != kFourMiB", source)
        self.assertIn("sizeBefore != kTwoMiB", source)
        self.assertIn("sizeAfter != kFourMiB", source)
        self.assertIn("RestoreTwoMiBOrCold", source)
        self.assertIn("WaitForGeIdle", source)
        self.assertIn("GE4 ACTIVE model=%d", source)

    def test_makefile_packages_only_the_exact_derived_wrapper(self) -> None:
        makefile = (ROOT / "Makefile.psp").read_text(encoding="utf-8")
        self.assertIn("TH08_PSP_SLIMPLUS_GE4 ?= 1", makefile)
        self.assertIn(f"GE4_SLIMPLUS_PRX_SHA256 := {builder.CANDIDATE_SHA256}", makefile)
        self.assertIn("--input \"$(GE4_PROVEN_PRX_SOURCE)\" --output \"$@\"", makefile)
        self.assertIn("-DTH08_PSP_SLIMPLUS_GE4=1", makefile)
        self.assertIn("$(GE4_MODEL_GATE_CONFIG_STAMP)", makefile)
        self.assertIn(
            "EBOOT.PBP: $(NEWLIB_HEAP_ABI_STAMP) $(PSPGL_GE4_LINK_STAMP) \\",
            makefile,
        )
        self.assertIn("\t$(GE4_PROVEN_PRX)", makefile)


if __name__ == "__main__":
    unittest.main()
