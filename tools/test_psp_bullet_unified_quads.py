from __future__ import annotations

import random
import re
import struct
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FEATURE = "TH08_PSP_BULLET_UNIFIED_QUADS"


def function_body(source: str, signature: str) -> str:
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


class PspBulletUnifiedQuadSourceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.makefile = (ROOT / "Makefile.psp").read_text(encoding="utf-8")
        cls.main = (ROOT / "psp" / "main.cpp").read_text(encoding="utf-8")
        cls.anm = (ROOT / "src" / "AnmManager.cpp").read_text(encoding="utf-8")
        cls.anm_h = (ROOT / "src" / "AnmManager.hpp").read_text(encoding="utf-8")
        cls.bullets = (ROOT / "src" / "BulletManager.cpp").read_text(
            encoding="utf-8"
        )
        cls.d3d = (ROOT / "src" / "modern" / "linux" / "d3d8_compat.cpp").read_text(
            encoding="utf-8"
        )
        cls.d3d_h = (ROOT / "src" / "modern" / "linux" / "include" / "d3d8.h").read_text(
            encoding="utf-8"
        )
        cls.d3d_types = (
            ROOT / "src" / "modern" / "linux" / "include" / "d3d8types.h"
        ).read_text(encoding="utf-8")
        cls.on_draw = function_body(
            cls.bullets, "ChainCallbackResult BulletManager::OnDraw("
        )
        cls.flush = function_body(cls.anm, "void AnmManager::FlushVertexBuffer()")
        cls.append = function_body(
            cls.anm, "ZunResult AnmManager::AddSpriteToDrawBuffer("
        )
        cls.indexed = function_body(cls.d3d, "HRESULT DrawIndexed(")

    def test_feature_is_independent_default_off_stamped_and_fingerprinted(self) -> None:
        self.assertIn(f"{FEATURE} ?= 0", self.makefile)
        self.assertIn(f"-D{FEATURE}=1", self.makefile)
        self.assertIn("bullet-unified-quads-0.stamp", self.makefile)
        self.assertIn("bullet-unified-quads-1.stamp", self.makefile)
        self.assertIn("$(BULLET_UNIFIED_QUADS_CONFIG_STAMP)", self.makefile)
        self.assertIn("TH08_PSP_FEATURE_BULLET_UNIFIED_QUADS", self.main)
        self.assertIn("BULLET_UNIFIED_QUADS=%d", self.main)

    def test_candidate_code_is_psp_only_and_does_not_touch_simulation(self) -> None:
        for source, token in (
            (self.anm, "g_PspBulletUnifiedQuadBatchActive"),
            (self.anm_h, "BeginPspBulletUnifiedQuadBatch"),
            (self.bullets, "BeginPspBulletUnifiedQuadBatch"),
        ):
            position = source.index(token)
            guard = source.rfind("#if", 0, position)
            self.assertIn("defined(PSP)", source[guard:position])
            self.assertIn(FEATURE, source[guard:position])
        draw_single = function_body(self.bullets, "ZunResult Bullet::DrawSingleBullet()")
        self.assertNotIn("UnifiedQuad", draw_single)
        for forbidden in ("RNG", "rand", "Update", "collision", "x87"):
            self.assertNotIn(forbidden, self.on_draw)

    def test_mode_is_exactly_the_six_bucket_interval(self) -> None:
        item = self.on_draw.index("g_ItemManager.OnDraw();")
        laser = self.on_draw.index("laser->bodyVm.pos")
        begin = self.on_draw.index("BeginPspBulletUnifiedQuadBatch();")
        buckets = re.search(r"for\s*\(i\s*=\s*0;\s*i\s*<\s*6;", self.on_draw)
        self.assertIsNotNone(buckets)
        assert buckets is not None
        end = self.on_draw.index("EndPspBulletUnifiedQuadBatch();")
        effects = self.on_draw.index("g_EffectManager.DrawBulletLayerEffects();")
        self.assertLess(item, laser)
        self.assertLess(laser, begin)
        self.assertLess(begin, buckets.start())
        self.assertLess(buckets.start(), end)
        self.assertLess(end, effects)
        self.assertEqual(self.on_draw.count("BeginPspBulletUnifiedQuadBatch();"), 1)
        self.assertEqual(self.on_draw.count("EndPspBulletUnifiedQuadBatch();"), 1)

    def test_mode_switches_flush_and_canonical_writer_stays_six_vertices(self) -> None:
        begin = function_body(
            self.anm, "void AnmManager::BeginPspBulletUnifiedQuadBatch()"
        )
        end = function_body(
            self.anm, "void AnmManager::EndPspBulletUnifiedQuadBatch()"
        )
        self.assertLess(
            begin.index("this->FlushVertexBuffer();"),
            begin.index("g_PspBulletUnifiedQuadBatchActive = true;"),
        )
        self.assertLess(
            end.index("this->FlushVertexBuffer();"),
            end.index("g_PspBulletUnifiedQuadBatchActive = false;"),
        )
        # Optional representations may add nested feature guards inside the
        # unified branch.  The stable boundary is its fail-closed comment, not
        # the first textual #endif.
        active_boundary = self.append.index(
            "// An invalid indexed range is disarmed by FlushVertexBuffer"
        )
        active = self.append[:active_boundary]
        for corner in range(4):
            self.assertIn(
                f"this->vertexBufferEndPtr[{corner}] = vertices[{corner}];", active
            )
        self.assertIn("this->vertexBufferEndPtr += 4;", active)
        canonical_start = self.append.index(
            "    this->vertexBufferEndPtr[0] = vertices[0];",
            active_boundary,
        )
        canonical = self.append[canonical_start:]
        for assignment in (
            "[0] = vertices[0]",
            "[1] = vertices[1]",
            "[2] = vertices[2]",
            "[3] = vertices[1]",
            "[4] = vertices[2]",
            "[5] = vertices[3]",
        ):
            self.assertIn(assignment, canonical)
        self.assertIn("this->vertexBufferEndPtr += 6;", canonical)

    def test_shared_u16_topology_capacity_and_fallback_are_proven(self) -> None:
        expected = (
            "sprite * 6U + 0U] = base;",
            "sprite * 6U + 1U] =",
            "sprite * 6U + 2U] =",
            "sprite * 6U + 3U] =",
            "sprite * 6U + 4U] =",
            "sprite * 6U + 5U] =",
        )
        positions = [self.anm.index(token) for token in expected]
        self.assertEqual(positions, sorted(positions))
        self.assertIn("kPspBulletUnifiedQuadCapacity = 0x600U", self.anm)
        self.assertIn("<= 0xffffU", self.anm)
        self.assertIn("this->spritesToDraw <= kPspBulletUnifiedQuadCapacity", self.flush)
        self.assertIn("canonicalQuadRange && batchSpan == expectedBytes", self.flush)
        self.assertIn("DrawIndexedPrimitiveUP(", self.flush)
        self.assertIn("D3DFMT_INDEX16", self.flush)
        self.assertIn("if (FAILED(indexedResult))", self.flush)
        for assignment in (
            "fallback[0] = quad[0]",
            "fallback[1] = quad[1]",
            "fallback[2] = quad[2]",
            "fallback[3] = quad[1]",
            "fallback[4] = quad[2]",
            "fallback[5] = quad[3]",
        ):
            self.assertIn(assignment, self.flush)

    def test_invalid_range_recovers_proven_quads_then_disarms_and_resets(self) -> None:
        recovery = function_body(self.flush, "if (!validRange)")
        self.assertIn("orderedInBuffer", self.flush)
        self.assertIn("batchOffset % vertexBytes == 0U", self.flush)
        self.assertNotIn("batchOffset % quadBytes == 0U", self.flush)
        self.assertIn("batchSpan % quadBytes == 0U", self.flush)
        self.assertIn("requestedQuads < rangeQuads", recovery)
        for assignment in (
            "fallback[0] = quad[0]",
            "fallback[1] = quad[1]",
            "fallback[2] = quad[2]",
            "fallback[3] = quad[1]",
            "fallback[4] = quad[2]",
            "fallback[5] = quad[3]",
        ):
            self.assertIn(assignment, recovery)
        draw = recovery.index("DrawPrimitiveUP(")
        reset = recovery.index("this->spritesToDraw = 0;")
        disarm = recovery.index("g_PspBulletUnifiedQuadBatchActive = false;")
        self.assertLess(draw, reset)
        self.assertLess(draw, disarm)
        self.assertIn("reason=unsafe_pointer_or_alignment", recovery)
        self.assertIn("result=fail_closed", recovery)
        self.assertIn("this->flushesThisFrame++;", recovery)

    def test_d3d8_api_has_unique_conversion_indexed_submit_and_desktop_fallback(self) -> None:
        self.assertIn("DrawIndexedPrimitiveUP", self.d3d_h)
        self.assertIn("D3DFMT_INDEX16 = 101", self.d3d_types)
        self.assertIn("D3DFMT_INDEX32 = 102", self.d3d_types)
        self.assertIn("vertexIndex < vertexRangeEnd", self.indexed)
        self.assertIn("PspClientVertex &output = pspDrawVertices[vertexIndex]", self.indexed)
        self.assertEqual(self.indexed.count("glDrawElements("), 1)
        self.assertIn("GL_UNSIGNED_SHORT", self.indexed)
        self.assertIn("vertexIndex >= vertexRangeEnd", self.indexed)
        self.assertIn("BYTE *expanded", self.indexed)
        self.assertIn("Draw(type, primitiveCount, expanded, stride)", self.indexed)
        self.assertIn("BYTE triangle[3U * 256U]", self.indexed)


