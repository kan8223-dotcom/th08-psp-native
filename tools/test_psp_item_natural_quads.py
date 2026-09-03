#!/usr/bin/env python3
"""Focused source and topology gates for ITEM_TIME natural 6V->4V batches."""

from __future__ import annotations

import random
import struct
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ANM = (ROOT / "src/AnmManager.cpp").read_text(encoding="utf-8")
ANM_HPP = (ROOT / "src/AnmManager.hpp").read_text(encoding="utf-8")
ITEM = (ROOT / "src/ItemManager.cpp").read_text(encoding="utf-8")
D3D = (ROOT / "src/modern/linux/d3d8_compat.cpp").read_text(
    encoding="utf-8"
)
INTERNAL = (ROOT / "src/modern/linux/d3d8_internal.hpp").read_text(
    encoding="utf-8"
)
MAKEFILE = (ROOT / "Makefile.psp").read_text(encoding="utf-8")
MAIN = (ROOT / "psp/main.cpp").read_text(encoding="utf-8")
MEMORY = (ROOT / "psp/memory_telemetry.cpp").read_text(encoding="utf-8")
CONTRACT = (ROOT / "tools/item_natural_quads_contract.py").read_text(
    encoding="utf-8"
)
FEATURE = "TH08_PSP_ITEM_NATURAL_QUADS"


def body(source: str, signature: str) -> str:
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


def make_vertex(rng: random.Random) -> bytes:
    """Canonical VertexTex1DiffuseXyzrhw, including arbitrary float bits."""

    words = [rng.getrandbits(32) for _ in range(7)]
    return struct.pack("<7I", *words)


def make_quad(rng: random.Random) -> list[bytes]:
    corners = [make_vertex(rng) for _ in range(4)]
    return [
        corners[0],
        corners[1],
        corners[2],
        corners[1],
        corners[2],
        corners[3],
    ]


def topology_valid(vertices: list[bytes]) -> bool:
    if not vertices or len(vertices) % 6:
        return False
    return all(
        vertices[offset + 1] == vertices[offset + 3]
        and vertices[offset + 2] == vertices[offset + 4]
        for offset in range(0, len(vertices), 6)
    )


def pack_final(source: bytes) -> bytes:
    """Arbitrary deterministic 28B->24B final transform for topology proof."""

    words = struct.unpack("<7I", source)
    # The topology proof is independent of the renderer's transform details:
    # any pure per-vertex map must produce the same final indexed triangles.
    return struct.pack(
        "<6I",
        words[5],
        words[6],
        words[4],
        words[0] ^ 0x3F000000,
        words[1] ^ 0x3F000000,
        words[2] ^ 0xBF800000,
    )


