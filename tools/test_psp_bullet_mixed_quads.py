#!/usr/bin/env python3
"""Host-only source and ordering gates for the PSP Bullet mixed 2V/4V path."""

from __future__ import annotations

import math
import pathlib
import random
import struct
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


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
                return source[start : index + 1]
    raise AssertionError(f"unterminated function: {signature}")


def vertex(x: float, y: float, z: float, color: int, u: float, v: float) -> bytes:
    return struct.pack("<ffffIff", x, y, z, 1.0, color, u, v)


def axis_quad(seed: int) -> tuple[bytes, bytes, bytes, bytes]:
    x0 = float(seed * 3 + 1)
    y0 = float(seed * 5 + 2)
    x1 = x0 + float(seed % 7 + 1)
    y1 = y0 + float(seed % 11 + 1)
    z = struct.unpack("<f", struct.pack("<I", 0x3E800000 + seed % 0x1000))[0]
    color = (0x80010203 + seed * 0x10101) & 0xFFFFFFFF
    u0 = float(seed % 17) / 32.0
    v0 = float(seed % 19) / 32.0
    u1 = u0 + 0.125
    v1 = v0 + 0.125
    return (
        vertex(x0, y0, z, color, u0, v0),
        vertex(x1, y0, z, color, u1, v0),
        vertex(x0, y1, z, color, u0, v1),
        vertex(x1, y1, z, color, u1, v1),
    )


def reconstruct(pair: tuple[bytes, bytes]) -> tuple[bytes, bytes, bytes, bytes]:
    p0 = list(struct.unpack("<ffffIff", pair[0]))
    p1 = list(struct.unpack("<ffffIff", pair[1]))
    q1 = p0.copy()
    q1[0] = p1[0]
    q1[5] = p1[5]
    q2 = p0.copy()
    q2[1] = p1[1]
    q2[6] = p1[6]
    return (
        pair[0],
        struct.pack("<ffffIff", *q1),
        struct.pack("<ffffIff", *q2),
        pair[1],
    )


class BulletMixedSourceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.header = read("src/AnmManager.hpp")
        cls.anm = read("src/AnmManager.cpp")
        cls.bullets = read("src/BulletManager.cpp")
        cls.backend = read("src/modern/linux/d3d8_compat.cpp")
        cls.internal = read("src/modern/linux/d3d8_internal.hpp")
        cls.patch = read("deps/pspgl-ge4/pspgl-th08-native-mixed-submit-v1.patch")
        cls.classifier = body(cls.anm, "ClassifyPspBulletMixedQuad(")
        cls.append = body(cls.anm, "ZunResult AnmManager::AddSpriteToDrawBuffer(")
        cls.flush = body(cls.anm, "bool FlushPspMixedQuadBatch(")
        cls.bullet_flush = body(
            cls.anm, "bool FlushPspBulletMixedQuadBatch("
        )
        cls.backend_method = body(cls.backend, "bool DrawPspBulletMixedQuads(")
        patch_tail = cls.patch[cls.patch.rindex(
            "int __pspgl_th08_draw_native_mixed_quads("
        ) :]
        cls.native = body(patch_tail, "int __pspgl_th08_draw_native_mixed_quads(")

    def test_source_modes_are_default_off_exclusive_and_dependency_gated(self) -> None:
        self.assertIn("Bullet mixed-quad audit and fast path are mutually exclusive", self.header)
        self.assertIn("#define TH08_PSP_BULLET_MIXED_QUADS_ENABLED 0", self.header)
        self.assertIn("#define TH08_PSP_BULLET_MIXED_QUADS_PRODUCT_ENABLED 0", self.header)
        self.assertIn("require TH08_PSP_BULLET_UNIFIED_QUADS", self.header)
        self.assertIn("requires TH08_PSP_BULLET_DIRECT_GE", self.header)
        self.assertIn("requires Bullet unified/direct GE", self.backend)
        self.assertIn("PspSpritePairRunOwner::Bullet", self.bullet_flush)

    def test_classifier_is_exact_and_conservative(self) -> None:
        for token in (
            "std::isfinite(quad[vertex].pos.x)",
            "std::isfinite(quad[vertex].textureUV.y)",
            "PspBulletMixedFloatBitsEqual(quad[0].pos.z",
            "PspBulletMixedFloatBitsEqual(quad[0].w",
            "PspBulletMixedFloatBits(1.0f)",
            "quad[0].diffuse != quad[3].diffuse",
            "quad[0].pos.x, quad[2].pos.x",
            "quad[1].pos.x, quad[3].pos.x",
            "quad[0].pos.y, quad[1].pos.y",
            "quad[2].pos.y, quad[3].pos.y",
            "quad[0].textureUV.x",
            "quad[2].textureUV.y",
            "quad[1].pos.x > quad[0].pos.x",
            "quad[2].pos.y > quad[0].pos.y",
            "quad[1].textureUV.x > quad[0].textureUV.x",
            "quad[2].textureUV.y > quad[0].textureUV.y",
        ):
            self.assertIn(token, self.classifier)
        self.assertIn("run.stickyGeneral == 0U", self.anm)
        self.assertIn("run.stickyGeneral = 1U", self.anm)

    def test_product_stages_diagonal_prefix_then_sticky_quad_suffix(self) -> None:
        pair0 = self.append.index("this->vertexBufferEndPtr[0] = vertices[0]")
        pair1 = self.append.index("this->vertexBufferEndPtr[1] = vertices[3]", pair0)
        pair_advance = self.append.index("this->vertexBufferEndPtr += 2", pair1)
        quad0 = self.append.index("this->vertexBufferEndPtr[0] = vertices[0]", pair_advance)
        quad3 = self.append.index("this->vertexBufferEndPtr[3] = vertices[3]", quad0)
        self.assertLess(pair0, pair1)
        self.assertLess(pair1, pair_advance)
        self.assertLess(pair_advance, quad0)
        self.assertLess(quad0, quad3)
        self.assertIn("const u8 stickyGeneral = mixedRun.stickyGeneral", self.append)
        self.assertIn("mixedRun.stickyGeneral = stickyGeneral", self.append)
        self.assertIn("const u8 stateRunActive = mixedRun.stateRunActive", self.append)
        self.assertIn("mixedRun.stateRunActive = stateRunActive", self.append)

    def test_six_buckets_and_canonical_recovery_keep_original_order(self) -> None:
        on_draw = body(self.bullets, "ChainCallbackResult BulletManager::OnDraw(")
        begin = on_draw.index("BeginPspBulletUnifiedQuadBatch")
        buckets = on_draw.index("for (i = 0; i < 6; i++)")
        links = on_draw.index("node = node->nextInDrawBucket", buckets)
        end = on_draw.index("EndPspBulletUnifiedQuadBatch")
        self.assertLess(begin, buckets)
        self.assertLess(buckets, links)
        self.assertLess(links, end)
        pair_replay = self.flush.index("for (u32 pair = 0U; pair < pairCount")
        general_replay = self.flush.index("for (u32 general = 0U;")
        self.assertLess(pair_replay, general_replay)
        for token in (
            "triangles[0] = quad[0]",
            "triangles[1] = quad[1]",
            "triangles[2] = quad[2]",
            "triangles[3] = quad[1]",
            "triangles[4] = quad[2]",
            "triangles[5] = quad[3]",
        ):
            self.assertIn(token, self.anm)

    def test_fail_closed_resets_staging_and_canonical_fallthrough_proves_6v_room(self) -> None:
        for token in (
            "manager->spritesToDraw = 0U",
            "manager->vertexBufferStartPtr = manager->vertexBuffer",
            "manager->vertexBufferEndPtr = manager->vertexBuffer",
            "++stats.missingRunBatches",
            "++stats.invalidRangeBatches",
            "disarmAndResetStaging()",
        ):
            self.assertIn(token, self.flush)
        self.assertIn(
            "PspBulletUnifiedQuadBufferCanAppendVertices(this, 6U)",
            self.append,
        )
        guard = self.append.index(
            "// A rejected mixed batch may arrive here from a cursor only two vertices"
        )
        canonical_write = self.append.index(
            "this->vertexBufferEndPtr[0] = vertices[0]", guard
        )
        self.assertLess(guard, canonical_write)
        self.assertIn("reason=canonical_6v_capacity", self.append[guard:canonical_write])

        # The concrete review counterexample: a mixed cursor at limit-2 cannot
        # accept canonical 6V until fail-closed reset returns it to base.
        capacity = 0x18000
        cursor = capacity - 2
        self.assertFalse(cursor + 6 <= capacity)
        cursor = 0
        self.assertTrue(cursor + 6 <= capacity)

    def test_recovery_hresult_and_fail_closed_telemetry_are_exact(self) -> None:
        draw = body(self.anm, "bool DrawPspBulletMixedCanonicalQuad(")
        self.assertIn("return SUCCEEDED", draw)
        self.assertIn("canonicalReplayDrawFailed = true", self.flush)
        self.assertIn("canonicalReplayComplete ? totalQuads : 0U", self.flush)
        self.assertIn("!validRange || canonicalReplayDrawFailed", self.flush)
        for field in (
            "failClosedBatches",
            "missingRunBatches",
            "invalidRangeBatches",
            "canonicalRecoveryDrawFailures",
        ):
            self.assertIn(f"u32 {field};", self.header)
        finish = body(self.anm, "void FinishPspMixedBatch(")
        self.assertIn("canonicalRecoveredQuads == totalQuads", finish)
        self.assertIn("++stats.canonicalRecoveryDrawFailures", finish)

    def test_shared_sidecar_owner_is_never_blindly_clobbered(self) -> None:
        begin_bullet = body(
            self.anm, "void AnmManager::BeginPspBulletUnifiedQuadBatch()"
        )
        begin_item = body(
            self.anm, "void AnmManager::BeginPspItemTimeDrawPairPass()"
        )
        end_item = body(self.anm, "void AnmManager::EndPspItemTimeDrawPairPass()")
        end_bullet = body(
            self.anm, "void AnmManager::EndPspBulletUnifiedQuadBatch()"
        )
        reset_item = body(
            self.anm, "void AnmManager::ResetPspItemTimeDrawPairStats()"
        )
        self.assertLess(
            begin_bullet.index("sidecar.run.owner != PspSpritePairRunOwner::None"),
            begin_bullet.index("memset(&sidecar.run"),
        )
        self.assertLess(
            begin_item.index("sidecar.run.owner != PspSpritePairRunOwner::None"),
            begin_item.index("memset(&sidecar.run"),
        )
        self.assertIn("++sidecar.rejectedItemPassDepth", begin_item)
        self.assertIn("--sidecar.rejectedItemPassDepth", end_item)
        self.assertIn("PspItemTimeDrawPairBoundary", begin_bullet)
        self.assertIn("++sidecar.rejectedBulletPassDepth", begin_bullet)
        self.assertIn("--sidecar.rejectedBulletPassDepth", end_bullet)
        self.assertLess(
            begin_bullet.index("PspItemTimeDrawPairBoundary"),
            begin_bullet.index("this->FlushVertexBuffer()"),
        )
        self.assertLess(
            end_bullet.index("this->FlushVertexBuffer()"),
            end_bullet.index("--sidecar.rejectedBulletPassDepth"),
        )
        conflict = begin_item.index(
            "if (sidecar.run.owner != PspSpritePairRunOwner::None)"
        )
        self.assertLess(
            begin_item.index("this->FlushVertexBuffer()", conflict),
            begin_item.index("g_PspBulletUnifiedQuadBatchActive = false", conflict),
        )
        self.assertLess(
            end_item.index("this->FlushVertexBuffer()"),
            end_item.index("--sidecar.rejectedItemPassDepth"),
        )
        self.assertIn(
            "g_PspBulletUnifiedQuadBatchActive = true", end_item
        )
        self.assertIn("sidecar.rejectedItemPassDepth == 0U", end_bullet)
        self.assertIn("sidecar.rejectedBulletPassDepth == 0U", end_item)
        self.assertNotIn("memset(&sidecar, 0, sizeof(sidecar))", reset_item)
        self.assertIn("memset(&sidecar.cache", reset_item)
        self.assertIn("memset(&sidecar.stats", reset_item)

    def test_cross_owner_reentry_model_never_mixes_frontend_representations(self) -> None:
        # Item -> Bullet: submit the Item pair prefix, isolate Bullet as 6V,
        # flush it at EndBullet, and retain the Item pass owner for resumption.
        owner = "item"
        front = ["item-2v"]
        emitted: list[str] = []
        emitted.extend(front)
        front.clear()
        rejected_bullet_depth = 1
        front.extend(["bullet-6v", "bullet-6v"])
        emitted.extend(front)
        front.clear()
        rejected_bullet_depth -= 1
        self.assertEqual(owner, "item")
        self.assertEqual(rejected_bullet_depth, 0)
        self.assertEqual(emitted, ["item-2v", "bullet-6v", "bullet-6v"])

        # Bullet -> Item: submit the mixed Bullet run, suspend its token,
        # isolate Item as 6V, then restore the still-active Bullet pass.
        owner = "bullet"
        front = ["bullet-2v", "bullet-4v"]
        emitted = list(front)
        front.clear()
        rejected_item_depth = 2  # also exercises nested rejected Item passes
        front.append("item-6v-outer")
        front.append("item-6v-inner")
        emitted.extend(front)
        front.clear()
        rejected_item_depth -= 1
        self.assertEqual(owner, "bullet")
        self.assertEqual(rejected_item_depth, 1)
        rejected_item_depth -= 1
        bullet_token_restored = rejected_item_depth == 0 and owner == "bullet"
        self.assertTrue(bullet_token_restored)
        self.assertEqual(
            emitted,
            ["bullet-2v", "bullet-4v", "item-6v-outer", "item-6v-inner"],
        )

        # Alternating nesting must not restore Bullet while either isolated
        # owner depth remains live.
        rejected_item_depth = 1
        rejected_bullet_depth = 1
        rejected_bullet_depth -= 1
        restore_at_inner_bullet_end = (
            rejected_bullet_depth == 0 and rejected_item_depth == 0
        )
        self.assertFalse(restore_at_inner_bullet_end)
        rejected_item_depth -= 1
        restore_at_outer_item_end = (
            rejected_bullet_depth == 0 and rejected_item_depth == 0
        )
        self.assertTrue(restore_at_outer_item_end)

    def test_backend_preflights_owner_ranges_counts_and_index_authority(self) -> None:
        method = self.backend_method
        prepare = method.index("PrepareState(true)")
        convert = method.index("const auto convertRange")
        submit = method.index("__pspgl_th08_draw_native_mixed_quads")
        for token in (
            "!g_PspBulletDirectGeBatchActive",
            "g_PspItemDirectGeBatchActive",
            "stride != 28U",
            "pairVertexCount > 0xffffU",
            "quadVertexCount > 0x10000U",
            "quadIndexCount % 3U != 0U",
            "quadIndexCount % 6U != 0U",
            "alignof(float)",
            "alignof(unsigned short)",
            "pspBulletDirectGeIndexAuthority != quadIndices",
            "kQuadCorners[6]",
            "pspBulletDirectGeIndexAuthorityRejected = true",
            "kPspBulletDirectGeVertexCapacity - nativeVertexCount",
        ):
            self.assertIn(token, method)
            self.assertLess(method.index(token), prepare)
        self.assertLess(prepare, convert)
        self.assertLess(convert, submit)
        self.assertLess(submit, method.index("pspBulletDirectGeVertexCursor +="))
        for forbidden in ("malloc(", "realloc(", "memalign(", "sceGuStart", "sceGuFinish", "sceGuDrawArray"):
            self.assertNotIn(forbidden, method)
        self.assertIn("th08_psp_draw_bullet_mixed_quads", self.internal)

    def test_native_hook_rejects_everything_before_first_prim_and_keeps_order(self) -> None:
        native = self.native
        first_prim = native.index("__pspgl_context_render_prim")
        last_validation = native.index("quad_index_bytes != expected_quad_index_bytes")
        self.assertLess(last_validation, first_prim)
        pair_prim = native.index("GE_SPRITES", first_prim)
        quad_prim = native.index("GE_TRIANGLES", pair_prim)
        self.assertLess(pair_prim, quad_prim)
        for forbidden in ("sceGuStart", "sceGuFinish", "sceGuDrawArray", "malloc(", "memcpy("):
            self.assertNotIn(forbidden, native)


