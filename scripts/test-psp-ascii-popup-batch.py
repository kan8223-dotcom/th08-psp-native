#!/usr/bin/env python3
"""Host-only contract tests for the default-off TH08 PSP score popup batch."""

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
    # nearbyintf under TH08's default round-to-nearest/even mode, then -0.5f.
    shaken = f32(f32(value) + f32(shake))
    return f32(float(round(shaken)) - 0.5)


def canonical_quad(
    x: float,
    y: float,
    z: float,
    width: float,
    retained_height: float,
    scale_x: float,
    scale_y: float,
    anchor: int,
    shake_x: float,
    shake_y: float,
) -> tuple[tuple[float, float, float], ...]:
    half_w = f32(f32(f32(width) * f32(scale_x)) / f32(2.0))
    half_h = f32(f32(f32(retained_height) * f32(scale_y)) / f32(2.0))
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
    left = rounded_edge(left, shake_x)
    right = rounded_edge(right, shake_x)
    top = rounded_edge(top, shake_y)
    bottom = rounded_edge(bottom, shake_y)
    return ((left, top, z), (right, top, z), (left, bottom, z), (right, bottom, z))


def sprite_pair(**kwargs: float | int) -> tuple[tuple[float, float, float], ...]:
    quad = canonical_quad(**kwargs)
    return quad[0], quad[3]


def visible(quad: tuple[tuple[float, float, float], ...], viewport=(32, 16, 384, 448)) -> bool:
    left, top, width, height = viewport
    xs = [vertex[0] for vertex in quad]
    ys = [vertex[1] for vertex in quad]
    return not (
        max(xs) < left
        or max(ys) < top
        or min(xs) > left + width
        or min(ys) > top + height
    )


def sprite_index(digit: int, timer: int) -> int:
    result = digit
    if timer >= 52:
        result += 11 if timer < 56 else 21
    return result


def popup_final_state(popups, widths, player=(100.0, 100.0), scale=(1.0, 1.0),
                      retained_height=15.0):
    state = {"height": retained_height}
    for popup in popups:
        if not popup["in_use"]:
            continue
        digits = popup["digits"]
        x, y = popup["position"]
        dx = f32(player[0] - x)
        dy = f32(player[1] - y)
        alpha = int(f32(f32(dx * dx) + f32(dy * dy)))
        alpha = 208 if alpha > 4096 else (
            ((alpha - 1024) << 7) // 3072 + 80 if alpha > 1024 else 80
        )
        cursor = f32(x - len(digits) * 4)
        for digit in reversed(digits):
            index = sprite_index(digit, popup["timer"])
            state.update(sprite=index, width=widths[index], x=cursor, y=y,
                         alpha=alpha, scale=scale)
            cursor = f32(cursor + 8.0)
        state["x"] = cursor
    return state


class PspAsciiPopupBatchTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.makefile = read("Makefile.psp")
        cls.ascii = read("src/AsciiManager.cpp")
        cls.anm = read("src/AnmManager.cpp")
        cls.backend = read("src/modern/linux/d3d8_compat.cpp")
        cls.telemetry_h = read("psp/render_perf_telemetry.hpp")
        cls.telemetry_cpp = read("psp/render_perf_telemetry.cpp")
        cls.memory = read("psp/memory_telemetry.cpp")
        cls.main = read("psp/main.cpp")
        cls.fast = function_body(cls.anm, "ZunResult AnmManager::DrawPspAsciiPopupBatch")
        cls.submit = function_body(cls.backend, "bool DrawPspSpritePairs")

    def test_feature_is_default_off_stamped_and_fingerprinted(self) -> None:
        self.assertIn("TH08_PSP_ASCII_POPUP_BATCH ?= 0", self.makefile)
        self.assertIn("-DTH08_PSP_ASCII_POPUP_BATCH=1", self.makefile)
        self.assertIn("ascii-popup-batch-$(TH08_PSP_ASCII_POPUP_BATCH).stamp", self.makefile)
        self.assertIn("ASCII_POPUP_BATCH=%d", self.main)
        self.assertIn("TH08_PSP_FEATURE_ASCII_POPUP_BATCH", self.main)
        self.assertIn("test-psp-ascii-popup-batch", self.makefile)

    def test_score_only_wrapper_retains_atomic_canonical_fallback(self) -> None:
        self.assertIn("this->scorePopups", self.ascii)
        self.assertIn("DrawPspAsciiPopupBatch", self.ascii)
        self.assertIn("if (!scorePopupsDrawnByBatch)", self.ascii)
        self.assertIn("g_AnmManager->DrawNoRotation(&this->smallScoreText)", self.ascii)
        self.assertIn("popup = this->timePopups", self.ascii)
        self.assertIn("popups != g_AsciiManager.scorePopups", self.fast)
        self.assertIn("popupCount != kScorePopupCount", self.fast)
        reserve = self.fast.index("th08_psp_reserve_ascii_popup_sprite_pairs")
        first_vm_write = self.fast.index("vm->pos.x =", reserve)
        self.assertLess(reserve, first_vm_write)

    def test_th08_glyph_ten_animation_has_no_th07_exception(self) -> None:
        self.assertEqual([sprite_index(10, timer) for timer in (51, 52, 55, 56)],
                         [10, 21, 21, 31])
        mapping = function_body(self.anm, "i32 PspAsciiPopupSpriteIndex")
        self.assertNotIn("digit != 10", mapping)
        self.assertIn("popup.timer.current >= 52", mapping)
        self.assertIn("popup.timer.current < 56 ? 11 : 21", mapping)

    def test_fractional_tie_negative_shake_and_all_anchors(self) -> None:
        cases = (
            dict(x=100.25, y=80.75, z=0.4, width=7.0, retained_height=15.0,
                 scale_x=1.0, scale_y=1.0, shake_x=0.25, shake_y=-0.25),
            dict(x=-2.5, y=-3.5, z=0.9, width=8.0, retained_height=14.0,
                 scale_x=0.5, scale_y=1.5, shake_x=-0.5, shake_y=0.5),
        )
        for values in cases:
            for anchor in range(4):
                quad = canonical_quad(anchor=anchor, **values)
                self.assertEqual(sprite_pair(anchor=anchor, **values), (quad[0], quad[3]))
                for vertex in quad:
                    self.assertTrue(all(math.isfinite(component) for component in vertex))
                    self.assertEqual(f32(vertex[0] + 0.5), float(round(f32(vertex[0] + 0.5))))
                    self.assertEqual(f32(vertex[1] + 0.5), float(round(f32(vertex[1] + 0.5))))
        self.assertIn("screenShake", self.fast)
        self.assertIn("nearbyintf(quad[0].pos.x) - g_ZeroPointFive", self.anm)

    def test_viewport_edges_are_inclusive(self) -> None:
        self.assertTrue(visible(((31.0, 20.0, 0.0), (32.0, 20.0, 0.0),
                                 (31.0, 30.0, 0.0), (32.0, 30.0, 0.0))))
        self.assertFalse(visible(((30.0, 20.0, 0.0), (31.5, 20.0, 0.0),
                                  (30.0, 30.0, 0.0), (31.5, 30.0, 0.0))))
        self.assertTrue(visible(((416.0, 20.0, 0.0), (417.0, 20.0, 0.0),
                                 (416.0, 30.0, 0.0), (417.0, 30.0, 0.0))))
        self.assertFalse(visible(((416.5, 20.0, 0.0), (417.5, 20.0, 0.0),
                                  (416.5, 30.0, 0.0), (417.5, 30.0, 0.0))))
        self.assertIn("Preserve DrawInner's inclusive viewport edges exactly", self.anm)

    def test_texture_state_geometry_and_final_vm_contract(self) -> None:
        self.assertIn("batchTexture != sprite->texture", self.fast)
        self.assertIn("vm->spriteSize.x = vm->loadedSprite->widthPx", self.fast)
        self.assertNotIn("vm->spriteSize.y =", self.fast)
        self.assertIn("vm->pos.x += 8.0f", self.fast)
        self.assertIn("vm->color1.a = static_cast<u8>(alpha)", self.fast)
        self.assertIn("vm->scale.x = popupScaleX", self.fast)
        self.assertIn("g_QuadVertices[0].diffuse", self.fast)
        self.assertIn("pair[0] = g_QuadVertices[0]", self.fast)
        self.assertIn("pair[1] = g_QuadVertices[3]", self.fast)
        self.assertIn("((alpha - 1024) << 7) / 3072 + 80", self.fast)
        self.assertIn("MixColors", self.fast)
        # A mixed atlas must fail the validation pass, before VM writes.
        self.assertLess(self.fast.index("batchTexture != sprite->texture"),
                        self.fast.index("vm->pos.x =", self.fast.index("th08_psp_reserve")))

        widths = {index: float(index + 3) for index in range(32)}
        final = popup_final_state(
            (
                {"in_use": True, "digits": [1, 2, 10], "timer": 52,
                 "position": (40.0, 50.0)},
                {"in_use": True, "digits": [10, 3], "timer": 56,
                 "position": (80.0, 90.0)},
            ), widths, scale=(0.75, 1.25), retained_height=17.0)
        # Reverse drawing leaves text[0] as loadedSprite/spriteSize.x, advances
        # X once per glyph, and never adopts the candidate sprite's height.
        self.assertEqual(final["sprite"], 31)
        self.assertEqual(final["width"], widths[31])
        self.assertEqual(final["x"], 88.0)
        self.assertEqual(final["y"], 90.0)
        self.assertEqual(final["height"], 17.0)
        self.assertEqual(final["scale"], (0.75, 1.25))

    def test_pspgl_pair_submit_and_explicit_work_reduction_telemetry(self) -> None:
        self.assertIn("glDrawArrays(GL_SPRITES_PSP", self.submit)
        self.assertNotIn("sceGu", self.submit)
        self.assertIn("spriteCount * 2U", self.submit)
        for name in (
            "asciiPopupBatchCalls", "asciiPopupBatchDigits",
            "asciiPopupBatchSprites", "asciiPopupBatchVerticesSaved",
            "asciiPopupBatchBytesSaved", "asciiPopupBatchFallbacks",
        ):
            self.assertIn(name, self.telemetry_h)
            self.assertIn(name, self.telemetry_cpp)
        self.assertIn("sprites * 4U * 28U", self.telemetry_h)
        self.assertIn("render_ascii_popup_batch_bytes_saved_total", self.memory)


if __name__ == "__main__":
    unittest.main(verbosity=2)
