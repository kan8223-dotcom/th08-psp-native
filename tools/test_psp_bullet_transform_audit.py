#!/usr/bin/env python3
"""Host-only source/static gates for the PSP Bullet transform audit.

The audit is deliberately counter-only: it observes the canonical interpreter
without adding a terminal-program skip.  These checks bind the default-OFF
feature gate to every relevant writer, stage-window telemetry semantics, and
the boot fingerprint so a measured follow-up optimization can be reviewed as
a separate change.
"""

from __future__ import annotations

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
BULLET_CPP = (ROOT / "src" / "BulletManager.cpp").read_text(encoding="utf-8")
BULLET_HPP = (ROOT / "src" / "BulletManager.hpp").read_text(encoding="utf-8")
MAKEFILE = (ROOT / "Makefile.psp").read_text(encoding="utf-8")
MAIN_CPP = (ROOT / "psp" / "main.cpp").read_text(encoding="utf-8")
MEMORY_CPP = (ROOT / "psp" / "memory_telemetry.cpp").read_text(
    encoding="utf-8"
)
FEATURE = "TH08_PSP_BULLET_TRANSFORM_AUDIT"


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


class CounterWindowModel:
    """Executable host model for the required peek/take/reset contract."""

    def __init__(self) -> None:
        self.values = [0] * 11

    def note(self, field: int, amount: int = 1) -> None:
        self.values[field] += amount

    def peek(self) -> tuple[int, ...]:
        return tuple(self.values)

    def take(self) -> tuple[int, ...]:
        snapshot = self.peek()
        self.reset()
        return snapshot

    def reset(self) -> None:
        self.values = [0] * 11


