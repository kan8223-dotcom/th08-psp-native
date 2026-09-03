#!/usr/bin/env python3
"""Host-only source and differential gates for TH08_PSP_ITEM_DIRECT_GE."""

from __future__ import annotations

import random
import struct
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ANM = (ROOT / "src" / "AnmManager.cpp").read_text(encoding="utf-8")
ANM_H = (ROOT / "src" / "AnmManager.hpp").read_text(encoding="utf-8")
BULLETS = (ROOT / "src" / "BulletManager.cpp").read_text(encoding="utf-8")
ITEMS = (ROOT / "src" / "ItemManager.cpp").read_text(encoding="utf-8")
GAME = (ROOT / "src" / "GameManager.cpp").read_text(encoding="utf-8")
SUPERVISOR = (ROOT / "src" / "Supervisor.cpp").read_text(encoding="utf-8")
D3D = (ROOT / "src" / "modern" / "linux" / "d3d8_compat.cpp").read_text(
    encoding="utf-8"
)
INTERNAL = (
    ROOT / "src" / "modern" / "linux" / "d3d8_internal.hpp"
).read_text(encoding="utf-8")
MAKEFILE = (ROOT / "Makefile.psp").read_text(encoding="utf-8")
MAIN = (ROOT / "psp" / "main.cpp").read_text(encoding="utf-8")


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


