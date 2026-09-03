#!/usr/bin/env python3
"""Focused host contract/differential tests for PSP popup direct pairs."""

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


def add(a: float, b: float) -> float:
    return f32(f32(a) + f32(b))


def sub(a: float, b: float) -> float:
    return f32(f32(a) - f32(b))


def mul(a: float, b: float) -> float:
    return f32(f32(a) * f32(b))


def half_product(a: float, b: float) -> float:
    return f32(mul(a, b) / f32(2.0))


def nearby_pixel(raw: float, shake: float) -> float:
    # TH08 modern/PSP runs round-to-nearest/even, then subtracts binary32 0.5.
    return sub(float(round(add(raw, shake))), 0.5)


def canonical_quad(
    *, x: float, y: float, z: float, width: float, height: float,
    scale_x: float, scale_y: float, anchor: int, shake_x: float,
    shake_y: float, uv_start: tuple[float, float],
    uv_end: tuple[float, float], uv_scroll: tuple[float, float],
) -> tuple[tuple[float, ...], ...]:
    half_width = half_product(width, scale_x)
    half_height = half_product(height, scale_y)
    if anchor & 1:
        left = f32(x)
        right = add(add(half_width, x), half_width)
    else:
        left = sub(x, half_width)
        right = add(half_width, x)
    if anchor & 2:
        top = f32(y)
        bottom = add(add(half_height, y), half_height)
    else:
        top = sub(y, half_height)
        bottom = add(half_height, y)
    left = nearby_pixel(left, shake_x)
    right = nearby_pixel(right, shake_x)
    top = nearby_pixel(top, shake_y)
    bottom = nearby_pixel(bottom, shake_y)
    u0 = add(uv_start[0], uv_scroll[0])
    v0 = add(uv_start[1], uv_scroll[1])
    u1 = add(uv_end[0], uv_scroll[0])
    v1 = add(uv_end[1], uv_scroll[1])
    return (
        (left, top, f32(z), u0, v0),
        (right, top, f32(z), u1, v0),
        (left, bottom, f32(z), u0, v1),
        (right, bottom, f32(z), u1, v1),
    )


def direct_pair(
    *, x: float, y: float, z: float, width: float, height: float,
    scale_x: float, scale_y: float, anchor: int, shake_x: float,
    shake_y: float, uv_start: tuple[float, float],
    uv_end: tuple[float, float], uv_scroll: tuple[float, float],
) -> tuple[tuple[float, ...], tuple[float, ...]]:
    # Model the product's two-corner builder independently.  Do not select
    # corners from canonical_quad(): doing that would turn the randomized
    # differential test below into a self-identity check.
    direct_half_width = f32(f32(f32(width) * f32(scale_x)) / f32(2.0))
    direct_half_height = f32(f32(f32(height) * f32(scale_y)) / f32(2.0))
    if anchor & 1:
        direct_left = f32(x)
        direct_right = f32(f32(direct_half_width + f32(x)) +
                           direct_half_width)
    else:
        direct_left = f32(f32(x) - direct_half_width)
        direct_right = f32(direct_half_width + f32(x))
    if anchor & 2:
        direct_top = f32(y)
        direct_bottom = f32(f32(direct_half_height + f32(y)) +
                            direct_half_height)
    else:
        direct_top = f32(f32(y) - direct_half_height)
        direct_bottom = f32(direct_half_height + f32(y))

    direct_left = f32(float(round(f32(direct_left + f32(shake_x)))) - 0.5)
    direct_right = f32(float(round(f32(direct_right + f32(shake_x)))) - 0.5)
    direct_top = f32(float(round(f32(direct_top + f32(shake_y)))) - 0.5)
    direct_bottom = f32(float(round(f32(direct_bottom + f32(shake_y)))) - 0.5)
    direct_u0 = f32(f32(uv_start[0]) + f32(uv_scroll[0]))
    direct_v0 = f32(f32(uv_start[1]) + f32(uv_scroll[1]))
    direct_u1 = f32(f32(uv_end[0]) + f32(uv_scroll[0]))
    direct_v1 = f32(f32(uv_end[1]) + f32(uv_scroll[1]))
    return (
        (direct_left, direct_top, f32(z), direct_u0, direct_v0),
        (direct_right, direct_bottom, f32(z), direct_u1, direct_v1),
    )


def canonical_visible(quad, viewport=(32, 16, 384, 448)) -> bool:
    left, top, width, height = viewport
    xs = tuple(vertex[0] for vertex in quad)
    ys = tuple(vertex[1] for vertex in quad)
    return not (
        max(xs) < f32(left)
        or max(ys) < f32(top)
        or min(xs) > f32(left + width)
        or min(ys) > f32(top + height)
    )


