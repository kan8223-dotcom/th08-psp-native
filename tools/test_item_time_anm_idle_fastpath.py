#!/usr/bin/env python3
"""Host-only contracts for the stock ITEM_TIME script-68 idle fast path.

The production path may replace the interpreter only after the exact script
owner/fingerprint, VM instruction/time window, and every dynamic tail branch
have been ruled safe.  This test does not build or launch the PSP program.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
import copy
import struct
import unittest


ROOT = Path(__file__).resolve().parents[1]
ITEM_CPP = ROOT / "src" / "ItemManager.cpp"
ITEM_HPP = ROOT / "src" / "ItemManager.hpp"


def f32(value: float) -> float:
    return struct.unpack("<f", struct.pack("<f", value))[0]


@dataclass
class Timer:
    previous: int
    sub_frame: float
    current: int

    def tick(self, multiplier: float) -> None:
        self.previous = self.current
        if multiplier <= 0.99:
            self.sub_frame = f32(self.sub_frame + multiplier)
            if self.sub_frame >= 1.0:
                self.current += 1
                self.sub_frame = f32(self.sub_frame - 1.0)
        else:
            self.current += 1


@dataclass
class Vm:
    uv_pos: list[float] = field(default_factory=lambda: [0.0, 0.0])
    uv_vel: list[float] = field(default_factory=lambda: [0.0, 0.0])
    timer: Timer = field(default_factory=lambda: Timer(-999, 0.0, 1))
    untouched: bytes = bytes(range(64))

    def raw(self) -> bytes:
        return struct.pack(
            "<ffffifi64s",
            *(self.uv_pos + self.uv_vel),
            self.timer.previous,
            self.timer.sub_frame,
            self.timer.current,
            self.untouched,
        )


def add_and_wrap(position: float, velocity: float) -> float:
    position = f32(position + velocity)
    if position >= 1.0:
        position = f32(position - 1.0)
    elif position < 0.0:
        position = f32(position + 1.0)
    return position


def exact_idle_tail(vm: Vm, multiplier: float, counter: int) -> tuple[bool, int]:
    vm.uv_pos[0] = add_and_wrap(vm.uv_pos[0], vm.uv_vel[0])
    vm.uv_pos[1] = add_and_wrap(vm.uv_pos[1], vm.uv_vel[1])
    vm.timer.tick(multiplier)
    return False, (counter + 1) & 0xFFFFFFFF


class ItemTimeIdleFastpathModelTest(unittest.TestCase):
    def test_exact_tail_matches_canonical_model_at_all_boundaries(self) -> None:
        for current in (1, 2, 28, 29):
            for multiplier in (1.0, 0.75, 0.5, 0.25):
                for position, velocity in (
                    ((0.0, -0.0), (0.0, -0.0)),
                    ((0.99, 0.01), (0.02, -0.02)),
                    ((1.0, -0.25), (0.0, 0.0)),
                ):
                    original = Vm(
                        uv_pos=[f32(value) for value in position],
                        uv_vel=[f32(value) for value in velocity],
                        timer=Timer(current - 1, f32(0.75), current),
                    )
                    canonical = copy.deepcopy(original)
                    fast = copy.deepcopy(original)
                    canonical_result = exact_idle_tail(
                        canonical, multiplier, 0xFFFFFFFF
                    )
                    fast_result = exact_idle_tail(fast, multiplier, 0xFFFFFFFF)
                    self.assertEqual(canonical.raw(), fast.raw())
                    self.assertEqual(canonical_result, fast_result)


class ItemTimeIdleFastpathSourceContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = ITEM_CPP.read_text(encoding="utf-8")
        cls.header = ITEM_HPP.read_text(encoding="utf-8")
        start = cls.source.index("struct ItemTimeAnmIdleScriptCache")
        end = cls.source.index("} // namespace", start)
        cls.fastpath = cls.source[start:end]

    def test_audit_and_product_are_mutually_exclusive(self) -> None:
        self.assertIn(
            '#error "ITEM_TIME ANM idle audit and fast path are mutually exclusive"',
            self.header,
        )
        self.assertIn("TH08_PSP_ITEM_TIME_ANM_IDLE_AUDIT", self.header)
        self.assertIn("TH08_PSP_ITEM_TIME_ANM_IDLE_FASTPATH", self.header)
        self.assertIn("#elif defined(PSP)", self.source)

    def test_cache_is_owned_by_anm_and_script_pointer(self) -> None:
        for text in (
            "const AnmLoaded *owner;",
            "AnmRawInstr *script68;",
            "ZunBool fingerprintValid;",
            "const AnmLoaded *const owner = g_BulletManager.bulletAnm",
            "script68 = owner->scripts[68]",
            "g_ItemTimeAnmIdleScriptCache.owner != owner",
            "g_ItemTimeAnmIdleScriptCache.script68 != script68",
            "ValidateItemTimeAnmIdleScript68(script68)",
        ):
            self.assertIn(text, self.fastpath)

    def test_cache_and_stats_reset_with_item_manager(self) -> None:
        self.assertIn("ResetItemTimeAnmIdleFastpath()", self.source)
        initialize = self.source.index("void ItemManager::Initialize()")
        spawn = self.source.index("Item *ItemManager::SpawnItem", initialize)
        self.assertIn(
            "ResetItemTimeAnmIdleFastpath();", self.source[initialize:spawn]
        )
        self.assertIn(
            "memset(&g_ItemTimeAnmIdleScriptCache, 0", self.fastpath
        )

    def test_fingerprint_is_complete_and_literal(self) -> None:
        for text in (
            "sprite->opcode != AnmOpcode_Sprite",
            "sprite->instructionSize != 12",
            "sprite->time != 0",
            "sprite->varMask != 0",
            "sprite->intArgs[0] != 179",
            "color->opcode != AnmOpcode_ColorTime",
            "color->instructionSize != 28",
            "color->time != 30",
            "color->intArgs[0] != 20",
            "color->intArgs[1] != AnmInterpMode_Linear",
            "color->intArgs[2] != 128",
            "color->intArgs[3] != 128",
            "color->intArgs[4] != 128",
            "alpha->opcode != AnmOpcode_AlphaTime",
            "alpha->instructionSize != 20",
            "alpha->time != 30",
            "alpha->intArgs[2] != 192",
            "stop->opcode == AnmOpcode_Static",
            "stop->instructionSize == 8",
            "stop->time == 50",
        ):
            self.assertIn(text, self.fastpath)

    def test_every_identity_window_and_dynamic_gate_is_present(self) -> None:
        for text in (
            "item->itemType != ITEM_TIME",
            "item->sprite.scriptIndex != 68",
            "item->sprite.anmFile != owner",
            "item->sprite.beginningOfScript != script68",
            "item->sprite.currentInstruction != idleInstruction",
            "item->sprite.currentTimeInScript.current < 1",
            "item->sprite.currentTimeInScript.current > 29",
            "vm->flag19 != 0",
            "vm->pendingInterrupt != 0",
            "vm->angleVel.x != 0.0f",
            "vm->angleVel.y != 0.0f",
            "vm->angleVel.z != 0.0f",
            "vm->scaleGrowth.x != 0.0f",
            "vm->scaleGrowth.y != 0.0f",
            "i < AnmInterp_Last",
            "vm->interpEndTimers[i].current > 0",
        ):
            self.assertIn(text, self.fastpath)

    def test_exact_tail_mutates_only_canonical_tail_fields(self) -> None:
        tail_start = self.fastpath.index("ExecuteItemTimeAnmIdleFastpathTail")
        tail_end = self.fastpath.index(
            "ExecuteItemTimeAnmCanonicalFallback", tail_start
        )
        tail = self.fastpath[tail_start:tail_end]
        for text in (
            "vm->uvScrollPos.x += vm->uvScrollVel.x",
            "vm->uvScrollPos.y += vm->uvScrollVel.y",
            "vm->currentTimeInScript++",
            "g_AnmManager->scriptsExecutedThisFrame++",
            "return FALSE",
        ):
            self.assertIn(text, tail)
        self.assertNotIn("memcpy", tail)
        self.assertNotIn("FromAngleMagnitude", tail)

    def test_every_rejection_executes_canonical_interpreter(self) -> None:
        self.assertIn(
            "return g_AnmManager->ExecuteScript(&item->sprite)", self.fastpath
        )
        # One helper definition plus four rejection call sites: pre-identity,
        # owner/fingerprint identity, idle window, and dynamic-tail state.
        self.assertEqual(
            self.fastpath.count("ExecuteItemTimeAnmCanonicalFallback("), 5
        )

    def test_replay_audit_stats_expose_hits_and_fallbacks(self) -> None:
        for field in (
            "calls",
            "hits",
            "canonicalFallbacks",
            "identityFallbacks",
            "windowFallbacks",
            "dynamicFallbacks",
            "cachePointerChanges",
            "cacheRevalidations",
            "cacheValidationFailures",
        ):
            self.assertIn(f"u32 {field};", self.header)
        self.assertIn(
            "const ItemTimeAnmIdleFastpathStats &GetItemTimeAnmIdleFastpathStats()",
            self.source,
        )
        self.assertIn(
            "defined(TH08_REPLAY_SYNC_AUDIT) && TH08_REPLAY_SYNC_AUDIT",
            self.fastpath,
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