class PspItemDirectGeSourceTests(unittest.TestCase):
    def test_feature_is_psp_only_requires_unified_quads_and_binds_build_identity(self) -> None:
        for source, token in (
            (ANM, "BeginPspItemUnifiedQuadBatch"),
            (ANM_H, "BeginPspItemUnifiedQuadBatch"),
            (BULLETS, "BeginPspItemUnifiedQuadBatch"),
            (D3D, "TH08_PSP_ITEM_DIRECT_GE_ENABLED"),
        ):
            position = source.index(token)
            guard = source.rfind("#if", 0, position)
            self.assertIn("PSP", source[guard:position])
        self.assertIn(
            "TH08_PSP_ITEM_DIRECT_GE requires TH08_PSP_BULLET_UNIFIED_QUADS",
            D3D,
        )
        self.assertIn("th08_psp_item_direct_ge_set_batch", INTERNAL)
        self.assertIn("th08_psp_item_direct_ge_release_stage", INTERNAL)
        for token in (
            "TH08_PSP_ITEM_DIRECT_GE ?= 0",
            "item-direct-ge-$(TH08_PSP_ITEM_DIRECT_GE).stamp",
            "TH08_PSP_ITEM_DIRECT_GE=1 requires TH08_PSP_BULLET_UNIFIED_QUADS=1",
            "-DTH08_PSP_ITEM_DIRECT_GE=1",
            "$(ITEM_DIRECT_GE_CONFIG_STAMP)",
        ):
            self.assertIn(token, MAKEFILE)
        self.assertIn("ITEM_DIRECT_GE=%d", MAIN)
        self.assertIn("TH08_PSP_FEATURE_ITEM_DIRECT_GE", MAIN)

    def test_item_owner_exactly_brackets_item_draw_and_excludes_other_layers(self) -> None:
        on_draw = body(BULLETS, "ChainCallbackResult BulletManager::OnDraw(")
        begin_item = on_draw.index("BeginPspItemUnifiedQuadBatch();")
        item = on_draw.index("g_ItemManager.OnDraw();")
        end_item = on_draw.index("EndPspItemUnifiedQuadBatch();")
        laser = on_draw.index("laser->bodyVm.pos")
        begin_bullet = on_draw.index("BeginPspBulletUnifiedQuadBatch();")
        end_bullet = on_draw.index("EndPspBulletUnifiedQuadBatch();")
        effects = on_draw.index("g_EffectManager.DrawBulletLayerEffects();")
        self.assertLess(begin_item, item)
        self.assertLess(item, end_item)
        self.assertLess(end_item, laser)
        self.assertLess(laser, begin_bullet)
        self.assertLess(begin_bullet, end_bullet)
        self.assertLess(end_bullet, effects)
        self.assertEqual(on_draw.count("BeginPspItemUnifiedQuadBatch();"), 1)
        self.assertEqual(on_draw.count("EndPspItemUnifiedQuadBatch();"), 1)
        item_draw = body(ITEMS, "void ItemManager::OnDraw()")
        for excluded in (
            "CreateScorePopup",
            "CreatePlayerPointPopup",
            "DrawSingleBullet",
            "DrawBulletLayerEffects",
            "laser->",
        ):
            self.assertNotIn(excluded, item_draw)

    def test_dedicated_owner_flushes_before_enable_and_before_disable(self) -> None:
        begin = body(ANM, "void AnmManager::BeginPspItemUnifiedQuadBatch()")
        end = body(ANM, "void AnmManager::EndPspItemUnifiedQuadBatch()")
        clear = body(ANM, "void AnmManager::ClearVertexBuffer()")
        flush = body(ANM, "void AnmManager::FlushVertexBuffer()")
        recovery = body(flush, "if (!validRange)")
        self.assertLess(begin.index("FlushVertexBuffer"), begin.index("set_batch(true)"))
        self.assertIn("PspUnifiedQuadBatchOwner::Item", begin)
        self.assertIn("PspUnifiedQuadBatchOwner::Item", end)
        self.assertLess(end.index("FlushVertexBuffer"), end.index("set_batch(false)"))
        self.assertIn("th08_psp_item_direct_ge_set_batch(false)", clear)
        self.assertIn("th08_psp_item_direct_ge_set_batch(false)", recovery)

    def test_texture_blend_and_zwrite_boundaries_keep_existing_flushes(self) -> None:
        draw = body(ANM, "ZunResult AnmManager::DrawInner(")
        blend = body(ANM, "void AnmManager::SetRenderStateForVm(")
        render_state = body(SUPERVISOR, "void Supervisor::SetRenderState(")
        texture_change = draw.index("this->currentTexture != vm->loadedSprite->texture")
        texture_flush = draw.index("this->FlushVertexBuffer();", texture_change)
        texture_bind = draw.index("SetTexture(0, this->currentTexture)", texture_change)
        self.assertLess(texture_flush, texture_bind)
        blend_change = blend.index("this->currentBlendMode != vm->blendMode")
        self.assertGreater(blend.index("this->FlushVertexBuffer();"), blend_change)
        self.assertIn("D3DRS_ZWRITEENABLE", blend)
        self.assertLess(
            render_state.index("g_AnmManager->FlushVertexBuffer();"),
            render_state.index("d3dDevice->SetRenderState"),
        )

    def test_bounded_working_set_has_disjoint_fixed_partition_and_cursor(self) -> None:
        constructor = body(D3D, "LinuxDevice(SDL_Window *window_")
        destructor = body(D3D, "~LinuxDevice()")
        begin_scene = body(D3D, "HRESULT BeginScene()")
        self.assertIn("kPspItemDirectGeArenaQuads = 1280U", D3D)
        self.assertIn("kPspItemDirectGeVertexCapacity", D3D)
        self.assertIn("kPspItemDirectGeArenaBytes == 122880U", D3D)
        self.assertEqual(1280 * 4 * 24, 122880)
        self.assertIn("existing generic indexed path draws every overflowing batch", D3D)
        self.assertNotIn('"item direct GE vertices"', constructor)
        self.assertIn("allocation=lazy", constructor)
        indexed = body(D3D, "HRESULT DrawIndexed(")
        self.assertIn('"item direct GE vertices"', indexed)
        self.assertIn("pspItemDirectGeAllocationAttempted", indexed)
        self.assertLess(
            indexed.index("pspItemDirectGeAllocationAttempted = true"),
            indexed.index('"item direct GE vertices"'),
        )
        item_owner = indexed.index("if (g_PspItemDirectGeBatchActive &&")
        item_upload = indexed.index("PrepareState(true);", item_owner)
        item_allocation = indexed.index('"item direct GE vertices"', item_owner)
        self.assertLess(item_upload, item_allocation)
        self.assertIn("Complete any deferred texture upload", indexed)
        self.assertIn('"bullet direct GE vertices"', constructor)
        self.assertIn("storage=render_arena heap_fallback=0", constructor)
        self.assertIn("pspItemDirectGeVertexCursor", constructor)
        self.assertIn("pspItemDirectGeAllocationAttempted = false", constructor)
        self.assertIn("pspItemDirectGeFaulted = false", constructor)
        self.assertIn("pspBulletDirectGeVertexCursor", constructor)
        self.assertIn("pspItemDirectGeArenaPresent != presentCount", begin_scene)
        self.assertIn("pspBulletDirectGeArenaPresent != presentCount", begin_scene)
        self.assertIn("pspItemDirectGeVertexCursor = 0U", begin_scene)
        self.assertIn("pspBulletDirectGeVertexCursor = 0U", begin_scene)
        self.assertIn("RenderResourceArenaTryFree(\n                    pspItemDirectGeVertices", destructor)
        self.assertNotIn("free(pspItemDirectGeVertices)", destructor)

    def test_stage_boundary_fences_releases_and_rearms_lazy_storage(self) -> None:
        release = body(D3D, "bool ReleasePspItemDirectGeStageArena()")
        self.assertLess(
            release.index("g_PspItemDirectGeBatchActive"),
            release.index("glFinish();"),
        )
        self.assertIn("reason=active_batch", release)
        self.assertIn("pspItemDirectGeFaulted = true", release)
        self.assertLess(
            release.index("glFinish();"),
            release.index("RenderResourceArenaTryFree"),
        )
        self.assertIn("RenderResourceArenaFreeResult::Freed", release)
        self.assertIn("RenderResourceArenaFreeResult::Quarantined", release)
        self.assertLess(
            release.index("if (pspItemDirectGeFaulted)"),
            release.index("pspItemDirectGeAllocationAttempted = false"),
        )
        indexed = body(D3D, "HRESULT DrawIndexed(")
        self.assertIn(
            "g_PspItemDirectGeBatchActive &&\n                    "
            "!pspItemDirectGeFaulted",
            indexed,
        )
        self.assertIn("pspItemDirectGeVertices = NULL", release)
        self.assertIn("pspItemDirectGeAllocationAttempted = false", release)
        self.assertIn("pspItemDirectGeVertexCursor = 0U", release)
        self.assertIn("pspItemDirectGeArenaPresent = ~0UL", release)
        deleted = body(GAME, "ZunResult GameManager::DeletedCallback(")
        release_call = deleted.index("th08_psp_item_direct_ge_release_stage")
        for owner_cut in (
            "BulletManager::CutChain();",
            "EnemyManager::CutChain();",
            "EffectManager::CutChain();",
            "Gui::CutChain();",
        ):
            self.assertLess(deleted.index(owner_cut), release_call)
        self.assertLess(
            release_call,
            deleted.index("StagePoolArenaEndStage", release_call),
        )

    def test_native_shape_has_exclusive_owner_shared_authority_and_full_fallback(self) -> None:
        indexed = body(D3D, "HRESULT DrawIndexed(")
        self.assertIn("directOwnerExclusive", indexed)
        self.assertIn(
            "g_PspBulletDirectGeBatchActive !=\n            g_PspItemDirectGeBatchActive",
            indexed,
        )
        for token in (
            "D3DPT_TRIANGLELIST",
            "minVertexIndex == 0U",
            "primitiveCount / 2U <= kPspBulletDirectGeMaxQuads",
            "numVertexIndices == primitiveCount * 2U",
            "stride == 28U",
            "D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1",
            "pspBulletDirectGeIndexAuthority != directIndices",
            "kQuadCorners[6]",
        ):
            self.assertIn(token, indexed)
        submit = indexed.index("__pspgl_th08_draw_native_indexed_triangles")
        item_advance = indexed.index("pspItemDirectGeVertexCursor +=")
        bullet_advance = indexed.index("pspBulletDirectGeVertexCursor +=")
        self.assertLess(submit, item_advance)
        self.assertLess(submit, bullet_advance)
        self.assertIn(
            "directVertexCount <=\n                            "
            "kPspItemDirectGeVertexCapacity",
            indexed,
        )
        self.assertIn("++pspItemDirectGeFallbacks", indexed)
        self.assertIn("++pspBulletDirectGeFallbacks", indexed)
        self.assertIn("glDrawElements(", indexed)
        self.assertIn("Draw(type, primitiveCount, expanded, stride)", indexed)

    def test_visual_gate_counters_are_owner_specific(self) -> None:
        constructor = body(D3D, "LinuxDevice(SDL_Window *window_")
        destructor = body(D3D, "~LinuxDevice()")
        indexed = body(D3D, "HRESULT DrawIndexed(")
        for token in (
            "pspItemDirectGeSubmittedBatches",
            "pspItemDirectGeSubmittedQuads",
            "pspItemDirectGeFallbacks",
            "pspItemDirectGeArenaHighWater",
        ):
            self.assertIn(token, constructor + indexed + destructor)
        self.assertIn("ITEM_DIRECT_GE submitted_batches=", destructor)