class ItemNaturalQuadSourceTests(unittest.TestCase):
    def test_default_off_stamp_requirements_exclusions_and_identity(self) -> None:
        self.assertIn(f"{FEATURE} ?= 0", MAKEFILE)
        self.assertIn("item-natural-quads-0.stamp", MAKEFILE)
        self.assertIn("item-natural-quads-1.stamp", MAKEFILE)
        self.assertIn(f"-D{FEATURE}=1", MAKEFILE)
        self.assertIn(
            f"{FEATURE}=1 requires TH08_PSP_BULLET_UNIFIED_QUADS=1",
            MAKEFILE,
        )
        self.assertIn(
            f"{FEATURE} is isolated from prior Item topology experiments",
            MAKEFILE,
        )
        self.assertIn("$(ITEM_NATURAL_QUADS_CONFIG_STAMP)", MAKEFILE)
        self.assertIn("TH08_PSP_FEATURE_ITEM_NATURAL_QUADS", MAIN)
        self.assertIn("ITEM_NATURAL_QUADS=%d", MAIN)

    def test_item_wrapper_marks_only_item_time_and_adds_no_pass_bracket(self) -> None:
        draw = body(ITEM, "void ItemManager::OnDraw()")
        # The independently gated ITEM_TIME inline frontend may own the first
        # preprocessor branch.  Natural quads remain the same exclusive branch
        # even when their directive is consequently spelled #elif.
        loop_start = draw.index("while (item != NULL)")
        natural_if = draw.find(
            "#if TH08_PSP_ITEM_NATURAL_QUADS_ENABLED", loop_start
        )
        natural_elif = draw.find(
            "#elif TH08_PSP_ITEM_NATURAL_QUADS_ENABLED", loop_start
        )
        candidates = [index for index in (natural_if, natural_elif) if index >= 0]
        self.assertTrue(candidates)
        loop_feature = min(candidates)
        natural = draw[
            loop_feature :
            draw.index("#elif TH08_PSP_ITEM_TIME_DRAW_PAIR_ENABLED", loop_feature)
        ]
        self.assertIn("PspItemNaturalQuadNotePass();", draw[:loop_feature])
        self.assertIn("item->itemType == ITEM_TIME", natural)
        self.assertEqual(natural.count("PspItemNaturalQuadSetCurrentTarget(true)"), 1)
        self.assertEqual(natural.count("PspItemNaturalQuadSetCurrentTarget(false)"), 1)
        self.assertNotIn("BeginPspItem", natural)
        self.assertNotIn("EndPspItem", natural)
        self.assertNotIn("FlushVertexBuffer", natural)

    def test_marker_is_after_successful_canonical_six_vertex_append(self) -> None:
        add = body(ANM, "ZunResult AnmManager::AddSpriteToDrawBuffer(")
        mark = add.index("MarkPspItemNaturalQuadCanonicalAppend();")
        canonical = add.rfind("this->vertexBufferEndPtr[0] = vertices[0];", 0, mark)
        self.assertGreater(canonical, 0)
        window = add[canonical:mark]
        for token in (
            "vertexBufferEndPtr[1] = vertices[1]",
            "vertexBufferEndPtr[2] = vertices[2]",
            "vertexBufferEndPtr[3] = vertices[1]",
            "vertexBufferEndPtr[4] = vertices[2]",
            "vertexBufferEndPtr[5] = vertices[3]",
            "vertexBufferEndPtr += 6",
            "spritesToDraw++",
        ):
            self.assertIn(token, window)
        self.assertLess(mark, add.index("return ZUN_SUCCESS", mark))

    def test_existing_flush_replaces_one_draw_without_state_or_upload_calls(self) -> None:
        flush = body(ANM, "void AnmManager::FlushVertexBuffer()")
        first_state = flush.index("SetTextureStageState")
        helper = flush.index("TrySubmitPspItemNaturalQuadsAtCanonicalBoundary")
        canonical_draw = flush.index("DrawPrimitiveUP", helper)
        self.assertLess(first_state, helper)
        self.assertLess(helper, canonical_draw)
        natural = flush[helper:canonical_draw]
        self.assertNotIn("SetTextureStageState", natural)
        self.assertNotIn("SetRenderState", natural)
        self.assertNotIn("SetTexture(", natural)
        self.assertNotIn("RenderPerfNoteUploadAttempt", natural)
        self.assertNotIn("FlushVertexBuffer(", natural)
        self.assertIn("this->spritesToDraw = 0U", natural)
        self.assertIn("++this->flushesThisFrame", natural)

    def test_frontend_preflight_is_complete_and_preserves_canonical_fallback(self) -> None:
        submit = body(
            ANM,
            "PspItemNaturalFlushResult "
            "TrySubmitPspItemNaturalQuadsAtCanonicalBoundary(",
        )
        for token in (
            "g_PspBulletUnifiedQuadBatchActive",
            "PspUnifiedQuadBatchOwner::None",
            "vertexBufferStartPtr == NULL",
            "vertexBufferEndPtr == NULL",
            "quadCount == 0U",
            "quadCount > kPspBulletUnifiedQuadCapacity",
            "triggerQuads == 0U",
            "triggerQuads > quadCount",
            "batchEnd - batchBegin != expectedSpan",
            "memcmp(&vertices[1], &vertices[3]",
            "memcmp(&vertices[2], &vertices[4]",
            "ValidatePspItemNaturalIndexAuthority(quadCount)",
            "CanonicalFallback",
        ):
            self.assertIn(token, submit)
        self.assertNotIn("PrepareState", submit)
        self.assertNotIn("DrawPrimitive", submit)
        self.assertNotIn("FlushVertexBuffer", submit)

    def test_index_authority_is_exact_private_and_incrementally_validated(self) -> None:
        validate = body(ANM, "bool ValidatePspItemNaturalIndexAuthority(")
        self.assertIn("storage.indexAuthority = g_PspBulletUnifiedQuadIndices", validate)
        self.assertIn("storage.indexAuthority != g_PspBulletUnifiedQuadIndices", validate)
        self.assertIn("ordinal = storage.validatedIndexCount", validate)
        self.assertIn("kCorners[ordinal % 6U]", validate)
        self.assertIn("storage.indexAuthority[ordinal] != expected", validate)
        self.assertIn("storage.indexAuthorityRejected = 1U", validate)
        initializer = body(ANM, "void InitializePspBulletUnifiedQuadIndices()")
        for offset in range(6):
            self.assertIn(
                f"g_PspBulletUnifiedQuadIndices[sprite * 6U + {offset}U]",
                initializer,
            )

    def test_backend_rejects_before_prepare_and_has_atomic_owned_copy_fallback(self) -> None:
        submit = body(D3D, "PspItemNaturalQuadSubmitResult DrawPspItemNaturalQuads(")
        prepare = submit.index("PrepareState(true)")
        reject_prefix = submit[:prepare]
        for reason in (
            "PSP_ITEM_NATURAL_REJECT_DEVICE",
            "PSP_ITEM_NATURAL_REJECT_STATE",
            "PSP_ITEM_NATURAL_REJECT_INDEX",
            "PSP_ITEM_NATURAL_REJECT_CAPACITY",
        ):
            self.assertIn(reason, reject_prefix)
        self.assertNotIn("PrepareState(", reject_prefix)
        self.assertNotIn("glDraw", reject_prefix)
        self.assertEqual(submit.count("PrepareState(true)"), 1)
        self.assertEqual(submit.count("glDrawElements("), 1)
        self.assertEqual(
            submit.count("__pspgl_th08_draw_native_indexed_quads_copy("), 1
        )
        self.assertIn("TH08_PSP_ITEM_NATURAL_NATIVE_COPY_ENABLED", submit)
        self.assertIn("nativeCopyRejected", submit)
        self.assertIn("PSP_ITEM_NATURAL_SUBMIT_NATIVE", submit)
        self.assertIn(
            "PSP_ITEM_NATURAL_SUBMIT_CLIENT_AFTER_NATIVE_REJECT", submit
        )
        self.assertIn("PSP_ITEM_NATURAL_SUBMIT_CLIENT", submit)
        self.assertIn("ordinary client path once", submit)

    def test_backend_exact_corner_map_and_canonical_final_vertex_operations(self) -> None:
        submit = body(D3D, "PspItemNaturalQuadSubmitResult DrawPspItemNaturalQuads(")
        self.assertIn(
            "static const UINT kUniqueCanonicalCorners[4] = {0U, 1U, 2U, 5U}",
            submit,
        )
        for token in (
            "TransformPosition(position, true",
            "output.z = 1.0f - 2.0f * output.z",
            "color = EffectiveColor(color)",
            "color >> 16",
            "color >> 8",
            "color & 255U",
            "color >> 24",
            "output.u = uv[0]",
            "output.v = uv[1]",
            "sizeof(PspClientVertex)",
            "RenderPerfNoteDraw(indexCount)",
            "RenderPerfNoteStateEmitted(9U)",
        ):
            self.assertIn(token, submit)
        self.assertIn("sizeof(PspClientVertex) == 24U", D3D)
        self.assertIn("stride != 28U", submit)

    def test_off_on_frontend_storage_is_unconditional_and_fixed(self) -> None:
        declaration = ANM.index("struct PspItemNaturalQuadStorage")
        feature_guard = ANM.rfind(FEATURE, 0, declaration)
        psp_guard = ANM.rfind("#if defined(PSP)", 0, declaration)
        self.assertGreater(psp_guard, feature_guard)
        reservation = ANM[declaration : declaration + 1700]
        self.assertIn("sizeof(PspItemNaturalQuadStats) == 136U", reservation)
        self.assertIn("sizeof(PspItemNaturalQuadStorage) == 152U", reservation)
        self.assertIn("g_PspItemNaturalQuadStorage", reservation)
        self.assertIn("__attribute__((used))", reservation)
        self.assertNotIn("#if TH08_PSP_ITEM_NATURAL_QUADS_ENABLED", reservation.split("__attribute__((used));")[0])
        self.assertNotIn("PspItemNaturalQuad", D3D[D3D.index("class LinuxDevice") : D3D.index("class LinuxDevice") + 500])

    def test_telemetry_schema_algebra_and_reset_are_wired(self) -> None:
        self.assertIn("ITEM_NATURAL_QUADS_TELEMETRY", MEMORY)
        for token in (
            "mode=product",
            "counter_scope=stage_relative_interval",
            "cumulative=0",
            "existing_flush=1",
            "begin_end_added=0",
            "topology=6v_to_4v_indexed",
            "frontend_bytes_saved=0",
            "eligibleQuads) * 48UL",
            "native_no_copy_attempted=0",
            "client_owned_same_call=1",
        ):
            self.assertIn(token, MEMORY)
        self.assertGreaterEqual(MEMORY.count("ResetItemNaturalQuadInterval()"), 4)
        for field in (
            "passes",
            "canonical_batches",
            "item_time_candidates",
            "visible_item_time",
            "culled_item_time",
            "trigger_batches",
            "trigger_quads",
            "eligible_quads",
            "client_fallback_submits",
            "fallback_batches",
            "topology_checks",
            "topology_checked_quads",
            "extra_flushes",
            "abandoned_batches",
        ):
            self.assertIn(f"{field}=", MEMORY)
            self.assertIn(f'"{field}"', CONTRACT)


