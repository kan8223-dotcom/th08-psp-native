#!/usr/bin/env python3
"""Differential and integration gates for the PSP anti-tamper SWAR path."""

from __future__ import annotations

import pathlib
import re
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
HEADER = ROOT / "psp" / "antitamper_checksum.hpp"
HARNESS = ROOT / "tools" / "psp_antitamper_checksum_harness.cpp"
GAME_MANAGER_CPP = ROOT / "src" / "GameManager.cpp"
GAME_MANAGER_HPP = ROOT / "src" / "GameManager.hpp"
MAKEFILE = ROOT / "Makefile.psp"
MAIN = ROOT / "psp" / "main.cpp"
FEATURE = "TH08_PSP_ANTITAMPER_SWAR"


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening : index + 1]
    raise AssertionError(f"unterminated function: {signature}")


class AntiTamperChecksumTests(unittest.TestCase):
    def test_randomized_differential_harness(self) -> None:
        with tempfile.TemporaryDirectory(prefix="th08-antitamper-") as temp:
            binary = pathlib.Path(temp) / "antitamper-checksum"
            subprocess.run(
                [
                    "g++",
                    "-std=c++17",
                    "-O2",
                    "-Wall",
                    "-Wextra",
                    "-Wconversion",
                    "-Werror",
                    "-fsanitize=undefined",
                    "-fno-sanitize-recover=all",
                    f"-D{FEATURE}=1",
                    "-DPSP=1",
                    "-I",
                    str(ROOT),
                    str(HARNESS),
                    "-o",
                    str(binary),
                ],
                check=True,
                cwd=ROOT,
            )
            result = subprocess.run(
                [str(binary)], check=True, text=True, capture_output=True
            )
            self.assertIn("antitamper-checksum: PASS", result.stdout)
            self.assertIn("retail_bytes=628", result.stdout)

    def test_helper_has_safe_word_load_and_byte_fallbacks(self) -> None:
        text = HEADER.read_text(encoding="utf-8")
        self.assertIn("std::memcpy(&word, address, sizeof(word));", text)
        self.assertIn("reinterpret_cast<std::uintptr_t>(address) & 3U", text)
        self.assertIn("word & 0x00ff00ffU", text)
        self.assertIn("pairSums >> 16U", text)
        self.assertGreaterEqual(text.count("while (size"), 3)
        self.assertNotRegex(text, r"reinterpret_cast<[^>]*uint32_t\s*\*>")
        self.assertNotIn("malloc", text)
        self.assertNotIn("new ", text)

    def test_game_path_preserves_canonical_generic_checksum(self) -> None:
        source = GAME_MANAGER_CPP.read_text(encoding="utf-8")
        generic = function_body(source, "i32 GameManager::CalcChecksum")
        self.assertIn("sum += *address;", generic)
        self.assertIn("antiTamperValue += g_GameManager.globals->rng8[2]", generic)
        self.assertNotIn("AntiTamperSwarByteSum", generic)

        specialized = function_body(
            source, "i32 GameManager::CalcAntiTamperChecksum"
        )
        self.assertIn(f"defined({FEATURE})", specialized)
        self.assertIn("kTotalBytes == 628U", specialized)
        self.assertEqual(specialized.count("AntiTamperSwarByteSum"), 5)
        self.assertIn("AntiTamperAdvanceValue", specialized)
        self.assertIn("static_cast<i32>(sum)", specialized)
        self.assertIn("#else", specialized)
        self.assertEqual(specialized.count("CalcChecksum("), 5)

    def test_update_rng_calls_and_order_are_unchanged(self) -> None:
        header = GAME_MANAGER_HPP.read_text(encoding="utf-8")
        body = function_body(header, "void UpdateAntiTamper()")
        rng1 = body.index("rng1[2] = g_Rng.GetRandomU32InRange")
        rng7 = body.index("rng7[3] = g_Rng.GetRandomU32InRange")
        value = body.index("antiTamperValue = this->globals->rng1[2]")
        checksum = body.index("antiTamperChecksum = CalcAntiTamperChecksum()")
        expected = body.index("antiTamperExpectedValue")
        self.assertLess(rng1, rng7)
        self.assertLess(rng7, value)
        self.assertLess(value, checksum)
        self.assertLess(checksum, expected)
        self.assertEqual(body.count("GetRandomU32InRange"), 2)

    def test_default_off_stamp_and_boot_fingerprint(self) -> None:
        makefile = MAKEFILE.read_text(encoding="utf-8")
        main = MAIN.read_text(encoding="utf-8")
        self.assertIn(f"{FEATURE} ?= 0", makefile)
        self.assertIn("antitamper-swar-0.stamp", makefile)
        self.assertIn("antitamper-swar-1.stamp", makefile)
        self.assertIn(f"-D{FEATURE}=1", makefile)
        self.assertRegex(
            makefile,
            re.compile(
                r"src/GameManager\.o psp/main\.o:\s*"
                r"\$\(ANTITAMPER_SWAR_CONFIG_STAMP\)"
            ),
        )
        self.assertIn("ANTITAMPER_SWAR=%d", main)
        self.assertIn("TH08_PSP_FEATURE_ANTITAMPER_SWAR", main)


if __name__ == "__main__":
    unittest.main()
