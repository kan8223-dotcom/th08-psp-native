#!/usr/bin/env python3
"""Host differential/static gates for the PSP Bullet terminal sidecar.

The executable PSP path is deliberately tiny: one immutable-program terminal
bit per Bullet.  This suite stress-tests randomized spawn/update/reset/repair
lifecycles against a canonical model, then binds that model to every real
program/reset writer and to the build/telemetry fingerprint.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import random
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
BULLET_CPP = (ROOT / "src" / "BulletManager.cpp").read_text(encoding="utf-8")
BULLET_HPP = (ROOT / "src" / "BulletManager.hpp").read_text(encoding="utf-8")
ARENA_HPP = (ROOT / "psp" / "stage_pool_arena.hpp").read_text(encoding="utf-8")
ARENA_CPP = (ROOT / "psp" / "stage_pool_arena.cpp").read_text(encoding="utf-8")
MAKEFILE = (ROOT / "Makefile.psp").read_text(encoding="utf-8")
MAIN_CPP = (ROOT / "psp" / "main.cpp").read_text(encoding="utf-8")
MEMORY_CPP = (ROOT / "psp" / "memory_telemetry.cpp").read_text(
    encoding="utf-8"
)
FEATURE = "TH08_PSP_BULLET_TRANSFORM_TERMINAL_FASTPATH"


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


NONE = 0
ACTIVE_KINDS = frozenset((1, 2, 4, 8))
INSTANT_KINDS = frozenset((16, 32, 64))


@dataclass(eq=True)
class ModelBullet:
    program: tuple[tuple[int, bool], ...] = ((NONE, True),) * 18
    flags: int = 0
    index: int = 0
    active: int = 0
    live: bool = False


def canonical_advance(bullet: ModelBullet) -> str:
    while True:
        if bullet.index >= 18:
            return "terminal_index"
        kind, allow_while_active = bullet.program[bullet.index]
        if kind == NONE:
            return "terminal_none"
        if not allow_while_active and bullet.active != 0:
            return "active_blocked"
        if bullet.flags & kind == 0:
            bullet.index += 1
            continue
        if kind in ACTIVE_KINDS:
            bullet.active |= kind
            bullet.index += 1
            return "active_started"
        if kind in INSTANT_KINDS:
            bullet.index += 1
            continue
        raise AssertionError(f"unmodelled transform kind {kind}")


def advance_and_mark(bullet: ModelBullet, terminal: list[bool], slot: int) -> str:
    result = canonical_advance(bullet)
    if result in ("terminal_index", "terminal_none"):
        terminal[slot] = True
    return result


def authoritative_terminal(bullet: ModelBullet) -> bool:
    if bullet.index >= 18:
        return True
    if bullet.index < 0:
        return False
    return bullet.program[bullet.index][0] == NONE


class BulletTransformTerminalTests(unittest.TestCase):
    def test_random_differential_spawn_update_reset_and_repair_lifecycle(self) -> None:
        rng = random.Random(0x54483038)
        slot_count = 1536
        canonical = [ModelBullet() for _ in range(slot_count)]
        optimized = [ModelBullet() for _ in range(slot_count)]
        terminal = [False] * slot_count

        for _ in range(120_000):
            slot = rng.randrange(slot_count)
            operation = rng.randrange(100)

            if operation < 20 or not canonical[slot].live:
                records: list[tuple[int, bool]] = []
                for _record in range(18):
                    pick = rng.randrange(10)
                    if pick < 2:
                        kind = NONE
                    elif pick < 7:
                        kind = tuple(ACTIVE_KINDS)[rng.randrange(len(ACTIVE_KINDS))]
                    else:
                        kind = tuple(INSTANT_KINDS)[rng.randrange(len(INSTANT_KINDS))]
                    records.append((kind, bool(rng.getrandbits(1))))
                flags = 0
                for kind in ACTIVE_KINDS | INSTANT_KINDS:
                    if rng.getrandbits(1):
                        flags |= kind
                start = rng.randrange(19)
                fresh = ModelBullet(tuple(records), flags, start, 0, True)
                canonical[slot] = ModelBullet(**fresh.__dict__)
                optimized[slot] = ModelBullet(**fresh.__dict__)

                # Recycled-slot writer: clear before the immutable program is
                # published, then retain the mandatory canonical first call.
                terminal[slot] = False
                canonical_advance(canonical[slot])
                advance_and_mark(optimized[slot], terminal, slot)
            elif operation < 32:
                # All three production memset writers have this same lifecycle
                # meaning; source checks below bind each real site to the clear.
                canonical[slot] = ModelBullet()
                optimized[slot] = ModelBullet()
                terminal[slot] = False
            else:
                if rng.randrange(5) == 0:
                    canonical[slot].active = 0
                    optimized[slot].active = 0

                canonical_advance(canonical[slot])
                if not terminal[slot]:
                    advance_and_mark(optimized[slot], terminal, slot)

                # Audit builds repair only unsafe false positives; false
                # negatives are safe and simply re-run the canonical path.
                if rng.randrange(4000) == 0:
                    terminal[slot] = True
                    if not authoritative_terminal(optimized[slot]):
                        terminal[slot] = False
                if rng.randrange(4000) == 0:
                    terminal[slot] = False

            self.assertEqual(canonical[slot], optimized[slot])
            if terminal[slot]:
                self.assertTrue(authoritative_terminal(optimized[slot]))

    def test_sidecar_is_exactly_192_bytes_without_arena_growth(self) -> None:
        self.assertIn("kBulletPoolLogicalCount = 0x600U", ARENA_HPP)
        self.assertIn("kBulletTransformTerminalBytes", ARENA_HPP)
        self.assertIn("kBulletTransformTerminalBytes == 192U", ARENA_HPP)
        self.assertIn("u32 transformTerminalBits[kPspBulletActiveWordCount];", BULLET_CPP)
        self.assertIn("kBulletTransformTerminalBytes", BULLET_CPP)
        self.assertIn("kBulletRuntimeCacheEnd == 0xd81e80U", ARENA_CPP)
        self.assertIn("kTailGuardOffset == 0xd82000U", ARENA_CPP)
        self.assertIn("kReserveBytes == 0xd83000U", ARENA_CPP)

    def test_feature_is_default_off_stamped_dependent_and_audit_exclusive(self) -> None:
        self.assertRegex(MAKEFILE, rf"(?m)^{FEATURE}\s*\?=\s*0\s*$")
        self.assertIn(f"ifeq ($({FEATURE}),1)", MAKEFILE)
        self.assertIn(f"-D{FEATURE}=1", MAKEFILE)
        self.assertIn(
            f"$(error {FEATURE}=1 requires TH08_PSP_BULLET_FASTPATH=1)",
            MAKEFILE,
        )
        self.assertIn(
            "$(error TH08_PSP_BULLET_TRANSFORM_TERMINAL_FASTPATH and "
            "TH08_PSP_BULLET_TRANSFORM_AUDIT are mutually exclusive)",
            MAKEFILE,
        )
        self.assertIn("bullet-transform-terminal-fastpath-0.stamp", MAKEFILE)
        self.assertIn("bullet-transform-terminal-fastpath-1.stamp", MAKEFILE)
        self.assertIn("$(BULLET_TRANSFORM_TERMINAL_FASTPATH_CONFIG_STAMP)", MAKEFILE)

    def test_spawn_keeps_initial_advance_and_clears_before_program_write(self) -> None:
        spawn = BULLET_CPP[
            BULLET_CPP.index("BulletManager::SpawnSingleBullet") :
            BULLET_CPP.index("// FUNCTION: th08 0x42fe70")
        ]
        clear = spawn.index("PspBulletTransformTerminalClearForSpawn(bullet);")
        program_write = spawn.index("memcpy(bullet->transforms")
        index_write = spawn.index(
            "bullet->transformIndex = descriptor->transformStartIndex;"
        )
        initial_advance = spawn.index("bullet->AdvanceTransformProgram();")
        self.assertLess(clear, program_write)
        self.assertLess(program_write, index_write)
        self.assertLess(index_write, initial_advance)
        self.assertEqual(spawn.count("bullet->AdvanceTransformProgram();"), 1)

    def test_only_two_irreversible_terminal_returns_mark_and_active_block_does_not(self) -> None:
        advance = function_body(BULLET_CPP, "void Bullet::AdvanceTransformProgram()")
        self.assertEqual(advance.count("PspBulletTransformTerminalMarkIndex(this);"), 1)
        self.assertEqual(advance.count("PspBulletTransformTerminalMarkNone(this);"), 1)
        blocked = advance[
            advance.index("if (record->allowWhileActive == 0") :
            advance.index("if ((this->transformFlags & record->kind) == 0")
        ]
        self.assertNotIn("PspBulletTransformTerminalMark", blocked)

    def test_skip_is_fired_only_and_all_three_whole_slot_writers_clear(self) -> None:
        update = function_body(
            BULLET_CPP,
            "ChainCallbackResult BulletManager::OnUpdate(BulletManager *bulletManager)",
        )
        skip = "PspBulletTransformTerminalSkip("
        self.assertEqual(BULLET_CPP.count(skip), 2)  # helper plus one call site
        update_call = update.index(skip)
        self.assertLess(update.index("case BULLET_STATE_FIRED:"), update_call)
        self.assertLess(update_call, update.index("bullet->AdvanceTransformProgram();"))

        self.assertEqual(BULLET_CPP.count("memset(bullet, 0, sizeof(Bullet));"), 3)
        reset_pattern = re.compile(
            r"memset\(bullet, 0, sizeof\(Bullet\)\);\s*"
            r"#if defined\(TH08_PSP_BULLET_TRANSFORM_TERMINAL_FASTPATH_ENABLED\)\s*"
            r"PspBulletTransformTerminalClearForWholeSlotReset\(bullet\);"
        )
        self.assertEqual(len(reset_pattern.findall(BULLET_CPP)), 3)

    def test_allegrex_hot_path_records_one_32_bit_outcome_not_two_u64_counts(self) -> None:
        counters_start = BULLET_CPP.index(
            "struct PspBulletTransformTerminalCounters"
        )
        counters = BULLET_CPP[
            counters_start : BULLET_CPP.index("};", counters_start) + 2
        ]
        self.assertIn("u32 hits;", counters)
        self.assertNotIn("unsigned long long", counters)

        skip = function_body(
            BULLET_CPP, "inline bool PspBulletTransformTerminalSkip"
        )
        self.assertNotIn("firedCalls", skip)
        self.assertEqual(skip.count("++g_PspBulletTransformTerminalTelemetry."), 3)

        peek = function_body(
            BULLET_CPP, "PspPeekBulletTransformTerminalTelemetry()"
        )
        self.assertIn(
            "snapshot.firedCalls = snapshot.hits + snapshot.misses + snapshot.fallbacks;",
            peek,
        )

    def test_replay_audit_repairs_only_false_positive_bits(self) -> None:
        audit = function_body(BULLET_CPP, "void PspBulletRuntimeAuditAndRepair")
        self.assertIn("PspBulletTransformTerminalRawBit(index)", audit)
        self.assertIn("bullet.transformIndex >= 18", audit)
        self.assertIn("BULLET_TRANSFORM_NONE", audit)
        self.assertIn("PspBulletTransformTerminalSetBit(index, false);", audit)
        self.assertIn("invariantWitnesses", audit)
        self.assertIn("repairs", audit)

    def test_boot_and_interval_telemetry_bind_the_product(self) -> None:
        self.assertIn(
            "TH08_PSP_FEATURE_BULLET_TRANSFORM_TERMINAL_FASTPATH", MAIN_CPP
        )
        self.assertIn("BULLET_TRANSFORM_TERMINAL_FASTPATH=%d", MAIN_CPP)
        for api in (
            "PspPeekBulletTransformTerminalTelemetry",
            "PspTakeBulletTransformTerminalTelemetry",
            "PspResetBulletTransformTerminalTelemetry",
        ):
            self.assertIn(api, BULLET_HPP)
            self.assertIn(api, BULLET_CPP)
        self.assertGreaterEqual(
            MEMORY_CPP.count("th08::PspResetBulletTransformTerminalTelemetry();"),
            2,
        )
        self.assertIn("BULLET_TRANSFORM_TERMINAL kind=%s", MEMORY_CPP)
        self.assertIn("BULLET_TRANSFORM_TERMINAL_POLICY", MEMORY_CPP)
        for field in (
            "fired_calls=%llu",
            "hits=%llu",
            "misses=%llu",
            "fallbacks=%llu",
            "invariant_witnesses=%llu",
            "repairs=%llu",
            "terminal_index_marks=%llu",
            "terminal_none_marks=%llu",
            "mark_fallbacks=%llu",
            "spawn_program_clears=%llu",
            "whole_slot_clears=%llu",
        ):
            self.assertIn(field, MEMORY_CPP)


if __name__ == "__main__":
    unittest.main()