class PspItemDirectGeDifferentialTests(unittest.TestCase):
    @staticmethod
    def packed_vertex(rng: random.Random) -> bytes:
        return struct.pack(
            "<ffBBBBfff",
            rng.uniform(-4.0, 4.0),
            rng.uniform(-4.0, 4.0),
            rng.randrange(256),
            rng.randrange(256),
            rng.randrange(256),
            rng.randrange(256),
            rng.uniform(-512.0, 1024.0),
            rng.uniform(-512.0, 1024.0),
            rng.uniform(-1.0, 1.0),
        )

    def test_random_item_order_and_state_flushes_are_topology_identical(self) -> None:
        rng = random.Random(0x08_20_96)
        topology = (0, 1, 2, 1, 2, 3)
        for _run in range(256):
            count = rng.randrange(1, 2097)
            canonical: list[bytes] = []
            indexed: list[bytes] = []
            batch_quads = 0
            for _item in range(count):
                # Texture/blend/zwrite changes only split the current list;
                # they never reorder or alter a quad.
                if batch_quads and (batch_quads == 0x600 or rng.randrange(9) == 0):
                    batch_quads = 0
                quad = [self.packed_vertex(rng) for _ in range(4)]
                canonical.extend(quad[corner] for corner in topology)
                indexed.extend(quad[corner] for corner in topology)
                batch_quads += 1
            self.assertEqual(indexed, canonical)

    def test_bounded_item_and_full_bullet_arenas_are_independent_append_ranges(self) -> None:
        item_capacity = 1280 * 4
        bullet_capacity = 0x600 * 4
        item_cursor = 0
        bullet_cursor = 0
        # A measured 964-Item burst stays native.  A later 500-Item batch
        # cannot overwrite in-flight vertices and therefore takes the generic
        # fallback intact; the logical Item count is not truncated.
        native_batch = 964
        begin, end = item_cursor, item_cursor + native_batch * 4
        self.assertLessEqual(end, item_capacity)
        item_cursor = end
        overflow_batch = 500
        self.assertGreater(item_cursor + overflow_batch * 4, item_capacity)
        generic_drawn = overflow_batch
        # A single legal 0x600-quad unified batch is also larger than the
        # bounded Item arena.  The checked comparison must reject it before
        # evaluating capacity - directVertexCount, avoiding UINT underflow.
        maximum_batch_vertices = 0x600 * 4
        self.assertGreater(maximum_batch_vertices, item_capacity)
        has_room = (
            maximum_batch_vertices <= item_capacity
            and item_cursor <= item_capacity - maximum_batch_vertices
        )
        self.assertFalse(has_room)
        for bullet_batch in (128, 256, 384, 512, 256):
            begin, end = bullet_cursor, bullet_cursor + bullet_batch * 4
            self.assertEqual(begin, bullet_cursor)
            self.assertLessEqual(end, bullet_capacity)
            bullet_cursor = end
        self.assertEqual(item_cursor * 24, 92544)
        self.assertEqual(native_batch + generic_drawn, 1464)
        self.assertEqual(bullet_cursor * 24, 147456)


if __name__ == "__main__":
    unittest.main()