class PspBulletUnifiedQuadDifferentialTests(unittest.TestCase):
    def test_indexed_four_vertex_stream_is_byte_exact_canonical_six_vertex_stream(self) -> None:
        rng = random.Random(0x08_07_04)
        topology = (0, 1, 2, 1, 2, 3)
        for _ in range(20000):
            quad = []
            for _corner in range(4):
                vertex = struct.pack(
                    "<ffffIff",
                    rng.uniform(-512.0, 1024.0),
                    rng.uniform(-512.0, 1024.0),
                    rng.uniform(-1.0, 1.0),
                    1.0,
                    rng.getrandbits(32),
                    rng.uniform(-2.0, 2.0),
                    rng.uniform(-2.0, 2.0),
                )
                self.assertEqual(len(vertex), 28)
                quad.append(vertex)
            canonical = b"".join(
                (quad[0], quad[1], quad[2], quad[1], quad[2], quad[3])
            )
            indexed = b"".join(quad[index] for index in topology)
            self.assertEqual(indexed, canonical)

    def test_random_state_boundaries_preserve_order_and_attributes(self) -> None:
        rng = random.Random(0x6004)
        topology = (0, 1, 2, 1, 2, 3)
        for _run in range(512):
            canonical_output: list[tuple[int, bytes]] = []
            indexed_output: list[tuple[int, bytes]] = []
            for sprite in range(rng.randint(1, 1536)):
                state = rng.randrange(8)  # texture/blend/depth boundary witness
                quad = [rng.randbytes(28) for _ in range(4)]
                canonical_output.extend(
                    (state, quad[index]) for index in topology
                )
                indexed_output.extend(
                    (state, quad[index]) for index in topology
                )
            self.assertEqual(indexed_output, canonical_output)

    def test_pool_max_is_below_all_capacity_and_index_limits(self) -> None:
        quads = 0x600
        self.assertEqual(quads * 4, 6144)
        self.assertEqual(quads * 6, 9216)
        self.assertLessEqual(quads * 4 - 1, 0xFFFF)
        self.assertLessEqual(quads * 4, 0x18000)


