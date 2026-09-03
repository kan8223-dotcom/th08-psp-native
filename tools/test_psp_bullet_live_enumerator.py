#!/usr/bin/env python3
"""Host differential/static gates for TH08_PSP_BULLET_LIVE_ENUM.

The production loop is PSP-only.  The dependency-free enumerator header is
therefore compiled into a host harness which compares it with the canonical
0,1535..1 scan under identical same-frame mutations.  Static checks bind that
tested primitive to BulletManager's authoritative-state repair, lifecycle
fallback, build fingerprint, and interval telemetry.
"""

from __future__ import annotations

import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "src" / "PspBulletLiveEnumerator.hpp").read_text(
    encoding="utf-8"
)
BULLET_CPP = (ROOT / "src" / "BulletManager.cpp").read_text(encoding="utf-8")
BULLET_HPP = (ROOT / "src" / "BulletManager.hpp").read_text(encoding="utf-8")
MAKEFILE = (ROOT / "Makefile.psp").read_text(encoding="utf-8")
MAIN_CPP = (ROOT / "psp" / "main.cpp").read_text(encoding="utf-8")
MEMORY_CPP = (ROOT / "psp" / "memory_telemetry.cpp").read_text(
    encoding="utf-8"
)
HARNESS = ROOT / "tools" / "psp_bullet_live_enumerator_harness.cpp"
FEATURE = "TH08_PSP_BULLET_LIVE_ENUM"


def find_compiler() -> str:
    for candidate in (os.environ.get("CXX"), "g++", "clang++"):
        if candidate:
            resolved = shutil.which(candidate)
            if resolved:
                return resolved
    raise unittest.SkipTest("no host C++17 compiler found")


def function_body(source: str, signature: str) -> str:
    start = source.find(signature)
    if start < 0:
        raise AssertionError(f"missing function: {signature}")
    opening = source.find("{", start)
    if opening < 0:
        raise AssertionError(f"missing function body: {signature}")
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening : index + 1]
    raise AssertionError(f"unterminated function: {signature}")


class BulletLiveEnumeratorHostTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.temporary = tempfile.TemporaryDirectory(
            prefix="th08-bullet-live-enum-"
        )
        cls.executable = Path(cls.temporary.name) / "bullet_live_enum_harness"
        command = [
            find_compiler(),
            "-std=c++17",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-pedantic",
            "-I",
            str(ROOT / "src"),
            str(HARNESS),
            "-o",
            str(cls.executable),
        ]
        result = subprocess.run(command, text=True, capture_output=True)
        if result.returncode != 0:
            raise AssertionError(
                "host harness compilation failed:\n"
                + result.stdout
                + result.stderr
            )

    @classmethod
    def tearDownClass(cls) -> None:
        cls.temporary.cleanup()

    def test_random_and_targeted_differential_harness(self) -> None:
        result = subprocess.run(
            [str(self.executable)], text=True, capture_output=True
        )
        self.assertEqual(
            result.returncode,
            0,
            msg="host differential harness failed:\n"
            + result.stdout
            + result.stderr,
        )

    def test_feature_is_independent_default_off_and_stamped(self) -> None:
        self.assertRegex(
            MAKEFILE, rf"(?m)^{FEATURE}\s*\?=\s*0\s*$"
        )
        self.assertIn(f"ifeq ($({FEATURE}),1)", MAKEFILE)
        self.assertIn(
            f"$(error {FEATURE}=1 requires TH08_PSP_BULLET_FASTPATH=1)",
            MAKEFILE,
        )
        self.assertIn("bullet-live-enum-0.stamp", MAKEFILE)
        self.assertIn("bullet-live-enum-1.stamp", MAKEFILE)
        self.assertRegex(
            MAKEFILE,
            r"src/BulletManager\.o psp/memory_telemetry\.o psp/main\.o:\s*\\\n"
            r"\s*\$\(BULLET_LIVE_ENUM_CONFIG_STAMP\)",
        )

    def test_enumerator_is_dynamic_and_preserves_canonical_order(self) -> None:
        self.assertIn("const volatile unsigned int *words", HEADER)
        self.assertIn("slotZeroPending", HEADER)
        self.assertIn("descendingExclusive", HEADER)
        self.assertIn("*outSlot = 0U", HEADER)
        slot_zero = HEADER[HEADER.index("if (slotZeroPending)") :]
        self.assertLess(
            slot_zero.index("++*wordProbes;"), slot_zero.index("words[0] & 1U")
        )
        self.assertIn("unsigned int word = words[wordIndex]", HEADER)
        self.assertIn("word &= ~1U", HEADER)
        self.assertIn("__builtin_clz(word)", HEADER)
        self.assertNotIn("memcpy", HEADER)
        self.assertNotIn("std::array", HEADER)
        self.assertIn("kSlotCount = 0x600U", HEADER)
        self.assertIn("kWordCount == 48U", HEADER)

        update = BULLET_CPP[BULLET_CPP.index("BulletManager::OnUpdate") :]
        audit_pos = update.index("PspBulletRuntimeAuditAndRepair")
        construct_pos = update.index("PspBulletLiveEnumerator liveEnumerator")
        next_pos = update.index("liveEnumerator.Next")
        self.assertLess(audit_pos, construct_pos)
        self.assertLess(construct_pos, next_pos)
        self.assertIn("if (useLiveEnumerator)\n            continue;", update)

    def test_unavailable_or_invalid_sidecar_uses_full_scan(self) -> None:
        bit = function_body(BULLET_CPP, "inline bool PspBulletRuntimeBit")
        self.assertIn("g_PspBulletRuntimeCache == NULL", bit)
        self.assertIn("!g_PspBulletRuntimeActiveBitsValid", bit)
        self.assertRegex(bit, r"!g_PspBulletRuntimeActiveBitsValid\)\s*return true;")

        update = BULLET_CPP[BULLET_CPP.index("BulletManager::OnUpdate") :]
        self.assertIn(
            "const bool useLiveEnumerator = liveEnumerator.IsUsable();", update
        )
        self.assertIn("if (!useLiveEnumerator)\n        liveEnumSlotProbes += 0x600U;", update)
        self.assertIn("!useLiveEnumerator);", update)

    def test_writers_and_false_negative_repair_remain_authoritative(self) -> None:
        spawn = BULLET_CPP[
            BULLET_CPP.index("BulletManager::SpawnSingleBullet") :
            BULLET_CPP.index("// FUNCTION: th08 0x42fe70")
        ]
        self.assertIn("PspBulletRuntimeActivate(bullet);", spawn)

        deactivate = function_body(BULLET_CPP, "void Bullet::Deactivate()")
        self.assertIn("this->state = BULLET_STATE_UNUSED;", deactivate)
        self.assertIn("PspBulletRuntimeDeactivate(this);", deactivate)

        # These three whole-slot clears are the only memset-based Bullet death
        # writers in the translation unit, and each must clear the sidecar.
        clear_pattern = re.compile(
            r"memset\(bullet, 0, sizeof\(Bullet\)\);\s*"
            r"#if defined\(TH08_PSP_BULLET_TRANSFORM_TERMINAL_FASTPATH_ENABLED\)\s*"
            r"PspBulletTransformTerminalClearForWholeSlotReset\(bullet\);\s*"
            r"#endif\s*"
            r"#if defined\(TH08_PSP_BULLET_RUNTIME_FASTPATH\)\s*"
            r"PspBulletRuntimeDeactivate\(bullet\);"
        )
        self.assertEqual(BULLET_CPP.count("memset(bullet, 0, sizeof(Bullet));"), 3)
        self.assertEqual(len(clear_pattern.findall(BULLET_CPP)), 3)

        audit = function_body(
            BULLET_CPP, "void PspBulletRuntimeAuditAndRepair"
        )
        self.assertIn("state != BULLET_STATE_UNUSED", audit)
        self.assertIn("PspBulletRuntimeRawBit(index)", audit)
        self.assertIn("PspBulletRuntimeSetBit(index, stateActive);", audit)
        self.assertIn("g_PspBulletRuntimeActiveBitsValid = true;", audit)
        self.assertLess(
            audit.index("PspBulletRuntimeSetBit(index, stateActive);"),
            audit.index("g_PspBulletRuntimeActiveBitsValid = true;"),
        )
        self.assertGreaterEqual(
            BULLET_CPP.count("g_PspBulletRuntimeActiveBitsValid = false;"), 2
        )

    def test_boot_fingerprint_and_interval_telemetry_are_complete(self) -> None:
        self.assertIn("TH08_PSP_FEATURE_BULLET_LIVE_ENUM", MAIN_CPP)
        self.assertIn("BULLET_LIVE_ENUM=%d", MAIN_CPP)
        self.assertIn("PspBulletLiveEnumTelemetrySnapshot", BULLET_HPP)
        self.assertIn("PspPeekBulletLiveEnumTelemetry", BULLET_HPP)
        self.assertIn("PspTakeBulletLiveEnumTelemetry", BULLET_HPP)
        self.assertIn("PspResetBulletLiveEnumTelemetry", BULLET_HPP)
        self.assertIn("PspTakeBulletLiveEnumTelemetry()", MEMORY_CPP)
        self.assertIn("PspPeekBulletLiveEnumTelemetry()", MEMORY_CPP)
        self.assertIn(
            "renderPerf != nullptr ? th08::PspTakeBulletLiveEnumTelemetry()",
            MEMORY_CPP,
        )
        self.assertIn("th08::PspResetBulletLiveEnumTelemetry();", MEMORY_CPP)
        self.assertIn("mark_is_non_destructive=1", MEMORY_CPP)
        for field in (
            "bullet_enum_frames=%llu",
            "bullet_enum_slot_probes=%llu",
            "bullet_enum_word_probes=%llu",
            "bullet_enum_visited=%llu",
            "bullet_enum_fallback_frames=%llu",
        ):
            self.assertIn(field, MEMORY_CPP)


if __name__ == "__main__":
    unittest.main()