class BulletTransformAuditTests(unittest.TestCase):
    def test_counter_window_model_marks_peek_samples_take_and_baselines_reset(self) -> None:
        counters = CounterWindowModel()
        counters.note(0, 120)
        counters.note(1, 100)
        counters.note(9, 20)
        mark = counters.peek()
        self.assertEqual(mark[0], mark[1] + mark[9])
        self.assertEqual(counters.peek(), mark)
        self.assertEqual(counters.take(), mark)
        self.assertEqual(counters.peek(), (0,) * 11)
        counters.note(2, 7)
        counters.reset()
        self.assertEqual(counters.peek(), (0,) * 11)

    def test_feature_is_default_off_validated_stamped_and_closes_objects(self) -> None:
        self.assertRegex(MAKEFILE, rf"(?m)^{FEATURE}\s*\?=\s*0\s*$")
        self.assertIn(f"ifeq ($({FEATURE}),1)", MAKEFILE)
        self.assertIn(f"-D{FEATURE}=1", MAKEFILE)
        self.assertIn(f"else ifneq ($({FEATURE}),0)", MAKEFILE)
        self.assertIn(f"$(error {FEATURE} must be 0 or 1)", MAKEFILE)
        self.assertIn("bullet-transform-audit-0.stamp", MAKEFILE)
        self.assertIn("bullet-transform-audit-1.stamp", MAKEFILE)
        self.assertRegex(
            MAKEFILE,
            r"src/BulletManager\.o psp/memory_telemetry\.o psp/main\.o:[^\n]*\\?\n"
            r"(?:[^\n]*\n){0,3}[^\n]*\$\(BULLET_TRANSFORM_AUDIT_CONFIG_STAMP\)",
        )

    def test_snapshot_and_take_reset_api_are_complete(self) -> None:
        for field in (
            "advanceCalls",
            "firedUpdateCalls",
            "terminalIndexReturns",
            "terminalNoneReturns",
            "activeBlockedReturns",
            "disabledRecordsSkipped",
            "recordsDispatched",
            "activeTransformStarts",
            "childSpawnRecords",
            "spawnProgramWrites",
            "wholeSlotResets",
        ):
            self.assertIn(f"unsigned long long {field};", BULLET_HPP)
        self.assertIn("PspPeekBulletTransformAuditTelemetry", BULLET_HPP)
        self.assertIn("PspTakeBulletTransformAuditTelemetry", BULLET_HPP)
        self.assertIn("PspResetBulletTransformAuditTelemetry", BULLET_HPP)

        take = function_body(
            BULLET_CPP, "PspTakeBulletTransformAuditTelemetry()"
        )
        self.assertLess(
            take.index("PspPeekBulletTransformAuditTelemetry()"),
            take.index("PspResetBulletTransformAuditTelemetry();"),
        )

    def test_canonical_calls_and_program_writer_are_observed_not_skipped(self) -> None:
        spawn = BULLET_CPP[
            BULLET_CPP.index("BulletManager::SpawnSingleBullet") :
            BULLET_CPP.index("// FUNCTION: th08 0x42fe70")
        ]
        self.assertRegex(
            spawn,
            r"bullet->transformIndex = descriptor->transformStartIndex;\s*"
            r"#if defined\(TH08_PSP_BULLET_TRANSFORM_AUDIT_ENABLED\)\s*"
            r"PspBulletTransformAuditNoteSpawnProgramWrite\(\);\s*"
            r"#endif\s*bullet->AdvanceTransformProgram\(\);",
        )

        update = function_body(
            BULLET_CPP,
            "ChainCallbackResult BulletManager::OnUpdate(BulletManager *bulletManager)",
        )
        self.assertRegex(
            update,
            r"updateBullet:\s*"
            r"#if defined\(TH08_PSP_BULLET_TRANSFORM_AUDIT_ENABLED\)\s*"
            r"PspBulletTransformAuditNoteFiredUpdateCall\(\);\s*"
            r"#endif\s*"
            r"#if defined\(TH08_PSP_BULLET_TRANSFORM_TERMINAL_FASTPATH_ENABLED\)\s*"
            r"if \(!PspBulletTransformTerminalSkip\([\s\S]*?\)\)\s*"
            r"#endif\s*bullet->AdvanceTransformProgram\(\);",
        )
        self.assertEqual(BULLET_CPP.count("bullet->AdvanceTransformProgram();"), 2)
        self.assertIn(
            "TH08_PSP_BULLET_TRANSFORM_TERMINAL_FASTPATH and "
            "TH08_PSP_BULLET_TRANSFORM_AUDIT are mutually exclusive",
            MAKEFILE,
        )

    def test_interpreter_counts_each_exit_scan_dispatch_and_activation_class(self) -> None:
        advance = function_body(BULLET_CPP, "void Bullet::AdvanceTransformProgram()")
        self.assertIn("++g_PspBulletTransformAuditTelemetry.advanceCalls;", advance)
        for field in ("terminalIndexReturns", "terminalNoneReturns"):
            self.assertRegex(
                advance,
                rf"\+\+g_PspBulletTransformAuditTelemetry\.{field};\s*"
                rf"#endif\s*#if defined\(TH08_PSP_BULLET_TRANSFORM_TERMINAL_FASTPATH_ENABLED\)"
                rf"[\s\S]*?#endif\s*return;",
            )
        self.assertRegex(
            advance,
            r"\+\+g_PspBulletTransformAuditTelemetry\.activeBlockedReturns;\s*"
            r"#endif\s*return;",
        )
        self.assertRegex(
            advance,
            r"\+\+g_PspBulletTransformAuditTelemetry\.disabledRecordsSkipped;\s*"
            r"#endif\s*\+\+this->transformIndex;\s*goto nextRecord;",
        )
        self.assertLess(
            advance.index("PspBulletTransformAuditNoteRecordDispatch(record->kind);"),
            advance.index("switch (record->kind)"),
        )

        dispatch = function_body(
            BULLET_CPP, "inline void PspBulletTransformAuditNoteRecordDispatch"
        )
        for kind in (
            "BULLET_TRANSFORM_DECELERATE",
            "BULLET_TRANSFORM_ACCELERATE_VECTOR",
            "BULLET_TRANSFORM_ACCELERATE_POLAR",
            "BULLET_TRANSFORM_CHANGE_DIRECTION_RELATIVE",
            "BULLET_TRANSFORM_CHANGE_DIRECTION_AIMED",
            "BULLET_TRANSFORM_CHANGE_DIRECTION_ABSOLUTE",
            "BULLET_TRANSFORM_BOUNCE_ALL_EDGES",
            "BULLET_TRANSFORM_BOUNCE_EXCEPT_BOTTOM",
            "BULLET_TRANSFORM_WAIT",
            "BULLET_TRANSFORM_WRAP_X",
            "BULLET_TRANSFORM_WRAP_Y",
        ):
            self.assertIn(f"case {kind}:", dispatch)
        self.assertIn("activeTransformStarts", dispatch)
        self.assertIn("case BULLET_TRANSFORM_SPAWN_CHILD_PATTERN:", dispatch)
        self.assertIn("childSpawnRecords", dispatch)

    def test_all_whole_slot_reset_writers_are_counted_after_sidecar_repair(self) -> None:
        self.assertEqual(BULLET_CPP.count("memset(bullet, 0, sizeof(Bullet));"), 3)
        pattern = re.compile(
            r"memset\(bullet, 0, sizeof\(Bullet\)\);\s*"
            r"#if defined\(TH08_PSP_BULLET_TRANSFORM_TERMINAL_FASTPATH_ENABLED\)\s*"
            r"PspBulletTransformTerminalClearForWholeSlotReset\(bullet\);\s*"
            r"#endif\s*"
            r"#if defined\(TH08_PSP_BULLET_RUNTIME_FASTPATH\)\s*"
            r"PspBulletRuntimeDeactivate\(bullet\);\s*#endif\s*"
            r"#if defined\(TH08_PSP_BULLET_TRANSFORM_AUDIT_ENABLED\)\s*"
            r"PspBulletTransformAuditNoteWholeSlotReset\(\);"
        )
        self.assertEqual(len(pattern.findall(BULLET_CPP)), 3)

    def test_stage_window_telemetry_boot_fingerprint_and_policy_are_bound(self) -> None:
        self.assertIn("TH08_PSP_FEATURE_BULLET_TRANSFORM_AUDIT", MAIN_CPP)
        self.assertIn("BULLET_TRANSFORM_AUDIT=%d", MAIN_CPP)
        self.assertIn("PspTakeBulletTransformAuditTelemetry()", MEMORY_CPP)
        self.assertIn("PspPeekBulletTransformAuditTelemetry()", MEMORY_CPP)
        self.assertIn(
            "renderPerf != nullptr ? th08::PspTakeBulletTransformAuditTelemetry()",
            MEMORY_CPP,
        )
        self.assertGreaterEqual(
            MEMORY_CPP.count("th08::PspResetBulletTransformAuditTelemetry();"),
            2,
        )
        self.assertIn("BULLET_TRANSFORM_AUDIT kind=%s", MEMORY_CPP)
        self.assertIn("BULLET_TRANSFORM_AUDIT_POLICY", MEMORY_CPP)
        self.assertIn("mark_is_non_destructive=1", MEMORY_CPP)
        self.assertIn("canonical_path_always_runs=1", MEMORY_CPP)
        for field in (
            "advance_calls=%llu",
            "fired_update_calls=%llu",
            "terminal_index_returns=%llu",
            "terminal_none_returns=%llu",
            "active_blocked_returns=%llu",
            "disabled_records_skipped=%llu",
            "records_dispatched=%llu",
            "active_transform_starts=%llu",
            "child_spawn_records=%llu",
            "spawn_program_writes=%llu",
            "whole_slot_resets=%llu",
        ):
            self.assertIn(field, MEMORY_CPP)


if __name__ == "__main__":
    unittest.main()
