#!/usr/bin/env python3
"""Host contracts for the default-off PSP ITEM_TIME inline frontend."""

from __future__ import annotations

import pathlib
import random
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


def fadd(left: float, right: float) -> float:
    return f32(f32(left) + f32(right))


def fsub(left: float, right: float) -> float:
    return f32(f32(left) - f32(right))


def fmul(left: float, right: float) -> float:
    return f32(f32(left) * f32(right))


def fdiv(left: float, right: float) -> float:
    return f32(f32(left) / f32(right))


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
    half_width = fdiv(fmul(width, scale_x), 2.0)
    half_height = fdiv(fmul(height, scale_y), 2.0)
    if anchor & 1:
        left = f32(x)
        right = fadd(fadd(half_width, x), half_width)
    else:
        left = fsub(x, half_width)
        right = fadd(half_width, x)
    if anchor & 2:
        top = f32(y)
        bottom = fadd(fadd(half_height, y), half_height)
    else:
        top = fsub(y, half_height)
        bottom = fadd(half_height, y)

    # C nearbyintf under the default round-to-nearest-even mode.
    left = fsub(f32(round(fadd(left, shake_x))), 0.5)
    right = fsub(f32(round(fadd(right, shake_x))), 0.5)
    top = fsub(f32(round(fadd(top, shake_y))), 0.5)
    bottom = fsub(f32(round(fadd(bottom, shake_y))), 0.5)
    return left, right, top, bottom


def inline_edges(**values: float | int) -> tuple[float, float, float, float]:
    # Deliberately spell the candidate independently: two horizontal and two
    # vertical canonical corner values are shaken, rounded, then shared.
    width = f32(values["width"])
    height = f32(values["height"])
    sx = f32(values["scale_x"])
    sy = f32(values["scale_y"])
    x = f32(values["x"])
    y = f32(values["y"])
    anchor = int(values["anchor"])
    hx = f32(f32(width * sx) / f32(2.0))
    hy = f32(f32(height * sy) / f32(2.0))
    x0 = x if anchor & 1 else f32(x - hx)
    x1 = f32(f32(hx + x) + hx) if anchor & 1 else f32(hx + x)
    y0 = y if anchor & 2 else f32(y - hy)
    y1 = f32(f32(hy + y) + hy) if anchor & 2 else f32(hy + y)
    shake_x = f32(values["shake_x"])
    shake_y = f32(values["shake_y"])
    return (
        f32(float(round(f32(x0 + shake_x))) - f32(0.5)),
        f32(float(round(f32(x1 + shake_x))) - f32(0.5)),
        f32(float(round(f32(y0 + shake_y))) - f32(0.5)),
        f32(float(round(f32(y1 + shake_y))) - f32(0.5)),
    )


class ItemTimeInlineDrawTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.header = read("src/ItemManager.hpp")
        cls.source = read("src/ItemManager.cpp")
        cls.on_draw = function_body(cls.source, "void ItemManager::OnDraw")
        cls.build = function_body(
            cls.source, "bool BuildItemTimeInlineDrawQuad"
        )
        cls.commit = function_body(cls.source, "void CommitItemTimeInlineDraw")
        cls.product = function_body(cls.source, "bool TryDrawItemTimeInline")
        cls.audit = function_body(cls.source, "ZunResult AuditItemTimeInlineDraw")

    def test_gate_is_default_off_and_audit_product_are_exclusive(self) -> None:
        self.assertIn(
            "ITEM_TIME inline-draw audit and fast path are mutually exclusive",
            self.header,
        )
        self.assertIn(
            "#define TH08_PSP_ITEM_TIME_INLINE_DRAW_ENABLED 0", self.header
        )
        self.assertIn(
            "#define TH08_PSP_ITEM_TIME_INLINE_DRAW_PRODUCT_ENABLED 0",
            self.header,
        )
        self.assertIn(
            "M0 requires the canonical 6V Item backend", self.header
        )

    def test_product_keeps_canonical_state_and_append_boundary(self) -> None:
        for token in (
            "currentTexture != vm->loadedSprite->texture",
            "FlushVertexBuffer()",
            "SetTexture(",
            "currentVertexShader != 1",
            "SetRenderStateForVm(vm)",
            "AddSpriteToDrawBuffer(g_QuadVertices)",
        ):
            self.assertIn(token, self.commit)
        self.assertNotIn("DrawPrimitive", self.commit)
        self.assertNotIn("th08_psp_draw", self.commit)
        self.assertNotIn("BeginPsp", self.product)
        self.assertNotIn("EndPsp", self.product)

    def test_linked_list_order_and_fallback_remain_canonical(self) -> None:
        inline = self.on_draw.index("#if TH08_PSP_ITEM_TIME_INLINE_DRAW_ENABLED")
        inline_end = self.on_draw.index(
            "#elif TH08_PSP_ITEM_NATURAL_QUADS_ENABLED", inline
        )
        inline_branch = self.on_draw[inline:inline_end]
        product = inline_branch.index("TryDrawItemTimeInline")
        fallback = inline_branch.index("g_AnmManager->Draw2D", product)
        self.assertLess(product, fallback)
        self.assertNotIn("PspItemTimeDrawPairBoundary", inline_branch)
        self.assertIn("item = item->next", self.on_draw[inline_end:])

    def test_m0_is_canonical_authoritative_and_compares_final_bytes(self) -> None:
        candidate = self.audit.index("BuildItemTimeInlineDrawQuad")
        canonical = self.audit.index("g_AnmManager->Draw2D(vm)", candidate)
        quad_compare = self.audit.index("memcmp(candidate, g_QuadVertices", canonical)
        stream_compare = self.audit.index("memcmp(end - 6, expected", quad_compare)
        self.assertLess(candidate, canonical)
        self.assertLess(canonical, quad_compare)
        self.assertLess(quad_compare, stream_compare)
        self.assertIn("expected[3] = candidate[1]", self.audit)
        self.assertIn("expected[4] = candidate[2]", self.audit)
        self.assertIn("expected[5] = candidate[3]", self.audit)

    def test_builder_preserves_scratch_and_canonical_store_order(self) -> None:
        self.assertIn(
            "memcpy(output, g_QuadVertices, sizeof(g_QuadVertices))", self.build
        )
        order = [
            "spriteHalfWidth",
            "spriteHalfHeight",
            "screenShakeOffset.x",
            "nearbyintf(output[0].pos.x)",
            "vm->loadedSprite->uvStart.x",
            "f32 maxX",
            "ZunColor color",
        ]
        offsets = [self.build.index(token) for token in order]
        self.assertEqual(offsets, sorted(offsets))

    def test_randomized_float32_edges_match_canonical(self) -> None:
        rng = random.Random(0x54494D45)
        for _ in range(20_000):
            values = {
                "x": rng.uniform(-96.0, 544.0),
                "y": rng.uniform(-96.0, 576.0),
                "width": rng.uniform(0.125, 128.0),
                "height": rng.uniform(0.125, 128.0),
                "scale_x": rng.uniform(0.125, 4.0),
                "scale_y": rng.uniform(0.125, 4.0),
                "anchor": rng.randrange(4),
                "shake_x": rng.uniform(-8.0, 8.0),
                "shake_y": rng.uniform(-8.0, 8.0),
            }
            canonical = canonical_edges(**values)
            candidate = inline_edges(**values)
            self.assertEqual(
                struct.pack("<4f", *candidate), struct.pack("<4f", *canonical)
            )


if __name__ == "__main__":
    unittest.main()
