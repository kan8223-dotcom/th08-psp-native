#!/usr/bin/env python3
"""Host-only M0 contracts for the default-off all-Item mixed-quad path."""

from pathlib import Path
import re
import struct
import unittest


ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def body(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for offset in range(brace, len(source)):
        if source[offset] == "{":
            depth += 1
        elif source[offset] == "}":
            depth -= 1
            if depth == 0:
                return source[start : offset + 1]
    raise AssertionError(f"unterminated function: {signature}")


def vertex(x: float, y: float, u: float, v: float, color: int = 0xFFFFFFFF) -> bytes:
    return struct.pack("<ffffIff", x, y, 0.15, 1.0, color, u, v)


def quad(axis_aligned: bool) -> tuple[bytes, bytes, bytes, bytes]:
    result = (
        vertex(10.0, 20.0, 0.0, 0.0),
        vertex(18.0, 20.0, 1.0, 0.0),
        vertex(10.0, 28.0, 0.0, 1.0),
        vertex(18.0, 28.0, 1.0, 1.0),
    )
    if axis_aligned:
        return result
    return (result[0], vertex(17.0, 19.0, 1.0, 0.0), result[2], result[3])


def stage_mixed(quads: list[tuple[bytes, bytes, bytes, bytes]]) -> tuple[list[bytes], int, int]:
    """Reference prefix/sticky-suffix model used only by this host test."""
    staged: list[bytes] = []
    sticky = False
    pair_count = 0
    general_count = 0
    for current in quads:
        unpacked = [struct.unpack("<ffffIff", value) for value in current]
        eligible = (
            unpacked[0][0] == unpacked[2][0]
            and unpacked[1][0] == unpacked[3][0]
            and unpacked[0][1] == unpacked[1][1]
            and unpacked[2][1] == unpacked[3][1]
        )
        use_pair = eligible and not sticky
        if use_pair:
            staged.extend((current[0], current[3]))
            pair_count += 1
        else:
            staged.extend(current)
            general_count += 1
            sticky = True
    return staged, pair_count, general_count


def mixed_range_is_valid(
    pair_count: int,
    general_count: int,
    sprites_to_draw: int,
    buffer_begin: int,
    buffer_limit: int,
    batch_begin: int,
    batch_end: int,
) -> bool:
    """Reference model for the count-first mixed frontend proof."""
    capacity = 0x600
    vertex_bytes = struct.calcsize("<ffffIff")
    counts_bounded = (
        0 <= pair_count <= capacity
        and 0 <= general_count <= capacity
        and pair_count <= capacity - general_count
    )
    if not counts_bounded:
        return False
    total_quads = pair_count + general_count
    expected_vertices = pair_count * 2 + general_count * 4
    ordered = (
        buffer_begin <= batch_begin <= buffer_limit
        and batch_begin <= batch_end <= buffer_limit
    )
    if not ordered:
        return False
    return (
        (batch_begin - buffer_begin) % vertex_bytes == 0
        and total_quads == sprites_to_draw
        and total_quads != 0
        and batch_end - batch_begin == expected_vertices * vertex_bytes
    )


class ItemMixedQuadTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.header = read("src/AnmManager.hpp")
        cls.anm = read("src/AnmManager.cpp")
        cls.bullets = read("src/BulletManager.cpp")
        cls.backend = read("src/modern/linux/d3d8_compat.cpp")
        cls.internal = read("src/modern/linux/d3d8_internal.hpp")
        cls.makefile = read("Makefile.psp")
        cls.main = read("psp/main.cpp")
        cls.telemetry = read("psp/memory_telemetry.cpp")
        cls.append = body(cls.anm, "ZunResult AnmManager::AddSpriteToDrawBuffer(")
        cls.begin = body(cls.anm, "void AnmManager::BeginPspItemMixedQuadBatch(")
        cls.end = body(cls.anm, "void AnmManager::EndPspItemMixedQuadBatch(")
        cls.begin_bullet = body(
            cls.anm, "void AnmManager::BeginPspBulletUnifiedQuadBatch("
        )
        cls.end_bullet = body(
            cls.anm, "void AnmManager::EndPspBulletUnifiedQuadBatch("
        )
        cls.end_item_time = body(
            cls.anm, "void AnmManager::EndPspItemTimeDrawPairPass("
        )
        cls.restore_owner = body(
            cls.anm, "void RestorePspMixedQuadOwnerTokensIfQuiescent("
        )
        cls.record = body(cls.anm, "void RecordPspMixedQuad(")
        cls.flush = body(cls.anm, "bool FlushPspMixedQuadBatch(")
        cls.backend_method = body(cls.backend, "bool DrawPspBulletMixedQuads(")

    def test_default_off_and_dependencies_are_explicit(self) -> None:
        for token in (
            "TH08_PSP_ITEM_MIXED_QUADS_AUDIT ?= 0",
            "TH08_PSP_ITEM_MIXED_QUADS_FASTPATH ?= 0",
            "ITEM_MIXED_QUADS_AUDIT and TH08_PSP_ITEM_MIXED_QUADS_FASTPATH are mutually exclusive",
            "TH08_PSP_ITEM_MIXED_QUADS_AUDIT=1 requires TH08_PSP_BULLET_UNIFIED_QUADS=1",
            "TH08_PSP_ITEM_MIXED_QUADS_FASTPATH=1 requires TH08_PSP_BULLET_DIRECT_GE=1",
            "-DTH08_PSP_ITEM_MIXED_QUADS_AUDIT=1",
            "-DTH08_PSP_ITEM_MIXED_QUADS_FASTPATH=1",
        ):
            self.assertIn(token, self.makefile)
        self.assertIn("Item mixed quads and Item direct-GE are mutually exclusive", self.header)
        self.assertIn("Item mixed quads and ITEM_TIME draw-pair are mutually exclusive", self.header)

    def test_item_traversal_has_one_exact_owner_bracket(self) -> None:
        begin = self.bullets.index("g_AnmManager->BeginPspItemMixedQuadBatch();")
        draw = self.bullets.index("g_ItemManager.OnDraw();", begin)
        end = self.bullets.index("g_AnmManager->EndPspItemMixedQuadBatch();", draw)
        laser = self.bullets.index("for (i = 0;", end)
        self.assertLess(begin, draw)
        self.assertLess(draw, end)
        self.assertLess(end, laser)
        self.assertIn("#elif defined(PSP) && defined(TH08_PSP_ITEM_DIRECT_GE)", self.bullets)

    def test_audit_observes_final_quad_then_retains_canonical_six_vertices(self) -> None:
        audit = self.append.index("TH08_PSP_ITEM_MIXED_QUADS_AUDIT")
        classify = self.append.index("ClassifyPspBulletMixedQuad(vertices)", audit)
        canonical = self.append.rindex("this->vertexBufferEndPtr[5] = vertices[3]")
        self.assertLess(classify, canonical)
        self.assertIn("g_PspBulletUnifiedQuadBatchActive = false", self.begin)
        for token in (
            "vertexBufferEndPtr[0] = vertices[0]",
            "vertexBufferEndPtr[1] = vertices[1]",
            "vertexBufferEndPtr[2] = vertices[2]",
            "vertexBufferEndPtr[3] = vertices[1]",
            "vertexBufferEndPtr[4] = vertices[2]",
            "vertexBufferEndPtr[5] = vertices[3]",
        ):
            self.assertIn(token, self.append[canonical - 500 :])

    def test_product_is_prefix_then_sticky_suffix_without_reordering(self) -> None:
        q0 = quad(True)
        q1 = quad(False)
        q2 = quad(True)
        staged, pairs, general = stage_mixed([q0, q1, q2])
        self.assertEqual((pairs, general), (1, 2))
        self.assertEqual(staged[0:2], [q0[0], q0[3]])
        self.assertEqual(staged[2:6], list(q1))
        self.assertEqual(staged[6:10], list(q2))
        self.assertIn("run.stickyGeneral = 1U", self.anm)
        self.assertLess(
            self.flush.index("for (u32 pair = 0U; pair < pairCount"),
            self.flush.index("for (u32 general = 0U;"),
        )

    def test_capacity_split_preserves_sticky_state_and_pool_order(self) -> None:
        self.assertIn("constexpr u32 kPspBulletUnifiedQuadCapacity = 0x600U", self.anm)
        self.assertIn("#define MAX_ITEMS 2096", read("src/ItemManager.hpp"))
        # AddSprite also has capacity guards for the independently gated
        # packed-Bullet frontend.  Anchor this assertion inside the mixed
        # Item/Bullet branch so unrelated frontends can grow without making
        # the source-contract test inspect the wrong guard.
        mixed_branch = self.append.index(
            "const u32 requiredVertices = productOwner"
        )
        capacity_branch = self.append.index(
            "this->spritesToDraw >= kPspBulletUnifiedQuadCapacity",
            mixed_branch,
        )
        excerpt = self.append[capacity_branch : capacity_branch + 1500]
        self.assertIn("const u8 stickyGeneral = mixedRun.stickyGeneral", excerpt)
        self.assertIn("mixedRun.stickyGeneral = stickyGeneral", excerpt)
        self.assertIn("mixedRun.stateRunActive = stateRunActive", excerpt)

    def test_backend_requires_one_exclusive_owner_and_no_new_heap(self) -> None:
        for token in (
            "g_PspItemMixedGeBatchActive",
            "!g_PspBulletDirectGeBatchActive",
            "!g_PspItemDirectGeBatchActive",
            "!g_PspItemMixedGeBatchActive",
            "bool itemOwner",
        ):
            self.assertIn(token, self.backend_method)
        self.assertIn("th08_psp_item_mixed_ge_set_batch", self.internal)
        self.assertIn("th08_psp_draw_item_mixed_quads", self.internal)
        self.assertNotRegex(self.backend_method, r"\b(malloc|realloc|new)\s*\(")
        self.assertIn("pspBulletDirectGeVertices", self.backend_method)

    def test_owner_conflict_never_clobbers_and_restores_previous_pass(self) -> None:
        for token in (
            "sidecar.run.owner != PspSpritePairRunOwner::None",
            "++sidecar.itemStats.ownerConflictPasses",
            "++sidecar.rejectedItemPassDepth",
            "th08_psp_bullet_direct_ge_set_batch(false)",
        ):
            self.assertIn(token, self.begin)
        for token in (
            "--sidecar.rejectedItemPassDepth",
            "RestorePspMixedQuadOwnerTokensIfQuiescent()",
        ):
            self.assertIn(token, self.end)
        self.assertIn("th08_psp_item_mixed_ge_set_batch(false)", self.begin_bullet)
        self.assertIn("--sidecar.rejectedBulletPassDepth", self.end_bullet)
        self.assertIn(
            "RestorePspMixedQuadOwnerTokensIfQuiescent()", self.end_bullet
        )
        self.assertIn(
            "RestorePspMixedQuadOwnerTokensIfQuiescent()", self.end_item_time
        )
        for token in (
            "sidecar.rejectedItemPassDepth != 0U",
            "sidecar.rejectedBulletPassDepth != 0U",
            "sidecar.run.owner == PspSpritePairRunOwner::ItemTime",
            "sidecar.run.owner == PspSpritePairRunOwner::Bullet",
            "sidecar.run.owner == PspSpritePairRunOwner::Item",
            "th08_psp_bullet_direct_ge_set_batch(true)",
            "th08_psp_item_mixed_ge_set_batch(true)",
        ):
            self.assertIn(token, self.restore_owner)
        item_token = self.restore_owner.index(
            "th08_psp_item_mixed_ge_set_batch(false)"
        )
        self.assertGreater(
            self.restore_owner.rfind(
                "#if TH08_PSP_ITEM_MIXED_QUADS_PRODUCT_ENABLED", 0, item_token
            ),
            -1,
        )

    def test_owner_specific_savings_match_each_authoritative_topology(self) -> None:
        self.assertIn(
            "owner == PspSpritePairRunOwner::Item ? 4U : 2U", self.record
        )
        self.assertIn("stats.geVerticesSaved += 4U", self.record)
        self.assertIn(
            "if (owner == PspSpritePairRunOwner::Item)", self.record
        )
        self.assertIn("stats.frontendVerticesSaved += 2U", self.record)

        # Item authority is canonical 6V: pair 6->2 and general 6->4.
        item_pairs, item_general = 7, 5
        self.assertEqual(item_pairs * 4 + item_general * 2, 38)
        self.assertEqual(item_pairs * 4, 28)
        # Bullet authority is the accepted indexed 4V path: pair 4->2 only.
        bullet_pairs, bullet_general = 7, 5
        self.assertEqual(bullet_pairs * 2 + bullet_general * 0, 14)
        self.assertEqual(bullet_pairs * 4 + bullet_general * 0, 28)
        self.assertIn("Item general quads", self.header)

    def test_fail_close_rejects_wrapped_counts_and_non_element_offsets(self) -> None:
        count_proof = self.flush.index("const bool countsBounded")
        total = self.flush.index("const u32 totalQuads", count_proof)
        expected = self.flush.index("const uintptr_t expectedVertices", total)
        self.assertLess(count_proof, total)
        self.assertLess(total, expected)
        for token in (
            "pairCount <= kPspBulletUnifiedQuadCapacity",
            "generalCount <= kPspBulletUnifiedQuadCapacity",
            "pairCount <= kPspBulletUnifiedQuadCapacity - generalCount",
            "countsBounded ? pairCount + generalCount : 0U",
            "batchOffset % vertexBytes == 0U",
        ):
            self.assertIn(token, self.flush)

        base = 0x1000
        limit = base + 0x10000
        vertex_bytes = struct.calcsize("<ffffIff")
        # Exact u32 counterexample: UINT32_MAX+2 wraps to one in the old sum.
        self.assertFalse(
            mixed_range_is_valid(
                0xFFFFFFFF, 2, 1, base, limit, base, base + 6 * vertex_bytes
            )
        )
        self.assertFalse(
            mixed_range_is_valid(
                0x600, 1, 0x601, base, limit, base, base + 0xC04
            )
        )
        # +4 is ABI-aligned but not a 28-byte vertex-element boundary.
        self.assertFalse(
            mixed_range_is_valid(
                1, 0, 1, base, limit, base + 4, base + 4 + 2 * vertex_bytes
            )
        )
        self.assertTrue(
            mixed_range_is_valid(
                1,
                0,
                1,
                base,
                limit,
                base + vertex_bytes,
                base + 3 * vertex_bytes,
            )
        )

    def test_final_quad_and_game_state_are_not_mutated_by_staging(self) -> None:
        self.assertNotRegex(self.append, r"vertices\s*\[[^]]+\]\s*=")
        self.assertNotIn("g_QuadVertices[0] =", self.append)
        for forbidden in ("rng", "score", "item->", "currentPosition", "Replay"):
            self.assertNotIn(forbidden, self.append)

    def test_fail_closed_replay_and_telemetry_are_owner_specific(self) -> None:
        for token in (
            "owner == PspSpritePairRunOwner::Item",
            "th08_psp_draw_item_mixed_quads",
            "canonicalReplayComplete ? totalQuads : 0U",
            "disarmAndResetStaging()",
        ):
            self.assertIn(token, self.flush)
        self.assertIn("ITEM_MIXED_QUADS kind=%s", self.telemetry)
        self.assertIn("ResetPspItemMixedQuadStats", self.telemetry)
        self.assertIn("ITEM_MIXED_QUADS_AUDIT=%d", self.main)
        self.assertIn("ITEM_MIXED_QUADS_FASTPATH=%d", self.main)


if __name__ == "__main__":
    unittest.main(verbosity=2)
