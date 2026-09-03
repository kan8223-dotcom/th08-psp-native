#!/usr/bin/env python3
"""Host-only contracts for the default-off PSP ITEM_TIME draw-pair path."""

from __future__ import annotations

import math
import pathlib
import struct
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    raise AssertionError(f"unterminated function: {signature}")


def f32(value: float) -> float:
    return struct.unpack("<f", struct.pack("<f", value))[0]


def rounded_edge(value: float, shake: float) -> float:
    shaken = f32(f32(value) + f32(shake))
    return f32(float(round(shaken)) - 0.5)


def canonical_edges(
    x: float,
    y: float,
    width: float,
    height: float,
    scale_x: float,
    scale_y: float,
    anchor: int,
    shake_x: float,
    shake_y: float,
) -> tuple[float, float, float, float]:
    half_w = f32(f32(f32(width) * f32(scale_x)) / f32(2.0))
    half_h = f32(f32(f32(height) * f32(scale_y)) / f32(2.0))
    if anchor & 1:
        left = f32(x)
        right = f32(f32(half_w + f32(x)) + half_w)
    else:
        left = f32(f32(x) - half_w)
        right = f32(half_w + f32(x))
    if anchor & 2:
        top = f32(y)
        bottom = f32(f32(half_h + f32(y)) + half_h)
    else:
        top = f32(f32(y) - half_h)
        bottom = f32(half_h + f32(y))
    return (
        rounded_edge(left, shake_x),
        rounded_edge(right, shake_x),
        rounded_edge(top, shake_y),
        rounded_edge(bottom, shake_y),
    )


def d3d_vertex(seed: int) -> bytes:
    # x,y,z,w,color,u,v: the exact 28-byte frontend stride.
    return struct.pack(
        "<ffffIff",
        seed + 0.125,
        seed + 0.25,
        seed + 0.375,
        1.0,
        0x80010203 + seed,
        seed + 0.5,
        seed + 0.75,
    )


def pack_vertex(source: bytes) -> bytes:
    x, y, z, _w, color, u, v = struct.unpack("<ffffIff", source)
    rgba = bytes(
        ((color >> 16) & 255, (color >> 8) & 255, color & 255,
         (color >> 24) & 255)
    )
    # The test uses identity TransformPosition; layout/overlap is the contract.
    return struct.pack("<ff", u, v) + rgba + struct.pack("<fff", x, y, z)


class PspItemTimeDrawPairTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.makefile = read("Makefile.psp")
        cls.header = read("src/AnmManager.hpp")
        cls.anm = read("src/AnmManager.cpp")
        cls.item = read("src/ItemManager.cpp")
        cls.backend_h = read("src/modern/linux/d3d8_internal.hpp")
        cls.backend = read("src/modern/linux/d3d8_compat.cpp")
        cls.memory = read("psp/memory_telemetry.cpp")
        cls.main = read("psp/main.cpp")
        cls.audit = function_body(
            cls.anm, "ZunResult AnmManager::DrawPspItemTimePairAudit"
        )
        cls.product = function_body(
            cls.anm, "bool AnmManager::TryDrawPspItemTimeSpritePair"
        )
        cls.flush = function_body(cls.anm, "void FlushPspSpritePairRun")
        cls.axis = function_body(
            cls.anm, "bool PspItemTimeDrawPairQuadAxisAligned"
        )
        cls.backend_in_place = function_body(
            cls.backend, "bool DrawPspSpritePairsInPlace"
        )

    def test_three_modes_are_default_off_stamped_and_mutually_exclusive(self) -> None:
        self.assertIn("TH08_PSP_ITEM_TIME_DRAW_PAIR_AUDIT ?= 0", self.makefile)
        self.assertIn("TH08_PSP_ITEM_TIME_DRAW_PAIR_FASTPATH ?= 0", self.makefile)
        self.assertIn("item-time-draw-pair-audit-$(TH08_PSP_ITEM_TIME_DRAW_PAIR_AUDIT).stamp", self.makefile)
        self.assertIn("item-time-draw-pair-fastpath-$(TH08_PSP_ITEM_TIME_DRAW_PAIR_FASTPATH).stamp", self.makefile)
        self.assertIn("TH08_PSP_ITEM_TIME_DRAW_PAIR_AUDIT and TH08_PSP_ITEM_TIME_DRAW_PAIR_FASTPATH are mutually exclusive", self.makefile)
        self.assertIn("draw-pair audit and fast path are mutually exclusive", self.header)
        self.assertIn("draw-pair and Item direct-GE owners are mutually exclusive", self.header)
        self.assertIn("ITEM_TIME_DRAW_PAIR_AUDIT=%d", self.main)
        self.assertIn("ITEM_TIME_DRAW_PAIR_FASTPATH=%d", self.main)
        self.assertIn("test-psp-item-time-draw-pair", self.makefile)

    def test_off_audit_product_share_fixed_bss_and_manager_layout(self) -> None:
        reservation = "g_PspItemTimeDrawPairSidecarReservation"
        self.assertEqual(self.anm.count(reservation), 2)
        self.assertIn("constexpr u32 kPspItemTimeDrawPairSidecarBytes = 512U", self.anm)
        self.assertIn("__attribute__((used))", self.anm)
        self.assertIn("#if defined(PSP)\nnamespace\n{\nenum class PspSpritePairRunOwner", self.anm)
        self.assertIn("ItemTime,\n    Item,\n    Bullet", self.anm)
        self.assertIn("C_ASSERT(sizeof(AnmManager) == 0x2a2570)", self.header)
        manager_storage = self.header[
            self.header.index("    ZunColor color;", self.header.index("struct AnmManager")) :
            self.header.index("C_ASSERT(sizeof(AnmManager)")
        ]
        self.assertNotIn("ItemTimeDrawPairStats", manager_storage)
        self.assertNotIn("PspSpritePairRunState", manager_storage)
        self.assertIn("staging=AnmManager_vertexBuffer", self.memory)
        self.assertIn("heap_arena_bytes=0", self.memory)

    def test_audit_is_canonical_authoritative_and_bit_compares_endpoints(self) -> None:
        canonical = self.audit.index("const ZunResult result = this->Draw2D(vm)")
        candidate = self.audit.index("BuildAndValidatePspItemTimeDrawPair")
        compare = self.audit.index("memcmp(&candidate[0]")
        self.assertLess(canonical, candidate)
        self.assertLess(candidate, compare)
        self.assertIn("memcmp(&candidate[3]", self.audit)
        self.assertIn("memcmp(candidate, g_QuadVertices, sizeof(candidate))", self.audit)
        self.assertIn("PspItemTimeDrawPairQuadAxisAligned(g_QuadVertices, true)", self.audit)
        self.assertIn("endpointMismatches", self.audit)

    def test_exact_geometry_guards_negative_nonfinite_and_rounded_degenerate(self) -> None:
        build = function_body(
            self.anm, "PspItemTimeDrawPairRejectReason BuildAndValidatePspItemTimeDrawPair"
        )
        self.assertIn("std::isfinite(vm->rotation.x)", build)
        self.assertIn("std::isfinite(manager->screenShakeOffset.x)", build)
        self.assertIn("vm->scale.x <= 0.0f", build)
        self.assertIn("vm->spriteSize.y <= 0.0f", build)
        self.assertIn("nearbyintf(quad[0].pos.x) - g_ZeroPointFive", self.anm)
        self.assertIn("quad[0].pos.x < quad[3].pos.x", self.axis)
        self.assertIn("quad[0].pos.y < quad[3].pos.y", self.axis)
        self.assertNotIn("quad[0].pos.x <= quad[3].pos.x", self.axis)

        for anchor in range(4):
            left, right, top, bottom = canonical_edges(
                x=100.0,
                y=80.0,
                width=0.1,
                height=0.1,
                scale_x=0.1,
                scale_y=0.1,
                anchor=anchor,
                shake_x=0.0,
                shake_y=0.0,
            )
            self.assertEqual(left, right)
            self.assertEqual(top, bottom)
            self.assertFalse(left < right and top < bottom)

        normal = canonical_edges(
            x=100.25,
            y=80.75,
            width=8.0,
            height=16.0,
            scale_x=1.0,
            scale_y=1.0,
            anchor=0,
            shake_x=0.25,
            shake_y=-0.25,
        )
        self.assertTrue(normal[0] < normal[1] and normal[2] < normal[3])
        self.assertTrue(all(math.isfinite(value) for value in normal))

    def test_linked_list_order_boundaries_and_canonical_fallbacks(self) -> None:
        on_draw = function_body(self.item, "void ItemManager::OnDraw")
        boundary = on_draw.index("PspItemTimeDrawPairBoundary")
        non_time_draw = on_draw.index("g_AnmManager->Draw2D(&item->sprite)", boundary)
        self.assertLess(boundary, non_time_draw)
        self.assertIn("item = item->next", on_draw)

        for marker in (
            "!run.passActive",
            "identityReason != PSP_ITEM_TIME_DRAW_PAIR_ACCEPT",
            "reason != PSP_ITEM_TIME_DRAW_PAIR_ACCEPT",
            "PspSpritePairRunKeyMismatch",
            "!PspSpritePairRunCanStore",
        ):
            self.assertIn(marker, self.product)
        self.assertIn("FlushPspSpritePairRun(this)", self.product)
        self.assertIn("return false", self.product)
        self.assertIn("memcpy(g_QuadVertices, quad, sizeof(quad))", self.product)
        cull = self.product.index("if (!visible)")
        self.assertLess(self.product.index("memcpy(g_QuadVertices", 0), cull)
        self.assertIn("return true", self.product[cull:])
        self.assertIn("for (u32 pair = 0U; pair < pairCount; ++pair)", self.flush)
        self.assertIn("LoadPspSpritePairRunVm(manager, pair)", self.flush)
        self.assertIn("manager->Draw2D(vm)", self.flush)
        save = self.flush.index("memcpy(preservedQuad, g_QuadVertices")
        replay = self.flush.index("manager->Draw2D(vm)")
        restore = self.flush.index("memcpy(g_QuadVertices, preservedQuad")
        self.assertLess(save, replay)
        self.assertLess(replay, restore)

    def test_run_uses_existing_vertex_buffer_and_preserves_logical_counters(self) -> None:
        self.assertIn("this->vertexBuffer", self.product)
        self.assertIn("StorePspSpritePairRunVm", self.product)
        self.assertNotIn("malloc", self.product)
        self.assertNotIn("realloc", self.product)
        self.assertIn("manager->SetRenderStateForVm(representative)", self.flush)
        self.assertIn("renderStateChangesThisFrame += pairCount - 1U", self.flush)
        self.assertIn("renderStateChangesThisFrame -= pairCount", self.flush)
        self.assertIn("manager->spritesToDraw = 0U", self.flush)
        self.assertIn("manager->flushesThisFrame", self.flush)
        self.assertNotIn("HashPspItemTimeDrawPair", self.product)
        self.assertIn("HashPspItemTimeDrawPair", self.audit)

    def test_backend_preflights_then_uses_status_returning_pspgl_copy(self) -> None:
        body = self.backend_in_place
        preflight_end = body.index("const UINT vertexCount")
        for guard in ("data == NULL", "spriteCount == 0U", "stride !=", "fvf !=", "texture == NULL"):
            self.assertIn(guard, body[:preflight_end])
        prepare = body.index("PrepareState(true)")
        first_output = body.index("memcpy(data +")
        self.assertLess(preflight_end, prepare)
        self.assertLess(prepare, first_output)
        self.assertNotIn("malloc", body)
        self.assertNotIn("realloc", body)
        self.assertNotIn("sceGu", body)
        self.assertIn("EffectiveColor", body)
        self.assertIn("TransformPosition", body)
        self.assertNotIn("glDrawArrays(", body)
        native = body.index("__pspgl_th08_draw_native_sprite_pairs_copy")
        self.assertGreater(native, first_output)
        self.assertIn("== 0", body[native:])
        self.assertIn("return false", body[native:])
        self.assertIn("mutable input", self.backend_h)
        self.assertIn("No game-side", self.backend_h)
        self.assertIn("false before PRIM", self.backend_h)
        self.assertIn("TH08_PSP_ITEM_TIME_DRAW_PAIR_FASTPATH", self.makefile)
        self.assertIn("__pspgl_th08_draw_native_sprite_pairs_copy", self.makefile)

    def test_forward_28_to_24_in_place_pack_does_not_corrupt_future_input(self) -> None:
        originals = [d3d_vertex(seed) for seed in range(32)]
        storage = bytearray(b"".join(originals))
        expected = b"".join(pack_vertex(vertex) for vertex in originals)
        for index in range(len(originals)):
            source = bytes(storage[index * 28 : index * 28 + 28])
            self.assertEqual(source, originals[index])
            packed = pack_vertex(source)
            storage[index * 24 : index * 24 + 24] = packed
        self.assertEqual(bytes(storage[: len(expected)]), expected)

    def test_stock_identity_and_telemetry_are_fail_closed(self) -> None:
        identity = function_body(
            self.anm,
            "AnmManager::PspValidateItemTimeDrawPairIdentity",
        )
        fingerprint = function_body(
            self.anm, "bool ValidatePspItemTimeDrawPairScript68"
        )
        self.assertIn("kPspItemTimeDrawPairScriptIndex = 68", self.anm)
        self.assertIn("kPspItemTimeDrawPairInitialSpriteIndex = 179", self.anm)
        self.assertIn("kPspItemTimeDrawPairIndicatorSpriteIndex = 189", self.anm)
        self.assertIn("PspLoadReadyForItemTimeSpawnInit", identity)
        self.assertIn("PspItemTimeSpawnInitTablesContain", identity)
        self.assertIn("PspItemTimeSpawnInitScriptRangeContains", identity)
        self.assertIn("AnmOpcode_Sprite", fingerprint)
        self.assertIn("AnmOpcode_ColorTime", fingerprint)
        self.assertIn("AnmOpcode_AlphaTime", fingerprint)
        self.assertIn("AnmOpcode_Static", fingerprint)
        self.assertIn("ITEM_TIME_DRAW_PAIR kind=%s", self.memory)
        self.assertIn("endpoint_mismatches=%lu", self.memory)
        self.assertIn("semantic_hash=%08lx", self.memory)


if __name__ == "__main__":
    unittest.main(verbosity=2)
