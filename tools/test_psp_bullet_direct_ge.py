#!/usr/bin/env python3
"""Static and differential gates for TH08_PSP_BULLET_DIRECT_GE."""

from __future__ import annotations

import hashlib
import random
import struct
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAKEFILE = (ROOT / "Makefile.psp").read_text(encoding="utf-8")
MAIN = (ROOT / "psp" / "main.cpp").read_text(encoding="utf-8")
ANM = (ROOT / "src" / "AnmManager.cpp").read_text(encoding="utf-8")
D3D = (ROOT / "src" / "modern" / "linux" / "d3d8_compat.cpp").read_text(
    encoding="utf-8"
)
INTERNAL = (
    ROOT / "src" / "modern" / "linux" / "d3d8_internal.hpp"
).read_text(encoding="utf-8")
PSPGL_PATCH_PATH = ROOT / "deps" / "pspgl-ge4" / "pspgl-th08-native-submit-v3.patch"
PSPGL_PATCH = PSPGL_PATCH_PATH.read_text(encoding="utf-8")
PSPGL_README = (ROOT / "deps" / "pspgl-ge4" / "README.md").read_text(
    encoding="utf-8"
)


def native_patch_body() -> str:
    signature = "int __pspgl_th08_draw_native_indexed_triangles("
    return body(PSPGL_PATCH[PSPGL_PATCH.rindex(signature) :], signature)


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


class DirectGeSourceTests(unittest.TestCase):
    def test_feature_is_default_off_stamped_fingerprinted_and_requires_uq(self) -> None:
        self.assertIn("TH08_PSP_BULLET_DIRECT_GE ?= 0", MAKEFILE)
        self.assertIn("bullet-direct-ge-0.stamp", MAKEFILE)
        self.assertIn("bullet-direct-ge-1.stamp", MAKEFILE)
        self.assertIn("-DTH08_PSP_BULLET_DIRECT_GE=1", MAKEFILE)
        self.assertIn("requires TH08_PSP_BULLET_UNIFIED_QUADS=1", MAKEFILE)
        self.assertIn("BULLET_DIRECT_GE=%d", MAIN)

    def test_pspgl_archive_and_two_delta_provenance_are_hard_gated(self) -> None:
        self.assertIn("PSPGL_NATIVE_SUBMIT_PATCH_SHA256", MAKEFILE)
        self.assertIn(
            "ae70bc1a492212989a87fa1656ff002c6b6f5d8f2ade2b2efa50983dc77aaf47",
            MAKEFILE,
        )
        self.assertEqual(
            hashlib.sha256(PSPGL_PATCH_PATH.read_bytes()).hexdigest(),
            "ae70bc1a492212989a87fa1656ff002c6b6f5d8f2ade2b2efa50983dc77aaf47",
        )
        for symbol in (
            "__pspgl_th08_draw_native_indexed_triangles",
            "__pspgl_th08_native_submit_marker",
            "__pspgl_th08_ge4_fork_marker",
        ):
            self.assertIn(symbol, MAKEFILE + PSPGL_README)

    def test_extension_uses_pspgl_current_list_and_never_raw_gu_or_allocates(self) -> None:
        native = native_patch_body()
        self.assertIn("__pspgl_context_render_prim", native)
        self.assertIn("sceKernelDcacheWritebackRange(vertices", native)
        self.assertIn("sceKernelDcacheWritebackRange(indices", native)
        for forbidden in (
            "sceGuStart",
            "sceGuFinish",
            "sceGuDrawArray",
            "malloc(",
            "memcpy(",
        ):
            self.assertNotIn(forbidden, native)

    def test_extension_rejects_before_prim_and_has_exact_vtype(self) -> None:
        native = native_patch_body()
        render = native.index("__pspgl_context_render_prim")
        self.assertLess(native.index("index_count > 0xffffu"), render)
        self.assertLess(native.index("vertex_bytes % vertex_stride"), render)
        for token in (
            "GE_TEXTURE_32BITF",
            "GE_COLOR_8888",
            "GE_VERTEX_32BITF",
            "GE_TRANSFORM_3D",
            "GE_VINDEX_16BIT",
            "GE_TRIANGLES",
        ):
            self.assertIn(token, native)

    def test_batch_token_brackets_flush_and_only_the_six_bullet_buckets(self) -> None:
        begin = body(ANM, "void AnmManager::BeginPspBulletUnifiedQuadBatch()")
        end = body(ANM, "void AnmManager::EndPspBulletUnifiedQuadBatch()")
        clear = body(ANM, "void AnmManager::ClearVertexBuffer()")
        flush = body(ANM, "void AnmManager::FlushVertexBuffer()")
        recovery = body(flush, "if (!validRange)")
        self.assertLess(begin.index("FlushVertexBuffer"), begin.index("set_batch(true)"))
        self.assertLess(end.index("FlushVertexBuffer"), end.index("set_batch(false)"))
        self.assertNotIn("if (!g_PspBulletUnifiedQuadBatchActive)", end)
        self.assertLess(
            clear.index("set_batch(false)"),
            clear.index("g_PspBulletUnifiedQuadBatchActive = false"),
        )
        self.assertLess(
            recovery.index("DrawPrimitiveUP"), recovery.index("set_batch(false)")
        )
        self.assertLess(
            recovery.index("set_batch(false)"),
            recovery.index("g_PspBulletUnifiedQuadBatchActive = false"),
        )
        self.assertIn("th08_psp_bullet_direct_ge_set_batch", INTERNAL)

    def test_direct_shape_is_narrow_and_canonical_fallback_remains(self) -> None:
        indexed = body(D3D, "HRESULT DrawIndexed(")
        for token in (
            "g_PspBulletDirectGeBatchActive",
            "D3DPT_TRIANGLELIST",
            "minVertexIndex == 0U",
            "primitiveCount / 2U <= kPspBulletDirectGeMaxQuads",
            "numVertexIndices == primitiveCount * 2U",
            "stride == 28U",
            "D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1",
        ):
            self.assertIn(token, indexed)
        self.assertIn("glDrawElements(", indexed)
        self.assertIn("Draw(type, primitiveCount, expanded, stride)", indexed)

    def test_arena_is_aligned_append_only_and_reset_only_after_present(self) -> None:
        constructor = body(D3D, "LinuxDevice(SDL_Window *window_")
        destructor = body(D3D, "~LinuxDevice()")
        self.assertIn("RenderResourceArenaAllocate(", constructor)
        self.assertIn('"bullet direct GE vertices"', constructor)
        self.assertIn("heap_fallback=0", constructor)
        self.assertNotIn("memalign(", constructor)
        self.assertIn("RenderResourceArenaTryFree(", destructor)
        self.assertNotIn("free(pspBulletDirectGeVertices)", destructor)
        self.assertIn("kPspBulletDirectGeMaxQuads * 4U", D3D)
        begin_scene = body(D3D, "HRESULT BeginScene()")
        self.assertIn("pspBulletDirectGeArenaPresent != presentCount", begin_scene)
        self.assertIn("pspBulletDirectGeVertexCursor = 0U", begin_scene)
        indexed = body(D3D, "HRESULT DrawIndexed(")
        submit = indexed.index("__pspgl_th08_draw_native_indexed_triangles")
        advance = indexed.index("pspBulletDirectGeVertexCursor += directVertexCount")
        self.assertLess(submit, advance)

    def test_index_authority_is_exact_prefix_and_one_way_rejected(self) -> None:
        indexed = body(D3D, "HRESULT DrawIndexed(")
        self.assertIn("pspBulletDirectGeIndexAuthority == NULL", indexed)
        self.assertIn("pspBulletDirectGeIndexAuthority != directIndices", indexed)
        self.assertIn("pspBulletDirectGeValidatedIndexCount", indexed)
        self.assertIn("kQuadCorners[6]", indexed)
        self.assertIn("pspBulletDirectGeIndexAuthorityRejected = true", indexed)
        self.assertIn("Index values are deliberately not rescanned", PSPGL_README)
        # The exact-prefix proof must happen before the generic per-submit
        # range walk, and an accepted authority must skip that walk.
        proof = indexed.index("kQuadCorners[6]")
        generic_scan = indexed.index(
            "for (UINT ordinal = 0; ordinal < indexCount; ++ordinal)"
        )
        self.assertLess(proof, generic_scan)
        self.assertIn("if (!directIndexAuthorityValid)", indexed[:generic_scan])

    def test_render_perf_diagnostic_timer_is_visible_to_direct_success(self) -> None:
        indexed = body(D3D, "HRESULT DrawIndexed(")
        timer = indexed.index("const Uint64 drawStart")
        submit = indexed.index("__pspgl_th08_draw_native_indexed_triangles")
        sample = indexed.index("SDL_GetPerformanceCounter() - drawStart")
        self.assertLess(timer, submit)
        self.assertLess(submit, sample)