def direct_visible(pair, viewport=(32, 16, 384, 448)) -> bool:
    left, top, width, height = viewport
    return not (
        pair[1][0] < f32(left)
        or pair[1][1] < f32(top)
        or pair[0][0] > f32(left + width)
        or pair[0][1] > f32(top + height)
    )


def commit_direct_final_quad(last_pair, initial_diffuse, visible_colors):
    left_top, right_bottom = last_pair
    quad = (
        left_top,
        (right_bottom[0], left_top[1], right_bottom[2],
         right_bottom[3], left_top[4]),
        (left_top[0], right_bottom[1], left_top[2],
         left_top[3], right_bottom[4]),
        right_bottom,
    )
    diffuse = list(initial_diffuse)
    if visible_colors:
        diffuse = [visible_colors[-1]] * 4
    return quad, tuple(diffuse)


class PspAsciiPopupDirectPairTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.makefile = read("Makefile.psp")
        cls.anm = read("src/AnmManager.cpp")
        cls.main = read("psp/main.cpp")
        cls.telemetry_h = read("psp/render_perf_telemetry.hpp")
        cls.telemetry_cpp = read("psp/render_perf_telemetry.cpp")
        cls.memory = read("psp/memory_telemetry.cpp")
        cls.draw = function_body(
            cls.anm, "ZunResult AnmManager::DrawPspAsciiPopupBatch")
        cls.build_pair = function_body(
            cls.anm, "void BuildPspAsciiPopupDirectPair")
        cls.commit = function_body(
            cls.anm, "void CommitPspAsciiPopupDirectFinalQuad")

    def test_independent_gate_is_default_off_and_requires_batch(self) -> None:
        self.assertIn("TH08_PSP_ASCII_POPUP_DIRECT_PAIR ?= 0", self.makefile)
        self.assertIn("-DTH08_PSP_ASCII_POPUP_DIRECT_PAIR=1", self.makefile)
        self.assertIn(
            "TH08_PSP_ASCII_POPUP_DIRECT_PAIR=1 requires "
            "TH08_PSP_ASCII_POPUP_BATCH=1", self.makefile)
        self.assertIn(
            "ascii-popup-direct-pair-$(TH08_PSP_ASCII_POPUP_DIRECT_PAIR).stamp",
            self.makefile)
        self.assertIn("ASCII_POPUP_DIRECT_PAIR=%d", self.main)
        self.assertIn("TH08_PSP_FEATURE_ASCII_POPUP_DIRECT_PAIR", self.main)
        self.assertIn("test-psp-ascii-popup-direct-pair", self.makefile)

    def test_metadata_proof_and_reserve_precede_all_authoritative_writes(self) -> None:
        direct_start = self.draw.index("// Metadata-only pass")
        legacy_start = self.draw.index("// Pass 1 validates", direct_start)
        direct = self.draw[direct_start:legacy_start]
        self.assertNotIn("BuildPspAsciiPopupQuad", direct)
        self.assertNotIn("PspAsciiPopupQuadVisible", direct)
        self.assertIn("directSprites[", direct)
        self.assertIn("kPspAsciiPopupDirectSpriteCount", direct)
        reserve = direct.index("th08_psp_reserve_ascii_popup_sprite_pairs")
        vm_write = direct.index("vm->pos.x =", reserve)
        flush = direct.index("this->FlushVertexBuffer()", reserve)
        self.assertLess(reserve, vm_write)
        self.assertLess(vm_write, flush)
        self.assertIn("CommitPspAsciiPopupDirectFinalQuad", direct)
        self.assertIn("popups != g_AsciiManager.scorePopups", self.draw)
        self.assertIn("asciiAnm != g_AsciiManager.asciiAnm", self.draw)

    def test_direct_builder_writes_only_two_corners_in_canonical_order(self) -> None:
        self.assertNotIn("pair[2]", self.build_pair)
        self.assertNotIn("pair[3]", self.build_pair)
        self.assertIn("sprite.halfWidth + vm->pos.x + sprite.halfWidth",
                      self.build_pair)
        self.assertIn("nearbyintf(rawLeft + screenShakeX)", self.build_pair)
        self.assertIn("nearbyintf(rawRight + screenShakeX)", self.build_pair)
        self.assertIn("pair[0].w = w0", self.build_pair)
        self.assertIn("pair[1].w = w3", self.build_pair)

    def test_random_pair_and_cull_are_bit_exact_to_four_corner_authority(self) -> None:
        rng = random.Random(0x54483038)
        for _ in range(5000):
            values = dict(
                x=f32(rng.uniform(-256.0, 768.0)),
                y=f32(rng.uniform(-256.0, 768.0)),
                z=f32(rng.uniform(-4.0, 4.0)),
                width=f32(rng.uniform(0.125, 64.0)),
                height=f32(rng.uniform(0.125, 64.0)),
                scale_x=f32(rng.uniform(0.0625, 4.0)),
                scale_y=f32(rng.uniform(0.0625, 4.0)),
                anchor=rng.randrange(4),
                shake_x=f32(rng.uniform(-4.0, 4.0)),
                shake_y=f32(rng.uniform(-4.0, 4.0)),
                uv_start=(f32(rng.uniform(-1.0, 1.0)),
                          f32(rng.uniform(-1.0, 1.0))),
                uv_end=(f32(rng.uniform(1.0, 2.0)),
                        f32(rng.uniform(1.0, 2.0))),
                uv_scroll=(f32(rng.uniform(-0.25, 0.25)),
                           f32(rng.uniform(-0.25, 0.25))),
            )
            quad = canonical_quad(**values)
            pair = direct_pair(**values)
            self.assertEqual(pair, (quad[0], quad[3]))
            self.assertEqual(direct_visible(pair), canonical_visible(quad))

    def test_fractional_ties_negative_shake_anchors_and_viewport_edges(self) -> None:
        base = dict(
            x=-2.5, y=-3.5, z=0.75, width=7.0, height=15.0,
            scale_x=0.5, scale_y=1.5, shake_x=-0.5, shake_y=0.5,
            uv_start=(0.125, 0.25), uv_end=(0.5, 0.75),
            uv_scroll=(-0.0625, 0.03125),
        )
        for anchor in range(4):
            quad = canonical_quad(anchor=anchor, **base)
            self.assertEqual(direct_pair(anchor=anchor, **base),
                             (quad[0], quad[3]))
        edge_pairs = (
            (((31.0, 20.0, 0.0), (32.0, 30.0, 0.0)), True),
            (((30.0, 20.0, 0.0), (31.5, 30.0, 0.0)), False),
            (((416.0, 20.0, 0.0), (417.0, 30.0, 0.0)), True),
            (((416.5, 20.0, 0.0), (417.5, 30.0, 0.0)), False),
        )
        for positions, expected in edge_pairs:
            pair = tuple((*position, 0.0, 0.0) for position in positions)
            self.assertEqual(direct_visible(pair), expected)

    def test_final_shared_quad_keeps_last_geometry_and_last_visible_color(self) -> None:
        values = dict(
            x=100.0, y=80.0, z=0.5, width=8.0, height=15.0,
            scale_x=1.0, scale_y=1.0, anchor=0,
            shake_x=0.25, shake_y=-0.25,
            uv_start=(0.1, 0.2), uv_end=(0.3, 0.4), uv_scroll=(0.0, 0.0),
        )
        quad = canonical_quad(**values)
        initial = (0x01020304, 0x11121314, 0x21222324, 0x31323334)
        rebuilt, diffuse = commit_direct_final_quad(
            (quad[0], quad[3]), initial, [0xAABBCCDD, 0x10203040])
        self.assertEqual(rebuilt, quad)
        self.assertEqual(diffuse, (0x10203040,) * 4)
        rebuilt, diffuse = commit_direct_final_quad(
            (quad[0], quad[3]), initial, [])
        self.assertEqual(rebuilt, quad)
        self.assertEqual(diffuse, initial)
        self.assertIn("geometry/UV from its final digit", self.commit)
        self.assertIn("if (hasVisibleColor)", self.commit)

    def test_savings_telemetry_is_complete_and_has_exact_formula(self) -> None:
        names = (
            "asciiPopupDirectActivePopups",
            "asciiPopupDirectValidationQuadsAvoided",
            "asciiPopupDirectValidationCullTestsAvoided",
            "asciiPopupDirectNearbyintCallsAvoided",
        )
        for name in names:
            self.assertIn(name, self.telemetry_h)
            self.assertIn(name, self.telemetry_cpp)
        self.assertIn("digits * 8U", self.telemetry_h)
        self.assertIn("digits * 2U + activePopups * 2U", self.telemetry_h)
        self.assertIn("RenderPerfNoteAsciiPopupDirectPairSavings", self.draw)
        self.assertIn(
            "render_ascii_popup_direct_validation_quads_avoided_total",
            self.memory)
        self.assertIn(
            "render_ascii_popup_direct_validation_culls_avoided_total",
            self.memory)
        self.assertIn(
            "render_ascii_popup_direct_nearbyint_avoided_total",
            self.memory)


if __name__ == "__main__":
    unittest.main(verbosity=2)
