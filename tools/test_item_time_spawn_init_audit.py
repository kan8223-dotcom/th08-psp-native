#!/usr/bin/env python3
"""Host-only model and source contracts for script-68 spawn-init audit.

The audit is intentionally observational: the stock initializer owns the real
Item VM, while a candidate mutates only a full pre-call copy.  These tests lock
down the base/tail boundary, strict ANM identity, generation guard, exact
comparison, per-frame workload counters, and default-OFF build identity.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
ITEM_CPP = ROOT / "src" / "ItemManager.cpp"
ITEM_HPP = ROOT / "src" / "ItemManager.hpp"
ANM_CPP = ROOT / "src" / "AnmManager.cpp"
ANM_HPP = ROOT / "src" / "AnmManager.hpp"
PERF_CPP = ROOT / "psp" / "render_perf_telemetry.cpp"
PERF_HPP = ROOT / "psp" / "render_perf_telemetry.hpp"
MEMORY_CPP = ROOT / "psp" / "memory_telemetry.cpp"
MAIN_CPP = ROOT / "psp" / "main.cpp"
MAKEFILE = ROOT / "Makefile.psp"

VM_BYTES = 0x2A4
BASE_BYTES = 0x208


def modeled_shadow(pre_call: bytes) -> bytes:
    """Model only the critical canonical lifetime boundary.

    Initialize clears AnmVmBase [0, 0x208), while the script-related tail is
    stale-slot storage except for explicit writes.  Exact field values are
    covered by the source contract and runtime full-VM comparison.
    """

    result = bytearray(pre_call)
    result[:BASE_BYTES] = bytes(BASE_BYTES)
    # Model explicit tail writes without erasing unrelated tail storage.
    result[0x214:0x218] = (68).to_bytes(4, "little", signed=True)
    result[0x288:0x28C] = (0).to_bytes(4, "little", signed=True)
    return bytes(result)


@dataclass
class FramePeak:
    observed: int = 0xFFFFFFFF
    current: int = 0
    peak: int = 0
    peak_frame: int = 0xFFFFFFFF

    def note(self, stage_frame: int) -> None:
        if self.observed != stage_frame:
            self.observed = stage_frame
            self.current = 0
        self.current += 1
        if self.current > self.peak:
            self.peak = self.current
            self.peak_frame = stage_frame


class ItemTimeSpawnInitModelTest(unittest.TestCase):
    def test_base_clear_preserves_unwritten_tail(self) -> None:
        original = bytes((index * 37 + 11) & 0xFF for index in range(VM_BYTES))
        shadow = modeled_shadow(original)
        self.assertEqual(shadow[:BASE_BYTES], bytes(BASE_BYTES))
        self.assertEqual(shadow[BASE_BYTES:0x214], original[BASE_BYTES:0x214])
        self.assertEqual(shadow[0x218:0x288], original[0x218:0x288])
        self.assertEqual(shadow[0x28C:], original[0x28C:])

    def test_whole_vm_detector_observes_tail_difference(self) -> None:
        original = bytes((index * 13 + 3) & 0xFF for index in range(VM_BYTES))
        canonical = modeled_shadow(original)
        candidate = bytearray(canonical)
        candidate[-1] ^= 1
        self.assertNotEqual(canonical, bytes(candidate))

    def test_per_frame_peak_retains_burst_frame(self) -> None:
        peak = FramePeak()
        for frame, count in ((490, 3), (491, 686), (492, 17)):
            for _ in range(count):
                peak.note(frame)
        self.assertEqual(peak.peak, 686)
        self.assertEqual(peak.peak_frame, 491)


class ItemTimeSpawnInitSourceContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.item_cpp = ITEM_CPP.read_text(encoding="utf-8")
        cls.item_hpp = ITEM_HPP.read_text(encoding="utf-8")
        cls.anm_cpp = ANM_CPP.read_text(encoding="utf-8")
        cls.anm_hpp = ANM_HPP.read_text(encoding="utf-8")
        cls.perf_cpp = PERF_CPP.read_text(encoding="utf-8")
        cls.perf_hpp = PERF_HPP.read_text(encoding="utf-8")
        cls.memory_cpp = MEMORY_CPP.read_text(encoding="utf-8")
        cls.main_cpp = MAIN_CPP.read_text(encoding="utf-8")
        cls.makefile = MAKEFILE.read_text(encoding="utf-8")

    def test_feature_is_default_off_and_stamped(self) -> None:
        gate = "TH08_PSP_ITEM_TIME_SPAWN_INIT_AUDIT"
        self.assertIn(f"{gate} ?= 0", self.makefile)
        self.assertIn(f"-D{gate}=1", self.makefile)
        self.assertIn("item-time-spawn-init-audit-$(", self.makefile)
        self.assertIn("ITEM_TIME_SPAWN_INIT_AUDIT_CONFIG_STAMP", self.makefile)
        self.assertIn("TH08_PSP_FEATURE_ITEM_TIME_SPAWN_INIT_AUDIT", self.main_cpp)

    def test_strict_stock_script_and_generation_identity(self) -> None:
        for text in (
            "scriptIndex != kItemTimeScriptIndex",
            "sprite->intArgs[0] != kItemTimeInitialSpriteIndex",
            "color->opcode != AnmOpcode_ColorTime",
            "alpha->opcode != AnmOpcode_AlphaTime",
            "stop->opcode == AnmOpcode_Static",
            "PspLoadGenerationForSpawnInitAudit",
            "PspLoadReadyForSpawnInitAudit",
            "generationBefore != generationAfter",
            "generationFinal != generationAfter",
        ):
            self.assertIn(text, self.item_cpp + self.anm_cpp + self.anm_hpp)

    def test_canonical_is_authoritative_and_candidate_is_full_copy(self) -> None:
        start = self.item_cpp.index("void SetAndAuditItemTimeSpawnScript")
        end = self.item_cpp.index("} // namespace", start)
        audit = self.item_cpp[start:end]
        canonical = audit.index("owner->SetAndExecuteScriptIdx(&item->sprite")
        shadow = audit.index("InitializeItemTimeScript68Shadow")
        self.assertLess(canonical, shadow)
        self.assertIn("memcpy(&before, &item->sprite, sizeof(before))", audit)
        self.assertIn("memcpy(&shadow, &before, sizeof(shadow))", audit)
        self.assertIn("memcmp(&item->sprite, &shadow, sizeof(AnmVm))", audit)
        self.assertNotIn("memcpy(&item->sprite", audit)

    def test_candidate_clears_only_base_and_preserves_exact_tail(self) -> None:
        start = self.item_cpp.index("ZunBool InitializeItemTimeScript68Shadow")
        end = self.item_cpp.index("bool ItemTimeSpawnPointerFieldsMatch", start)
        shadow = self.item_cpp[start:end]
        self.assertIn(
            "memset(static_cast<AnmVmBase *>(vm), 0, sizeof(AnmVmBase))",
            shadow,
        )
        self.assertNotIn("memset(vm, 0, sizeof(AnmVm))", shadow)
        self.assertIn("vm->currentTimeInScript++", shadow)
        self.assertIn("++*scriptsExecuted", shadow)
        self.assertIn("++*scriptsStarted", shadow)
        self.assertIn("return FALSE", shadow)

    def test_runtime_audit_exposes_every_exactness_gate(self) -> None:
        for field in (
            "candidates",
            "eligible",
            "canonicalFallbacks",
            "fullVmMatches",
            "fullVmMismatches",
            "fieldMismatches",
            "candidateReturnContractMismatches",
            "pointerMismatches",
            "counterMismatches",
            "spriteMismatches",
            "loadGenerationMismatches",
            "compactRangeFallbacks",
            "peakCandidatesPerFrame",
            "peakCandidatesStageFrame",
            "peakEligiblePerFrame",
            "peakEligibleStageFrame",
        ):
            self.assertIn(f"u32 {field};", self.item_hpp)
        self.assertIn("ITEM_TIME_SPAWN_INIT_AUDIT kind=%s", self.memory_cpp)
        self.assertIn("authoritative=canonical candidate=shadow_only", self.memory_cpp)
        self.assertIn("vm_bytes=%u base_clear_bytes=%u", self.memory_cpp)

    def test_stage_sampler_accumulates_peaks_and_resets_all_fields(self) -> None:
        for field in (
            "itemTimeSpawnInitCandidates",
            "itemTimeSpawnInitEligible",
            "itemTimeSpawnInitFullVmMismatches",
            "itemTimeSpawnInitFieldMismatches",
            "itemTimeSpawnInitFallbacks",
        ):
            self.assertIn(f"std::uint64_t {field};", self.perf_hpp)
            self.assertIn(f"std::uint32_t {field};", self.perf_hpp)
            self.assertIn(f"destination->{field} +=", self.perf_cpp)
            self.assertIn(f"TH08_PSP_RENDER_PERF_MAX({field})", self.perf_cpp)
            self.assertIn(f"gRenderPerfCurrentFrame.{field} = 0", self.perf_cpp)
        self.assertIn("RenderPerfNoteItemTimeSpawnInitAudit", self.perf_hpp)

    def test_audit_and_product_are_separate_and_mutually_exclusive(self) -> None:
        self.assertIn("TH08_PSP_ITEM_TIME_SPAWN_INIT_PRODUCT_ENABLED",
                      self.item_cpp)
        self.assertIn("TH08_PSP_ITEM_TIME_SPAWN_INIT_FASTPATH", self.item_hpp)
        self.assertIn("TH08_PSP_ITEM_TIME_SPAWN_INIT_FASTPATH", self.makefile)
        self.assertIn("spawn-init audit and fast path are mutually exclusive",
                      self.item_hpp)
        start = self.item_cpp.index("void SetAndAuditItemTimeSpawnScript")
        end = self.item_cpp.index("} // namespace", start)
        audit = self.item_cpp[start:end]
        self.assertIn("owner->SetAndExecuteScriptIdx(&item->sprite", audit)
        self.assertNotIn("InitializeItemTimeScript68Fastpath", audit)


if __name__ == "__main__":
    unittest.main(verbosity=2)