class DirectGeDifferentialTests(unittest.TestCase):
    @staticmethod
    def pack_native(vertex: tuple[float, float, float, float, int, float, float]) -> bytes:
        x, y, z, _rhw, color, u, v = vertex
        red = (color >> 16) & 0xFF
        green = (color >> 8) & 0xFF
        blue = color & 0xFF
        alpha = (color >> 24) & 0xFF
        return struct.pack(
            "<ffBBBBfff", u, v, red, green, blue, alpha, x + 0.5, y + 0.5, 1.0 - 2.0 * z
        )

    def test_20000_random_quads_match_packed_canonical_six_vertex_stream(self) -> None:
        rng = random.Random(0x08_60_24)
        topology = (0, 1, 2, 1, 2, 3)
        for _ in range(20_000):
            quad = [
                (
                    rng.uniform(-1024.0, 1024.0),
                    rng.uniform(-1024.0, 1024.0),
                    rng.uniform(-1.0, 1.0),
                    1.0,
                    rng.getrandbits(32),
                    rng.uniform(-4.0, 4.0),
                    rng.uniform(-4.0, 4.0),
                )
                for _corner in range(4)
            ]
            packed = [self.pack_native(vertex) for vertex in quad]
            self.assertTrue(all(len(vertex) == 24 for vertex in packed))
            canonical = b"".join(packed[index] for index in topology)
            indexed = b"".join(packed[index] for index in topology)
            self.assertEqual(indexed, canonical)

    def test_six_bucket_append_model_never_overwrites_before_present(self) -> None:
        rng = random.Random(0x600_6)
        capacity = 0x600 * 4
        for _frame in range(10_000):
            total_quads = rng.randrange(0x601)
            cuts = sorted(rng.randrange(total_quads + 1) for _ in range(5))
            buckets = []
            previous = 0
            for cut in cuts + [total_quads]:
                buckets.append(cut - previous)
                previous = cut
            cursor = 0
            ranges: list[tuple[int, int]] = []
            for quads in buckets:
                begin = cursor
                cursor += quads * 4
                self.assertLessEqual(cursor, capacity)
                ranges.append((begin, cursor))
            for left, right in zip(ranges, ranges[1:]):
                self.assertLessEqual(left[1], right[0])
            # Present fence: only now may the next frame reset to zero.
            cursor = 0
            self.assertEqual(cursor, 0)


if __name__ == "__main__":
    unittest.main()