class ItemNaturalQuadTopologyTests(unittest.TestCase):
    def test_indexed_four_vertex_output_expands_to_exact_canonical_triangles(self) -> None:
        rng = random.Random(0x4954454D)
        indices: list[int] = []
        for quad in range(1536):
            base = quad * 4
            indices.extend((base, base + 1, base + 2, base + 1, base + 2, base + 3))
        for quad_count in (1, 2, 17, 255, 1536):
            with self.subTest(quad_count=quad_count):
                canonical = [vertex for _ in range(quad_count) for vertex in make_quad(rng)]
                self.assertTrue(topology_valid(canonical))
                packed = [
                    pack_final(canonical[offset + corner])
                    for offset in range(0, len(canonical), 6)
                    for corner in (0, 1, 2, 5)
                ]
                expanded = [packed[index] for index in indices[: quad_count * 6]]
                expected = [pack_final(vertex) for vertex in canonical]
                self.assertEqual(expanded, expected)
                self.assertEqual(len(canonical) * 28 - len(packed) * 24, quad_count * 72)
                self.assertEqual(len(canonical) * 24 - len(packed) * 24, quad_count * 48)

    def test_any_duplicate_topology_byte_mutation_is_rejected(self) -> None:
        rng = random.Random(0x54494D45)
        for duplicate_corner in (3, 4):
            for byte_offset in (0, 7, 16, 27):
                with self.subTest(corner=duplicate_corner, byte=byte_offset):
                    vertices = make_quad(rng)
                    mutated = bytearray(vertices[duplicate_corner])
                    mutated[byte_offset] ^= 0x80
                    vertices[duplicate_corner] = bytes(mutated)
                    self.assertFalse(topology_valid(vertices))


if __name__ == "__main__":
    unittest.main()