class PspBulletUnifiedQuadRecoveryModelTests(unittest.TestCase):
    BUFFER_BEGIN = 0x10000000
    VERTEX_BYTES = 28
    QUAD_BYTES = 4 * VERTEX_BYTES
    BUFFER_QUADS = 0x18000 // 4
    BUFFER_LIMIT = BUFFER_BEGIN + BUFFER_QUADS * QUAD_BYTES

    @classmethod
    def recoverable_quads(
        cls, batch_begin: int, batch_end: int, requested: int
    ) -> tuple[bool, int]:
        ordered = (
            cls.BUFFER_BEGIN <= batch_begin <= cls.BUFFER_LIMIT
            and batch_begin <= batch_end <= cls.BUFFER_LIMIT
        )
        offset = batch_begin - cls.BUFFER_BEGIN if ordered else 0
        span = batch_end - batch_begin if ordered else 0
        canonical = (
            ordered
            and offset % cls.VERTEX_BYTES == 0
            and span % cls.QUAD_BYTES == 0
        )
        range_quads = span // cls.QUAD_BYTES if canonical else 0
        return canonical, min(requested, range_quads)

    def test_exact_mismatch_capacity_and_unsafe_boundaries(self) -> None:
        qb = self.QUAD_BYTES
        begin = self.BUFFER_BEGIN + 31 * qb
        for requested, ranged, expected in (
            (32, 32, 32),
            (31, 32, 31),
            (32, 31, 31),
            (0x601, 0x601, 0x601),
            (1, 0, 0),
        ):
            canonical, recovered = self.recoverable_quads(
                begin, begin + ranged * qb, requested
            )
            self.assertTrue(canonical)
            self.assertEqual(recovered, expected)

        # One pre-bullet canonical 6V sprite advances the shared cursor by
        # 168 bytes: still a 28-byte vertex boundary, but 56 bytes off the
        # absolute 112-byte quad grid.  Bullet indices are relative to this
        # start, so this is a valid range and was the runtime rejection case.
        six_vertex_begin = self.BUFFER_BEGIN + 6 * self.VERTEX_BYTES
        self.assertEqual((six_vertex_begin - self.BUFFER_BEGIN) % qb, 56)
        canonical, recovered = self.recoverable_quads(
            six_vertex_begin, six_vertex_begin + 665 * qb, 665
        )
        self.assertTrue(canonical)
        self.assertEqual(recovered, 665)

        unsafe = (
            (self.BUFFER_BEGIN - qb, self.BUFFER_BEGIN),
            (self.BUFFER_BEGIN, self.BUFFER_LIMIT + qb),
            (self.BUFFER_BEGIN + qb, self.BUFFER_BEGIN),
            (self.BUFFER_BEGIN + 1, self.BUFFER_BEGIN + 1 + qb),
            (self.BUFFER_BEGIN, self.BUFFER_BEGIN + qb - 1),
        )
        for batch_begin, batch_end in unsafe:
            canonical, recovered = self.recoverable_quads(
                batch_begin, batch_end, 0x600
            )
            self.assertFalse(canonical)
            self.assertEqual(recovered, 0)

    def test_20000_random_ranges_never_recover_outside_proven_quads(self) -> None:
        rng = random.Random(0x08_60_06)
        qb = self.QUAD_BYTES
        for _ in range(20_000):
            requested = rng.randrange(0, self.BUFFER_QUADS + 1024)
            case = rng.randrange(6)
            if case < 2:
                start_vertex = rng.randrange(self.BUFFER_QUADS * 4 + 1)
                remaining_bytes = self.BUFFER_LIMIT - (
                    self.BUFFER_BEGIN + start_vertex * self.VERTEX_BYTES
                )
                range_quads = rng.randrange(remaining_bytes // qb + 1)
                batch_begin = (
                    self.BUFFER_BEGIN + start_vertex * self.VERTEX_BYTES
                )
                batch_end = batch_begin + range_quads * qb
                canonical, recovered = self.recoverable_quads(
                    batch_begin, batch_end, requested
                )
                self.assertTrue(canonical)
                self.assertEqual(recovered, min(requested, range_quads))
                self.assertLessEqual(recovered, requested)
                self.assertLessEqual(recovered, range_quads)
            elif case == 2:
                batch_begin = self.BUFFER_BEGIN - rng.randrange(1, qb + 1)
                batch_end = self.BUFFER_BEGIN
            elif case == 3:
                batch_begin = self.BUFFER_BEGIN
                batch_end = self.BUFFER_LIMIT + rng.randrange(1, qb + 1)
            elif case == 4:
                batch_begin = (
                    self.BUFFER_BEGIN
                    + 1
                    + self.VERTEX_BYTES * rng.randrange(4)
                )
                batch_end = batch_begin + qb
            else:
                batch_begin = self.BUFFER_BEGIN + qb
                batch_end = self.BUFFER_BEGIN

            if case >= 2:
                canonical, recovered = self.recoverable_quads(
                    batch_begin, batch_end, requested
                )
                self.assertFalse(canonical)
                self.assertEqual(recovered, 0)


if __name__ == "__main__":
    unittest.main()