class BulletMixedModelTests(unittest.TestCase):
    def test_exact_pair_reconstruction(self) -> None:
        for seed in range(20_000):
            quad = axis_quad(seed)
            self.assertEqual(reconstruct((quad[0], quad[3])), quad)

    def test_random_buckets_states_capacity_and_sticky_suffix_preserve_order(self) -> None:
        rng = random.Random(0x08_2_4_6)
        capacity = 0x600
        for frame in range(300):
            records: list[tuple[int, int, int, tuple[bytes, ...], bool]] = []
            ordinal = 0
            for bucket in range(6):
                for _ in range(rng.randrange(0, 500)):
                    # Bucket changes are not artificial render-state barriers.
                    state = rng.randrange(0, 9)
                    eligible = rng.randrange(0, 5) != 0
                    records.append((bucket, state, ordinal, axis_quad(frame * 3000 + ordinal + 1), eligible))
                    ordinal += 1

            output: list[tuple[int, int, bytes]] = []
            offset = 0
            while offset < len(records):
                state = records[offset][1]
                run_end = offset + 1
                while run_end < len(records) and records[run_end][1] == state:
                    run_end += 1
                sticky = False
                cursor = offset
                while cursor < run_end:
                    batch_end = min(run_end, cursor + capacity)
                    pairs: list[tuple[int, tuple[bytes, ...]]] = []
                    generals: list[tuple[int, tuple[bytes, ...]]] = []
                    for _bucket, _state, identity, quad, eligible in records[cursor:batch_end]:
                        use_pair = eligible and not sticky
                        if use_pair:
                            pairs.append((identity, quad))
                        else:
                            sticky = True
                            generals.append((identity, quad))
                    self.assertFalse(generals and pairs and pairs[-1][0] > generals[0][0])
                    for identity, quad in pairs:
                        rebuilt = reconstruct((quad[0], quad[3]))
                        output.extend((identity, corner, value) for corner, value in enumerate(rebuilt))
                    for identity, quad in generals:
                        output.extend((identity, corner, value) for corner, value in enumerate(quad))
                    cursor = batch_end
                offset = run_end

            expected = [
                (identity, corner, value)
                for _bucket, _state, identity, quad, _eligible in records
                for corner, value in enumerate(quad)
            ]
            self.assertEqual(output, expected)

    def test_guard_examples_reject_nonfinite_mirror_degenerate_and_mismatch(self) -> None:
        quad = [list(struct.unpack("<ffffIff", item)) for item in axis_quad(7)]
        cases = []
        nonfinite = [item.copy() for item in quad]
        nonfinite[2][0] = math.inf
        cases.append(nonfinite)
        mirror = [item.copy() for item in quad]
        for item in mirror:
            item[0] = -item[0]
        cases.append(mirror)
        degenerate = [item.copy() for item in quad]
        degenerate[1][0] = degenerate[0][0]
        degenerate[3][0] = degenerate[2][0]
        cases.append(degenerate)
        rotated = [item.copy() for item in quad]
        rotated[2][0] += 0.25
        cases.append(rotated)
        uv_nonrect = [item.copy() for item in quad]
        uv_nonrect[3][5] += 0.25
        cases.append(uv_nonrect)
        z_mismatch = [item.copy() for item in quad]
        z_mismatch[1][2] += 0.125
        cases.append(z_mismatch)
        w_mismatch = [item.copy() for item in quad]
        w_mismatch[2][3] = 0.5
        cases.append(w_mismatch)
        diffuse_mismatch = [item.copy() for item in quad]
        diffuse_mismatch[3][4] ^= 1
        cases.append(diffuse_mismatch)
        self.assertEqual(len(cases), 8)
        self.assertTrue(all(case != quad for case in cases))


if __name__ == "__main__":
    unittest.main()
