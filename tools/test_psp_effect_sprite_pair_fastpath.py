#!/usr/bin/env python3
"""Host geometry/order and source-contract gates for Effect pair product."""

from __future__ import annotations

import pathlib
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def braced(source: str, signature: str, start: int = 0) -> str:
    begin = source.index(signature, start)
    opening = source.index("{", begin)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[begin : index + 1]
    raise AssertionError(f"unterminated block: {signature}")


class PspEffectSpritePairFastpathTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.makefile = read("Makefile.psp")
        cls.main = read("psp/main.cpp")
        cls.effect = read("src/EffectManager.cpp")
        cls.geometry = read("psp/effect_sprite_pair_audit.hpp")
        cls.backend = read("src/modern/linux/d3d8_compat.cpp")
        cls.backend_header = read("src/modern/linux/d3d8_internal.hpp")
        cls.product = braced(
            cls.effect, "class PspEffectSpritePairProductPass final"
        )
        cls.draw = braced(cls.product, "ZunResult Draw(Effect *effect)")
        cls.fallback = braced(cls.product, "ZunResult CanonicalFallback")
        cls.begin_run = braced(cls.product, "void BeginRun")
        cls.queue = braced(cls.product, "ZunResult QueueVisible")
        cls.flush = braced(cls.product, "void Flush()", cls.product.index("private:"))
        cls.on_draw = braced(
            cls.effect, "ChainCallbackResult EffectManager::OnDraw"
        )
        cls.bullet_layer = braced(
            cls.effect, "i32 EffectManager::DrawBulletLayerEffects"
        )
        cls.background = braced(
            cls.effect, "i32 EffectManager::DrawBackgroundEffects"
        )
        cls.backend_in_place = braced(
            cls.backend, "bool DrawPspSpritePairsInPlace"
        )

    def compile_and_run(self, source: str, expected: str) -> None:
        with tempfile.TemporaryDirectory(
            prefix="th08-effect-pair-product-"
        ) as temp:
            binary = pathlib.Path(temp) / pathlib.Path(source).stem
            subprocess.run(
                [
                    "g++",
                    "-std=c++17",
                    "-O2",
                    "-ffp-contract=off",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(ROOT),
                    str(ROOT / source),
                    "-o",
                    str(binary),
                ],
                cwd=ROOT,
                check=True,
            )
            result = subprocess.run(
                [str(binary)], cwd=ROOT, check=True, text=True,
                capture_output=True
            )
            self.assertIn(expected, result.stdout)

    def test_randomized_geometry_and_order_models(self) -> None:
        self.compile_and_run(
            "tools/psp_effect_sprite_pair_audit_harness.cpp",
            "effect-sprite-pair: PASS samples=250000",
        )
        self.compile_and_run(
            "tools/psp_effect_sprite_pair_fastpath_harness.cpp",
            "effect-sprite-pair-fastpath: PASS key_samples=250000 "
            "order_samples=250000",
        )

    def test_default_off_stamp_fingerprint_and_dependency_gate(self) -> None:
        for token in (
            "TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH ?= 0",
            "-DTH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH=1",
            "effect-sprite-pair-fastpath-$(TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH).stamp",
            "TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH must be 0 or 1",
            "TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH and TH08_PSP_EFFECT_SPRITE_PAIR_AUDIT are mutually exclusive",
            "TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH requires the canonical Effect frontend",
            "$(EFFECT_SPRITE_PAIR_FASTPATH_CONFIG_STAMP)",
            "test-psp-effect-sprite-pair-fastpath",
        ):
            self.assertIn(token, self.makefile)
        dependency = (
            "src/EffectManager.o src/modern/linux/d3d8_compat.o psp/main.o: \\\n"
            "\t$(EFFECT_SPRITE_PAIR_FASTPATH_CONFIG_STAMP)"
        )
        self.assertIn(dependency, self.makefile)
        self.assertIn("EFFECT_SPRITE_PAIR_FASTPATH=%d", self.main)
        self.assertIn(
            "TH08_PSP_FEATURE_EFFECT_SPRITE_PAIR_FASTPATH", self.main
        )
        native_link_gate = (
            'test "$(TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH)" = "1"; then'
        )
        self.assertIn(native_link_gate, self.makefile)
        self.assertIn(
            "__pspgl_th08_draw_native_sprite_pairs_copy",
            self.makefile[self.makefile.index(native_link_gate):],
        )

    def test_product_uses_exact_m0_eligibility_order(self) -> None:
        ordered = (
            "PspEffectSpritePairProductSlotIndex",
            "EffectSpritePairNoteOrder",
            "effect->drawCallback != NULL",
            "effect->vm.rotation.z != 0.0f",
            "!effect->vm.visible",
            "effect->vm.loadedSprite == NULL",
            "BuildEffectSpritePairCanonicalQuad",
            "EffectSpritePairQuadFinite",
            "EffectSpritePairCanonicalQuadVisible",
            "PspEffectSpritePairProductSetDiffuse",
            "effect->vm.loadedSprite->texture == NULL",
            "!(effect->vm.scale.x > 0.0f)",
            "g_Supervisor.d3dDevice == NULL",
            "effect->vm.blendMode > AnmBlendMode_Additive",
            "ClassifyEffectSpritePairQuad",
        )
        positions = [self.draw.index(token) for token in ordered]
        self.assertEqual(positions, sorted(positions))
        for reason in (
            "FALLBACK_ROTATION",
            "FALLBACK_VISIBILITY",
            "FALLBACK_SCALE",
            "FALLBACK_NONFINITE",
            "FALLBACK_Z_OR_W",
            "FALLBACK_DIFFUSE",
            "FALLBACK_AXIS",
            "FALLBACK_UV",
            "FALLBACK_AREA_OR_MIRROR",
            "FALLBACK_STATE",
        ):
            self.assertIn(reason, self.effect)

    def test_cull_and_fallback_preserve_canonical_scratch_and_call_count(self) -> None:
        commit = self.draw.index(
            "memcpy(g_QuadVertices, candidate, sizeof(candidate))"
        )
        cull = self.draw.index("if (!visible)", commit)
        queue = self.draw.index("return QueueVisible(effect, candidate)")
        self.assertLess(commit, cull)
        self.assertLess(cull, queue)
        self.assertNotIn("Draw2D", self.draw)
        self.assertEqual(self.fallback.count("manager_->Draw2D(&effect->vm)"), 1)
        self.assertLess(self.fallback.index("Flush();"),
                        self.fallback.index("manager_->Draw2D"))
        self.assertIn("PspEffectSpritePairProductNoteFallback", self.fallback)
        self.assertIn("memcpy(preservedQuad, g_QuadVertices", self.flush)
        self.assertIn("PspEffectSpritePairProductLoadVm", self.flush)
        self.assertEqual(self.flush.count("manager_->Draw2D(vm)"), 1)
        self.assertIn(
            "memcpy(g_QuadVertices, preservedQuad", self.flush
        )

    def test_native_run_is_adjacent_ordered_and_current_list_owned(self) -> None:
        self.assertIn("MakeEffectSpritePairRunKey", self.queue)
        self.assertIn("EffectSpritePairRunKeysEqual", self.queue)
        for field in (
            "std::uintptr_t texture",
            "std::uint32_t blendMode",
            "std::uint32_t zWriteDisabled",
            "std::uint32_t depthTestDisabled",
        ):
            self.assertIn(field, self.geometry)
        self.assertIn(
            "left.depthTestDisabled != 0U ||\n"
            "            left.zWriteDisabled == right.zWriteDisabled",
            self.geometry,
        )
        self.assertIn("effect->vm.zWriteDisabled", self.queue)
        self.assertIn("g_Supervisor.IsDepthTestDisabled() != 0", self.queue)
        self.assertIn("BuildEffectSpritePair(quad, pair)", self.queue)
        self.assertIn("PspEffectSpritePairProductStoreVm", self.queue)
        self.assertLess(
            self.begin_run.index("manager_->FlushVertexBuffer()"),
            self.begin_run.index("manager_->vertexBufferStartPtr"),
        )
        self.assertIn("SetRenderStateForVm(lastVm_)", self.flush)
        self.assertIn("pairCount - 1U", self.flush)
        self.assertIn("th08_psp_draw_sprite_pairs_in_place", self.flush)
        self.assertNotIn("sceGuStart", self.product)
        self.assertNotIn("DrawPrimitive", self.product)
        self.assertNotIn("DrawIndexedPrimitive", self.product)
        self.assertIn(
            "__pspgl_th08_draw_native_sprite_pairs_copy",
            self.backend_in_place,
        )
        self.assertNotIn("glDrawArrays(", self.backend_in_place)
        self.assertNotIn("sceGuStart", self.backend_in_place)
        self.assertLess(
            self.backend_in_place.index("PrepareState(true)"),
            self.backend_in_place.index(
                "__pspgl_th08_draw_native_sprite_pairs_copy"
            ),
        )
        self.assertIn(
            "TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH", self.backend_header
        )
        self.assertIn(
            "TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH_ENABLED",
            self.backend,
        )

    def test_all_four_groups_and_nonordinary_boundaries_are_bracketed(self) -> None:
        for name in ("Group0", "Group4"):
            self.assertIn(
                f"pspEffectSpritePair{name}.Draw(effect)", self.on_draw
            )
            self.assertIn(
                f"pspEffectSpritePair{name}.Finish()", self.on_draw
            )
            self.assertIn(
                f"pspEffectSpritePair{name}.CallbackBoundary()",
                self.on_draw,
            )
        self.assertLess(
            self.on_draw.index("pspEffectSpritePairGroup0.Finish()"),
            self.on_draw.index(
                "effectManager->drawGroupSentinel2.nextInDrawGroup"
            ),
        )
        self.assertIn(
            "pspEffectSpritePairGroup3.Draw(effect)", self.bullet_layer
        )
        self.assertIn(
            "pspEffectSpritePairGroup3.CallbackBoundary()",
            self.bullet_layer,
        )
        self.assertIn(
            "pspEffectSpritePairGroup3.Finish()", self.bullet_layer
        )
        self.assertIn(
            "pspEffectSpritePairGroup1.Draw(effect)", self.background
        )
        self.assertGreaterEqual(
            self.background.count("pspEffectSpritePairGroup1.Boundary()"),
            2,
        )
        self.assertIn(
            "pspEffectSpritePairGroup1.Finish()", self.background
        )
        for body in (self.on_draw, self.bullet_layer, self.background):
            self.assertIn("effect = effect->nextInDrawGroup", body)

    def test_no_product_heap_frame_log_me_or_raw_ge_path(self) -> None:
        for forbidden in (
            "malloc(", "calloc(", "realloc(", "new ", "fprintf(",
            "stderr", "sceGuStart", "Media Engine", "TH08_PSP_ME",
        ):
            self.assertNotIn(forbidden, self.product)
        report = braced(
            self.effect, "void PspEffectSpritePairProductReport"
        )
        self.assertEqual(report.count("th08::psp::BootLog("), 2)
        release = braced(
            self.effect, "ZunResult EffectManager::ReleaseEffectResources"
        )
        self.assertLess(
            release.index("PspEffectSpritePairProductReport()"),
            release.index("Effect *effect = effectManager->effects"),
        )
        self.assertNotIn("BootLog", self.draw)
        self.assertNotIn("BootLog", self.queue)
        self.assertNotIn("BootLog", self.flush)


if __name__ == "__main__":
    unittest.main()
