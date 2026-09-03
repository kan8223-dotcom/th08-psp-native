#!/usr/bin/env python3
"""Model and source-contract tests for the ITEM_TIME script-68 idle audit.

The feature under test is observational.  Production state must always come
from the canonical ``AnmManager::ExecuteScript`` call; a private shadow mirrors
only the no-instruction tail for script time 1..29 and is compared bytewise.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
import copy
import math
import struct
import unittest


ROOT = Path(__file__).resolve().parents[1]
ITEM_CPP = ROOT / "src" / "ItemManager.cpp"
ITEM_HPP = ROOT / "src" / "ItemManager.hpp"


def f32(value: float) -> float:
    return struct.unpack("<f", struct.pack("<f", value))[0]


def u32(value: int) -> int:
    return value & 0xFFFFFFFF


@dataclass
class Timer:
    previous: int = -999
    sub_frame: float = 0.0
    current: int = 0

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
class VmModel:
    uv_pos: list[float] = field(default_factory=lambda: [0.0, 0.0])
    uv_vel: list[float] = field(default_factory=lambda: [0.0, 0.0])
    timer: Timer = field(default_factory=Timer)
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


def canonical_idle(vm: VmModel, multiplier: float, counter: int) -> tuple[bool, int]:
    # This is the reachable tail of ExecuteScript once all seven interpolation
    # end timers, angle velocity, and scale growth have failed their branches.
    vm.uv_pos[0] = add_and_wrap(vm.uv_pos[0], vm.uv_vel[0])
    vm.uv_pos[1] = add_and_wrap(vm.uv_pos[1], vm.uv_vel[1])
    vm.timer.tick(multiplier)
    return False, u32(counter + 1)


def fast_shadow_idle(vm: VmModel, multiplier: float, counter: int) -> tuple[bool, int]:
    vm.uv_pos[0] = add_and_wrap(vm.uv_pos[0], vm.uv_vel[0])
    vm.uv_pos[1] = add_and_wrap(vm.uv_pos[1], vm.uv_vel[1])
    vm.timer.tick(multiplier)
    return False, u32(counter + 1)


class ItemTimeIdleModelTest(unittest.TestCase):
    def compare(self, vm: VmModel, multiplier: float, counter: int) -> None:
        canonical = copy.deepcopy(vm)
        shadow = copy.deepcopy(vm)
        canonical_return, canonical_counter = canonical_idle(
            canonical, multiplier, counter
        )
        shadow_return, shadow_counter = fast_shadow_idle(
            shadow, multiplier, counter
        )
        self.assertEqual(canonical.raw(), shadow.raw())
        self.assertEqual(canonical_return, shadow_return)
        self.assertEqual(canonical_counter, shadow_counter)

    def test_idle_window_endpoints_and_fractional_ticks(self) -> None:
        for current in (1, 2, 28, 29):
            for multiplier in (1.0, 0.75, 0.5, 0.25):
                vm = VmModel(timer=Timer(previous=current - 1, current=current))
                self.compare(vm, multiplier, 0x10203040)

    def test_uv_positive_and_negative_wrap(self) -> None:
        cases = (
            ([0.99, 0.01], [0.02, -0.02]),
            ([1.0, -0.25], [0.0, 0.0]),
            ([-0.0, 0.0], [0.0, -0.0]),
            ([0.25, 0.75], [0.0, 0.0]),
        )
        for position, velocity in cases:
            vm = VmModel(
                uv_pos=[f32(value) for value in position],
                uv_vel=[f32(value) for value in velocity],
                timer=Timer(previous=10, sub_frame=f32(0.75), current=11),
            )
            self.compare(vm, 0.5, 0xFFFFFFFF)

    def test_bytewise_detector_observes_any_shadow_difference(self) -> None:
        canonical = VmModel(timer=Timer(previous=6, current=7))
        shadow = copy.deepcopy(canonical)
        canonical_idle(canonical, 1.0, 0)
        fast_shadow_idle(shadow, 1.0, 0)
        shadow.untouched = bytes([shadow.untouched[0] ^ 1]) + shadow.untouched[1:]
        self.assertNotEqual(canonical.raw(), shadow.raw())


class ItemTimeIdleSourceContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = ITEM_CPP.read_text(encoding="utf-8")
        cls.header = ITEM_HPP.read_text(encoding="utf-8")

    def test_feature_is_psp_audit_only(self) -> None:
        gate = "TH08_PSP_ITEM_TIME_ANM_IDLE_AUDIT"
        self.assertIn("defined(PSP) && defined(" + gate + ")", cls_source := self.source)
        self.assertIn("ItemTimeAnmIdleAuditStats", self.header)
        self.assertNotIn("TH08_PSP_ITEM_TIME_ANM_IDLE_FASTPATH", cls_source)

    def test_strict_stock_script68_identity(self) -> None:
        for text in (
            "item->sprite.scriptIndex != 68",
            "sprite->intArgs[0] != 179",
            "color->opcode != AnmOpcode_ColorTime",
            "alpha->opcode != AnmOpcode_AlphaTime",
            "stop->opcode == AnmOpcode_Static",
            "item->sprite.currentTimeInScript.current >= 1",
            "item->sprite.currentTimeInScript.current <= 29",
            "item->sprite.currentInstruction == idleInstruction",
        ):
            self.assertIn(text, self.source)

    def test_dynamic_gate_covers_every_generic_tail_branch(self) -> None:
        for text in (
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
            self.assertIn(text, self.source)

    def test_shadow_preserves_uv_tick_and_synthetic_counter(self) -> None:
        shadow_start = self.source.index("ZunBool ExecuteItemTimeAnmIdleShadow")
        shadow_end = self.source.index("ZunBool ExecuteItemAnmWithIdleAudit")
        shadow = self.source[shadow_start:shadow_end]
        for text in (
            "vm->uvScrollPos.x += vm->uvScrollVel.x",
            "vm->uvScrollPos.y += vm->uvScrollVel.y",
            "vm->currentTimeInScript++",
            "++*scriptsExecuted",
            "return FALSE",
        ):
            self.assertIn(text, shadow)
        self.assertNotIn("g_AnmManager->scriptsExecutedThisFrame++", shadow)

    def test_authoritative_path_is_canonical_and_shadow_is_full_vm(self) -> None:
        audit_start = self.source.index("ZunBool ExecuteItemAnmWithIdleAudit")
        audit_end = self.source.index("} // namespace", audit_start)
        audit = self.source[audit_start:audit_end]
        canonical_call = audit.index("g_AnmManager->ExecuteScript(&item->sprite)")
        shadow_call = audit.index("ExecuteItemTimeAnmIdleShadow")
        self.assertLess(canonical_call, shadow_call)
        self.assertIn("memcpy(&fastShadow, &before, sizeof(fastShadow))", audit)
        self.assertIn(
            "memcmp(&item->sprite, &fastShadow, sizeof(AnmVm)) == 0", audit
        )
        self.assertNotIn("memcpy(&item->sprite", audit)

    def test_reset_and_public_match_mismatch_counters_exist(self) -> None:
        self.assertIn("ResetItemTimeAnmIdleAudit();", self.source)
        for field in (
            "eligibleIdleCalls",
            "fullVmMatches",
            "fullVmMismatches",
            "returnMatches",
            "returnMismatches",
            "counterMatches",
            "counterMismatches",
        ):
            self.assertIn(f"u32 {field};", self.header)


if __name__ == "__main__":
    unittest.main(verbosity=2)
