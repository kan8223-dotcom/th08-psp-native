#!/usr/bin/env python3
"""Host differential and source-contract gates for Effect sprite-pair M0."""

from __future__ import annotations

import pathlib
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


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
                return source[start : index + 1]
    raise AssertionError(f"unterminated function: {signature}")


class PspEffectSpritePairAuditTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.makefile = read("Makefile.psp")
        cls.main = read("psp/main.cpp")
        cls.effect = read("src/EffectManager.cpp")
        cls.geometry = read("psp/effect_sprite_pair_audit.hpp")
        cls.audit = function_body(
            cls.effect, "ZunResult PspAuditOrdinaryEffectSpritePair"
        )
        cls.on_draw = function_body(
            cls.effect, "ChainCallbackResult EffectManager::OnDraw"
        )
        cls.bullet_layer = function_body(
            cls.effect, "i32 EffectManager::DrawBulletLayerEffects"
        )
        cls.background = function_body(
            cls.effect, "i32 EffectManager::DrawBackgroundEffects"
        )

    def test_actual_header_matches_independent_host_model(self) -> None:
        with tempfile.TemporaryDirectory(
            prefix="th08-effect-sprite-pair-"
        ) as temp:
            binary = pathlib.Path(temp) / "effect-sprite-pair"
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
                    str(
                        ROOT
                        / "tools"
                        / "psp_effect_sprite_pair_audit_harness.cpp"
                    ),
                    "-o",
                    str(binary),
                ],
                cwd=ROOT,
                check=True,
            )
            result = subprocess.run(
                [str(binary)],
                cwd=ROOT,
                check=True,
                text=True,
                capture_output=True,
            )
            self.assertIn(
                "effect-sprite-pair: PASS samples=250000", result.stdout
            )

    def test_mode_is_default_off_stamped_and_fingerprinted(self) -> None:
        self.assertIn(
            "TH08_PSP_EFFECT_SPRITE_PAIR_AUDIT ?= 0", self.makefile
        )
        self.assertIn(
            "-DTH08_PSP_EFFECT_SPRITE_PAIR_AUDIT=1", self.makefile
        )
        self.assertIn(
            "effect-sprite-pair-audit-$(TH08_PSP_EFFECT_SPRITE_PAIR_AUDIT).stamp",
            self.makefile,
        )
        self.assertIn(
            "requires the canonical six-vertex Effect path", self.makefile
        )
        self.assertIn(
            "EFFECT_SPRITE_PAIR_AUDIT=%d", self.main
        )
        self.assertIn(
            "TH08_PSP_FEATURE_EFFECT_SPRITE_PAIR_AUDIT", self.main
        )
        self.assertIn(
            "test-psp-effect-sprite-pair-audit", self.makefile
        )

    def test_m0_is_canonical_authoritative_and_has_no_product_backend(self) -> None:
        candidate = self.audit.index(
            "BuildEffectSpritePairCanonicalQuad"
        )
        canonical = self.audit.index(
            "g_AnmManager->Draw2D(&effect->vm)", candidate
        )
        quad_compare = self.audit.index(
            "memcmp(candidate, g_QuadVertices, sizeof(candidate))",
            canonical,
        )
        self.assertLess(candidate, canonical)
        self.assertLess(canonical, quad_compare)
        self.assertEqual(
            self.audit.count("g_AnmManager->Draw2D(&effect->vm)"), 1
        )
        for forbidden in (
            "DrawPrimitive",
            "DrawIndexedPrimitive",
            "AddSpriteToDrawBuffer",
            "TryDraw",
            "FASTPATH",
            "g_Rng",
            "GetRandom",
        ):
            self.assertNotIn(forbidden, self.audit)
        self.assertIn(
            "The experiment ends here: canonical Draw2D is always the only writer",
            self.audit,
        )

    def test_m0_compares_complete_game_and_presentation_contract(self) -> None:
        required = (
            "u8 effectBefore[sizeof(Effect)]",
            "u8 vmBefore[sizeof(AnmVm)]",
            "memcmp(effectBefore, effect, sizeof(effectBefore))",
            "memcmp(vmBefore, &effect->vm, sizeof(vmBefore))",
            "PspEffectSpritePairSlotIndex",
            "EffectSpritePairNoteOrder",
            "PspEffectSpritePairPositionBytesEqual",
            "PspEffectSpritePairUvBytesEqual",
            "PspEffectSpritePairColorBytesEqual",
            "ReconstructEffectSpritePairQuad",
            "PspEffectSpritePairCanonicalBufferMatches",
            "PspEffectSpritePairCanonicalStateMatches",
            "currentTexture",
            "currentBlendMode",
            "currentVertexShader",
            "disableZWrite",
            "renderStateChangesThisFrame",
            "flushesThisFrame",
            "PSP_EFFECT_PAIR_FALLBACK_NONFINITE",
            "PspEffectSpritePairNoteFallback",
        )
        for token in required:
            self.assertIn(token, self.effect)

    def test_all_ordinary_effect_groups_use_the_same_audit_wrapper(self) -> None:
        self.assertIn("pspEffectSpritePairGroup0.Draw(effect)", self.on_draw)
        self.assertIn("pspEffectSpritePairGroup4.Draw(effect)", self.on_draw)
        self.assertIn(
            "pspEffectSpritePairGroup3.Draw(effect)", self.bullet_layer
        )
        self.assertIn(
            "pspEffectSpritePairGroup1.Draw(effect)", self.background
        )
        total_wrappers = (
            self.on_draw.count(".Draw(effect)")
            + self.bullet_layer.count(".Draw(effect)")
            + self.background.count(".Draw(effect)")
        )
        # Each call site now has one mutually-exclusive M0 branch and one
        # default-off product branch.  Both use the same four list positions.
        self.assertEqual(total_wrappers, 8)
        self.assertIn("effect->drawCallback != NULL", self.on_draw)
        self.assertIn("effect = effect->nextInDrawGroup", self.on_draw)
        self.assertIn("effect = effect->nextInDrawGroup", self.bullet_layer)
        self.assertIn("effect = effect->nextInDrawGroup", self.background)

    def test_geometry_preserves_exact_draw_no_rotation_semantics(self) -> None:
        build = function_body(
            self.geometry, "inline void BuildEffectSpritePairCanonicalQuad"
        )
        self.assertNotIn(".w =", build)
        self.assertNotIn(".diffuse =", build)
        order = (
            "spriteHalfWidth",
            "spriteHalfHeight",
            "quad[0].pos.x += shakeX",
            "std::nearbyint(quad[0].pos.x)",
            "vm.loadedSprite->uvStart.x + vm.uvScrollPos.x",
        )
        offsets = [build.index(token) for token in order]
        self.assertEqual(offsets, sorted(offsets))
        self.assertIn(
            "EffectSpritePairFloatBitsEqual(quad[0].pos.x, quad[2].pos.x)",
            self.geometry,
        )
        self.assertIn(
            "EffectSpritePairFloatBits(quad[0].w)", self.geometry
        )
        self.assertIn("AreaOrMirror", self.geometry)
        self.assertIn("EffectSpritePairFinite", self.geometry)

    def test_fallback_remains_one_canonical_call_and_reports_mismatches(self) -> None:
        self.assertIn("canonicalFallbacks", self.effect)
        self.assertIn("fallbackCounts", self.effect)
        self.assertIn("EFFECT_SPRITE_PAIR_M0 mismatch=", self.effect)
        self.assertIn("EFFECT_SPRITE_PAIR_M0 frame=", self.effect)
        self.assertIn("EFFECT_SPRITE_PAIR_M0_FALLBACK frame=", self.effect)
        for reason in (
            "slot=%lu",
            "order=%lu",
            "callback=%lu",
            "rotation=%lu",
            "visibility=%lu",
            "sprite=%lu",
            "texture=%lu",
            "scale=%lu",
            "nonfinite=%lu",
            "z_or_w=%lu",
            "diffuse=%lu",
            "axis=%lu",
            "uv=%lu",
            "area_or_mirror=%lu",
            "state=%lu",
            "canonical_mismatch=%lu",
        ):
            self.assertIn(reason, self.effect)
        self.assertIn("orderHash", self.effect)
        self.assertIn("order_hash=%08lx", self.effect)
        self.assertNotIn("semanticHash", self.effect)

        detail = function_body(
            self.effect, "void PspEffectSpritePairNoteMismatch"
        )
        for counter in (
            "effectMismatches",
            "vmMismatches",
            "quadMismatches",
            "positionMismatches",
            "uvMismatches",
            "colorMismatches",
            "pairMismatches",
            "bufferMismatches",
            "stateMismatches",
            "textureMismatches",
            "blendMismatches",
            "slotMismatches",
            "orderMismatches",
        ):
            self.assertIn(counter, detail)
        cap = detail.index("mismatchDetailLogs >= 8U")
        increment = detail.index("++g_PspEffectSpritePairAuditStats.mismatchDetailLogs")
        output = detail.index("th08::psp::BootLog(")
        self.assertLess(cap, increment)
        self.assertLess(increment, output)
        self.assertNotIn("fprintf(stderr", self.effect)
        self.assertIn('#include "fileio.hpp"', self.effect)
        self.assertIn("detail=%lu/8", detail)
        for kind in (
            '"position_bytes"',
            '"uv_bytes"',
            '"color_bytes"',
            '"pair_reconstruction"',
            '"texture_state"',
            '"blend_state"',
        ):
            self.assertIn(kind, self.effect)
        self.assertNotIn("TH08_PSP_EFFECT_SPRITE_PAIR_FASTPATH", self.audit)

    def test_reporting_survives_render_cadence_and_flushes_teardown(self) -> None:
        begin = function_body(
            self.effect, "void PspEffectSpritePairBeginFrame"
        )
        self.assertIn("frame > g_PspEffectSpritePairAuditFrame", begin)
        self.assertIn("frame >= g_PspEffectSpritePairAuditNextReportFrame", begin)
        self.assertIn('"periodic"', begin)
        self.assertNotIn("% 600U == 0U", begin)

        report = function_body(
            self.effect, "void PspEffectSpritePairReport"
        )
        self.assertEqual(report.count("th08::psp::BootLog("), 2)
        self.assertIn("reason=%s", report)
        self.assertIn("last_draw_frame=%lu", report)
        self.assertIn("crossed_stage_frame=%lu", report)
        self.assertIn("g_PspEffectSpritePairAuditLastReportedPasses", report)

        release = function_body(
            self.effect, "ZunResult EffectManager::ReleaseEffectResources"
        )
        flush = release.index('PspEffectSpritePairFinalize("teardown")')
        free_loop = release.index("Effect *effect = effectManager->effects")
        self.assertLess(flush, free_loop)
        self.assertIn("PspEffectSpritePairResetGeneration", self.effect)


if __name__ == "__main__":
    unittest.main()
