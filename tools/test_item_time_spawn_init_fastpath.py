#!/usr/bin/env python3
"""Host-only model/source contracts for ITEM_TIME spawn-init fastpath.

The product is deliberately narrower than the r038 shadow proof: only the
stock script-68 owner in one immutable PSP ANM load generation may bypass the
generic dispatcher.  Every identity, readiness, table, byte-range, or
fingerprint rejection must fall through to the canonical call.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import struct
import unittest


ROOT = Path(__file__).resolve().parents[1]
ITEM_CPP = ROOT / "src" / "ItemManager.cpp"
ITEM_HPP = ROOT / "src" / "ItemManager.hpp"
ANM_CPP = ROOT / "src" / "AnmManager.cpp"
ANM_HPP = ROOT / "src" / "AnmManager.hpp"
MEMORY_CPP = ROOT / "psp" / "memory_telemetry.cpp"
MAIN_CPP = ROOT / "psp" / "main.cpp"
MAKEFILE = ROOT / "Makefile.psp"

VM_BYTES = 0x2A4
BASE_BYTES = 0x208


def modeled_initializer(pre_call: bytes) -> bytes:
    result = bytearray(pre_call)
    result[:BASE_BYTES] = bytes(BASE_BYTES)
    result[0x214:0x218] = (68).to_bytes(4, "little", signed=True)
    result[0x288:0x28C] = (0).to_bytes(4, "little", signed=True)
    return bytes(result)


@dataclass(frozen=True)
class Identity:
    owner: int
    raw_data: int
    scripts: int
    sprites: int
    script68: int
    generation: int
    anm_idx: int


class ModeledGenerationCache:
    def __init__(self) -> None:
        self.identity: Identity | None = None
        self.validation: str = "unvalidated"
        self.revalidations = 0
        self.fingerprint_walks = 0
        self.generation_changes = 0

    def resolve(self, identity: Identity, *, in_range: bool = True,
                fingerprint: bool = True) -> bool:
        if self.identity is not None:
            same_owner_generation = (
                self.identity.owner == identity.owner
                and self.identity.generation == identity.generation
            )
            if same_owner_generation:
                if self.identity != identity:
                    return False
                return self.validation == "valid"
            if (self.identity.owner == identity.owner
                    and self.identity.generation != identity.generation):
                self.generation_changes += 1

        self.identity = identity
        self.revalidations += 1
        if not in_range:
            self.validation = "range"
            return False
        self.fingerprint_walks += 1
        if not fingerprint:
            self.validation = "fingerprint"
            return False
        self.validation = "valid"
        return True


class ItemTimeSpawnInitFastpathModelTest(unittest.TestCase):
    def test_psp_off_on_reservation_model_is_exactly_88_bytes(self) -> None:
        cache_bytes = struct.calcsize("<5I2Ii")
        stats_bytes = struct.calcsize("<14I")
        self.assertEqual(cache_bytes, 32)
        self.assertEqual(stats_bytes, 56)
        self.assertEqual(cache_bytes + stats_bytes, 88)

    def test_stale_tail_survives_product_initializer(self) -> None:
        original = bytes((index * 29 + 7) & 0xFF for index in range(VM_BYTES))
        result = modeled_initializer(original)
        self.assertEqual(result[:BASE_BYTES], bytes(BASE_BYTES))
        self.assertEqual(result[BASE_BYTES:0x214], original[BASE_BYTES:0x214])
        self.assertEqual(result[0x218:0x288], original[0x218:0x288])
        self.assertEqual(result[0x28C:], original[0x28C:])

    def test_4972_calls_walk_fingerprint_once(self) -> None:
        cache = ModeledGenerationCache()
        identity = Identity(1, 2, 3, 4, 5, 19, 7)
        self.assertTrue(all(cache.resolve(identity) for _ in range(4972)))
        self.assertEqual(cache.revalidations, 1)
        self.assertEqual(cache.fingerprint_walks, 1)

    def test_generation_change_revalidates_once(self) -> None:
        cache = ModeledGenerationCache()
        first = Identity(1, 2, 3, 4, 5, 19, 7)
        second = Identity(1, 8, 9, 10, 11, 20, 7)
        self.assertTrue(cache.resolve(first))
        self.assertTrue(cache.resolve(second))
        self.assertTrue(cache.resolve(second))
        self.assertEqual(cache.revalidations, 2)
        self.assertEqual(cache.fingerprint_walks, 2)
        self.assertEqual(cache.generation_changes, 1)

    def test_same_generation_pointer_change_fails_closed(self) -> None:
        cache = ModeledGenerationCache()
        first = Identity(1, 2, 3, 4, 5, 19, 7)
        changed = Identity(1, 2, 3, 4, 99, 19, 7)
        self.assertTrue(cache.resolve(first))
        self.assertFalse(cache.resolve(changed))
        self.assertEqual(cache.revalidations, 1)

    def test_invalid_range_is_cached_without_fingerprint_walk(self) -> None:
        cache = ModeledGenerationCache()
        identity = Identity(1, 2, 3, 4, 5, 19, 7)
        self.assertFalse(cache.resolve(identity, in_range=False))
        self.assertFalse(cache.resolve(identity, in_range=True))
        self.assertEqual(cache.revalidations, 1)
        self.assertEqual(cache.fingerprint_walks, 0)


class ItemTimeSpawnInitFastpathSourceContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.item_cpp = ITEM_CPP.read_text(encoding="utf-8")
        cls.item_hpp = ITEM_HPP.read_text(encoding="utf-8")
        cls.anm_cpp = ANM_CPP.read_text(encoding="utf-8")
        cls.anm_hpp = ANM_HPP.read_text(encoding="utf-8")
        cls.memory_cpp = MEMORY_CPP.read_text(encoding="utf-8")
        cls.main_cpp = MAIN_CPP.read_text(encoding="utf-8")
        cls.makefile = MAKEFILE.read_text(encoding="utf-8")

    def test_product_gate_is_default_off_stamped_and_mutually_exclusive(self) -> None:
        gate = "TH08_PSP_ITEM_TIME_SPAWN_INIT_FASTPATH"
        self.assertIn(f"{gate} ?= 0", self.makefile)
        self.assertIn(f"-D{gate}=1", self.makefile)
        self.assertIn("ITEM_TIME_SPAWN_INIT_FASTPATH_CONFIG_STAMP", self.makefile)
        self.assertIn("mutually exclusive", self.makefile)
        self.assertIn("spawn-init audit and fast path are mutually exclusive",
                      self.item_hpp)
        self.assertIn("TH08_PSP_FEATURE_ITEM_TIME_SPAWN_INIT_FASTPATH",
                      self.main_cpp)

    def test_compact_table_and_byte_range_guards_precede_dereference(self) -> None:
        start = self.item_cpp.index("bool ResolveItemTimeSpawnFastpathScript")
        end = self.item_cpp.index("void InitializeItemTimeScript68Fastpath", start)
        resolve = self.item_cpp[start:end]
        table_guard = resolve.index("PspItemTimeSpawnInitTablesContain")
        script_read_token = "owner->scripts[kItemTimeSpawnFastpathScriptIndex]"
        cached_read = resolve.index(script_read_token)
        revalidation_read = resolve.index(script_read_token, table_guard)
        range_guard = resolve.index("PspItemTimeSpawnInitScriptRangeContains")
        fingerprint = resolve.index("ValidateItemTimeSpawnFastpathScript68")
        # Cached table access is legal only after the generation identity and
        # prior VALID state are checked.  A new generation proves table bounds
        # before its first script-slot read.
        self.assertLess(resolve.index("ITEM_TIME_SPAWN_CACHE_VALID"), cached_read)
        self.assertLess(table_guard, revalidation_read)
        self.assertLess(range_guard, fingerprint)
        for token in (
            "u32 spriteCount;",
            "state.spriteCount = spriteCount;",
            "state.compactSize - byteCount",
            "static_cast<u32>(scriptIndex) < state.scriptCount",
            "static_cast<u32>(spriteIndex) < state.spriteCount",
        ):
            self.assertIn(token, self.anm_cpp)

    def test_generation_cache_covers_every_mutable_identity(self) -> None:
        for field in (
            "AnmLoaded *owner;",
            "AnmRawEntry *rawData;",
            "AnmRawInstr **scripts;",
            "AnmLoadedSprite *sprites;",
            "AnmRawInstr *script68;",
            "u32 generation;",
            "i32 anmIdx;",
        ):
            self.assertIn(field, self.item_cpp)
        self.assertIn("cacheOwnerGenerationMatch", self.item_cpp)
        self.assertIn("cacheGenerationChanges", self.item_cpp)
        self.assertIn("cacheRevalidations", self.item_cpp)
        self.assertIn("cacheValidationFailures", self.item_cpp)

    def test_off_on_bss_reservation_is_unconditional_typed_and_exact(self) -> None:
        storage = self.item_cpp.index("struct ItemTimeSpawnInitFastpathStorage")
        product_guard = self.item_cpp.index(
            "#if defined(PSP) && TH08_PSP_ITEM_TIME_SPAWN_INIT_PRODUCT_ENABLED",
            storage,
        )
        global_object = self.item_cpp.index(
            "g_ItemTimeSpawnInitFastpathStorage __attribute__((used));",
            storage,
        )
        self.assertLess(storage, global_object)
        self.assertLess(global_object, product_guard)
        for token in (
            "ItemTimeSpawnInitCache cache;",
            "ItemTimeSpawnInitFastpathStats stats;",
            "static_assert(sizeof(ItemTimeSpawnInitCache) == 32",
            "static_assert(alignof(ItemTimeSpawnInitCache) == 4",
            "static_assert(sizeof(ItemTimeSpawnInitFastpathStats) == 56",
            "static_assert(offsetof(ItemTimeSpawnInitFastpathStorage, stats) == 32",
            "static_assert(sizeof(ItemTimeSpawnInitFastpathStorage) == 88",
            "static_assert(alignof(ItemTimeSpawnInitFastpathStorage) == 4",
            "alignas(4) ItemTimeSpawnInitFastpathStorage",
            "g_ItemTimeSpawnInitFastpathStorage __attribute__((used))",
        ):
            self.assertIn(token, self.item_cpp)
        header_stats = self.item_hpp.index("struct ItemTimeSpawnInitFastpathStats")
        header_product_getter = self.item_hpp.index(
            "const ItemTimeSpawnInitFastpathStats &GetItemTimeSpawnInitFastpathStats"
        )
        self.assertLess(header_stats, header_product_getter)

    def test_strict_four_instruction_fingerprint(self) -> None:
        start = self.item_cpp.index("bool ValidateItemTimeSpawnFastpathScript68")
        end = self.item_cpp.index("bool ResolveItemTimeSpawnFastpathScript", start)
        validate = self.item_cpp[start:end]
        for token in (
            "AnmOpcode_Sprite",
            "instructionSize != 12",
            "AnmOpcode_ColorTime",
            "instructionSize != 28",
            "AnmOpcode_AlphaTime",
            "instructionSize != 20",
            "AnmOpcode_Static",
            "stop->instructionSize == 8",
            "stop->time == 50",
        ):
            self.assertIn(token, validate)

    def test_product_preserves_full_vm_boundary_and_counters(self) -> None:
        start = self.item_cpp.index("void InitializeItemTimeScript68Fastpath")
        end = self.item_cpp.index(
            "bool TryInitializeItemTimeSpawnScript68Fastpath", start
        )
        initializer = self.item_cpp[start:end]
        self.assertIn(
            "memset(static_cast<AnmVmBase *>(vm), 0, sizeof(AnmVmBase))",
            initializer,
        )
        self.assertNotIn("memset(vm, 0, sizeof(AnmVm))", initializer)
        for token in (
            "vm->loadedSprite = &owner->sprites",
            "vm->matrix1.m[0][0]",
            "vm->matrix2 = vm->matrix1",
            "vm->matrix3.m[1][1]",
            "vm->currentInstruction = reinterpret_cast<AnmRawInstr *>",
            "vm->currentTimeInScript++",
            "++g_AnmManager->scriptsExecutedThisFrame",
            "++g_AnmManager->scriptsStartedThisFrame",
        ):
            self.assertIn(token, initializer)
        self.assertLess(
            initializer.index("scriptsExecutedThisFrame"),
            initializer.index("scriptsStartedThisFrame"),
        )

    def test_every_rejection_falls_through_to_canonical(self) -> None:
        spawn_start = self.item_cpp.index("Item *ItemManager::SpawnItem")
        spawn_end = self.item_cpp.index(
            "DIFFABLE_STATIC_ARRAY_ASSIGN", spawn_start
        )
        spawn = self.item_cpp[spawn_start:spawn_end]
        fastpath = spawn.index("TryInitializeItemTimeSpawnScript68Fastpath")
        canonical = spawn.index(
            "g_BulletManager.bulletAnm->SetAndExecuteScriptIdx", fastpath
        )
        self.assertLess(fastpath, canonical)
        self.assertIn("if (!TryInitializeItemTimeSpawnScript68Fastpath", spawn)
        self.assertLess(spawn.index("GetRandomF32SignedInRange"), fastpath)
        self.assertLess(spawn.index("RecordItemSpawnRequest"), fastpath)
        self.assertGreater(spawn.index("RecordItemSpawnResult", fastpath), fastpath)

    def test_product_telemetry_exposes_hit_and_all_fallback_classes(self) -> None:
        for field in (
            "calls",
            "hits",
            "canonicalFallbacks",
            "ownerFallbacks",
            "generationFallbacks",
            "readinessFallbacks",
            "scriptFallbacks",
            "rangeFallbacks",
            "fingerprintFallbacks",
            "cacheHits",
            "cacheRevalidations",
            "cacheGenerationChanges",
            "cacheValidationFailures",
            "cacheResets",
        ):
            self.assertIn(f"u32 {field};", self.item_hpp)
        self.assertIn("ITEM_TIME_SPAWN_INIT_FASTPATH kind=%s", self.memory_cpp)
        self.assertIn("GetItemTimeSpawnInitFastpathStats", self.memory_cpp)

    def test_telemetry_scope_and_remaining_pixel_gate_are_explicit(self) -> None:
        self.assertIn("ITEM_TIME_SPAWN_INIT_FASTPATH_POLICY", self.memory_cpp)
        for token in (
            "counter_scope=manager_lifetime",
            "reset_scope=ItemManager::Initialize",
            "cache_scope=immutable_ready_compact_anm_load_generation",
            "replay_hash_proves_gameplay_state_only=1",
            "draw_fields_require_same_frame_pixel_gate=1",
            "pixel_gate=pending",
        ):
            self.assertIn(token, self.memory_cpp)
        self.assertIn("immutable Ready+compact ANM load-generation",
                      self.item_cpp)
        self.assertIn("same-frame pixel gate", self.item_cpp)

    def test_audit_return_is_labeled_contract_not_canonical_equality(self) -> None:
        self.assertIn("candidateReturnContractMatches", self.item_hpp)
        self.assertIn("candidateReturnContractMismatches", self.item_hpp)
        self.assertNotIn("u32 returnMatches;", self.item_hpp[
            self.item_hpp.index("struct ItemTimeSpawnInitAuditStats"):
            self.item_hpp.index("const ItemTimeSpawnInitAuditStats")
        ])
        self.assertIn("canonical ExecuteScript return, so no", self.item_cpp)
        self.assertIn("canonical return value is observable here", self.item_cpp)
        self.assertIn("candidate_return_contract_matches=", self.memory_cpp)


if __name__ == "__main__":
    unittest.main(verbosity=2)
