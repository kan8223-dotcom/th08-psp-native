from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FEATURE = "TH08_PSP_EFFECT_INDEXED_QUADS"
DEPENDENCY = "TH08_PSP_BULLET_UNIFIED_QUADS"


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


class PspEffectIndexedQuadSourceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.anm = (ROOT / "src" / "AnmManager.cpp").read_text(encoding="utf-8")
        cls.anm_h = (ROOT / "src" / "AnmManager.hpp").read_text(
            encoding="utf-8"
        )
        cls.effects = (ROOT / "src" / "EffectManager.cpp").read_text(
            encoding="utf-8"
        )
        cls.bullets = (ROOT / "src" / "BulletManager.cpp").read_text(
            encoding="utf-8"
        )
        cls.makefile = (ROOT / "Makefile.psp").read_text(encoding="utf-8")
        cls.main = (ROOT / "psp" / "main.cpp").read_text(encoding="utf-8")
        cls.telemetry = (ROOT / "psp" / "memory_telemetry.cpp").read_text(
            encoding="utf-8"
        )
        cls.begin = function_body(
            cls.anm, "void AnmManager::BeginPspEffectIndexedQuadBatch()"
        )
        cls.end = function_body(
            cls.anm, "void AnmManager::EndPspEffectIndexedQuadBatch()"
        )
        cls.append = function_body(
            cls.anm, "ZunResult AnmManager::AddSpriteToDrawBuffer("
        )
        cls.flush = function_body(cls.anm, "void AnmManager::FlushVertexBuffer()")
        cls.effect_draw = function_body(
            cls.effects, "ChainCallbackResult EffectManager::OnDraw("
        )
        cls.bullet_layer = function_body(
            cls.effects, "i32 EffectManager::DrawBulletLayerEffects()"
        )
        cls.background = function_body(
            cls.effects, "i32 EffectManager::DrawBackgroundEffects()"
        )

    def test_feature_is_psp_only_default_off_by_undefined_and_dependency_gated(
        self,
    ) -> None:
        # The integration switch is deliberately not added here: every use is
        # a value-tested preprocessor gate, so an undefined macro is OFF.
        for source, token in (
            (self.anm_h, "BeginPspEffectIndexedQuadBatch"),
            (self.effects, "class PspEffectIndexedQuadPass"),
            (self.effects, "PspEffectIndexedQuadPass pspEffectIndexedQuadPass"),
        ):
            position = source.index(token)
            guarded_prefix = source[max(0, position - 320) : position]
            self.assertIn(FEATURE, guarded_prefix)
            self.assertIn(DEPENDENCY, guarded_prefix)
        effect_class = self.effects.index("class PspEffectIndexedQuadPass")
        class_guard = self.effects.rfind("#if", 0, effect_class)
        self.assertIn("defined(PSP)", self.effects[class_guard:effect_class])

    def test_build_toggle_is_default_off_stamped_and_fingerprinted(self) -> None:
        self.assertIn(f"{FEATURE} ?= 0", self.makefile)
        self.assertIn("effect-indexed-quads-0.stamp", self.makefile)
        self.assertIn("effect-indexed-quads-1.stamp", self.makefile)
        self.assertIn(f"-D{FEATURE}=1", self.makefile)
        self.assertIn(
            f"{FEATURE}=1 requires {DEPENDENCY}=1", self.makefile
        )
        self.assertIn("$(EFFECT_INDEXED_QUADS_CONFIG_STAMP)", self.makefile)
        self.assertIn("TH08_PSP_FEATURE_EFFECT_INDEXED_QUADS", self.main)
        self.assertIn("EFFECT_INDEXED_QUADS=%d", self.main)

    def test_owner_isolated_from_bullet_item_and_optional_native_backends(self) -> None:
        owner_enum = re.search(
            r"enum class PspUnifiedQuadBatchOwner : u8\s*\{(?P<body>.*?)\};",
            self.anm,
            re.DOTALL,
        )
        self.assertIsNotNone(owner_enum)
        assert owner_enum is not None
        for owner in ("None", "Item", "Bullet", "Effect"):
            self.assertRegex(owner_enum.group("body"), rf"\b{owner}\b")

        self.assertIn("PspUnifiedQuadBatchOwner::Effect", self.begin)
        self.assertIn("PspUnifiedQuadBatchOwner::Effect", self.end)
        for forbidden in (
            "th08_psp_bullet_direct_ge_set_batch",
            "th08_psp_item_direct_ge_set_batch",
            "th08_psp_bullet_packed_vertex",
            "PspSpritePairRunOwner",
        ):
            self.assertNotIn(forbidden, self.begin)
            self.assertNotIn(forbidden, self.end)

        packed_start = self.append.index(
            "if (g_PspBulletPackedVertexFastpathActive &&"
        )
        packed_gate = self.append[packed_start : packed_start + 240]
        self.assertIn("PspUnifiedQuadBatchOwner::Bullet", packed_gate)
        self.assertNotIn("PspUnifiedQuadBatchOwner::Effect", packed_gate)

        mixed_start = self.append.index("const bool bulletMixedOwner")
        mixed_end = self.append.index(
            "// An invalid indexed range is disarmed by FlushVertexBuffer"
        )
        mixed_and_generic = self.append[mixed_start:mixed_end]
        self.assertIn("PspUnifiedQuadBatchOwner::Bullet", mixed_and_generic)
        self.assertIn("PspUnifiedQuadBatchOwner::Item", mixed_and_generic)
        self.assertNotIn("PspUnifiedQuadBatchOwner::Effect", mixed_and_generic)

    def test_begin_end_are_exact_representation_boundaries(self) -> None:
        self.assertLess(
            self.begin.index("this->FlushVertexBuffer();"),
            self.begin.index("g_PspBulletUnifiedQuadBatchActive = true;"),
        )
        self.assertLess(
            self.begin.index("InitializePspBulletUnifiedQuadIndices();"),
            self.begin.index("g_PspBulletUnifiedQuadBatchActive = true;"),
        )
        self.assertLess(
            self.end.index("this->FlushVertexBuffer();"),
            self.end.index("g_PspBulletUnifiedQuadBatchActive = false;"),
        )
        self.assertIn(
            "g_PspUnifiedQuadBatchOwner == PspUnifiedQuadBatchOwner::Effect",
            self.end,
        )

    def test_raii_brackets_all_three_effect_draw_passes_and_early_return(self) -> None:
        scope = function_body(self.effects, "class PspEffectIndexedQuadPass final")
        self.assertIn("BeginPspEffectIndexedQuadBatch();", scope)
        self.assertIn("EndPspEffectIndexedQuadBatch();", scope)

        for draw in (self.effect_draw, self.bullet_layer, self.background):
            self.assertEqual(draw.count("PspEffectIndexedQuadPass"), 1)

        minimum_return = self.background.index(
            "if (g_Supervisor.cfg.effectQuality == MINIMUM)"
        )
        scope_start = self.background.index("PspEffectIndexedQuadPass")
        moderate_return = self.background.index(
            "g_Supervisor.cfg.effectQuality == MODERATE"
        )
        self.assertLess(minimum_return, scope_start)
        self.assertLess(scope_start, moderate_return)

    def test_bullet_owner_ends_before_bullet_layer_effect_owner_begins(self) -> None:
        bullet_draw = function_body(
            self.bullets, "ChainCallbackResult BulletManager::OnDraw("
        )
        self.assertLess(
            bullet_draw.index("EndPspBulletUnifiedQuadBatch();"),
            bullet_draw.index("g_EffectManager.DrawBulletLayerEffects();"),
        )

    def test_exact_quad_topology_and_four_unique_vertex_storage_are_reused(self) -> None:
        initialize = function_body(
            self.anm, "void InitializePspBulletUnifiedQuadIndices()"
        )
        assignments = (
            "+ 0U] = base;",
            "+ 1U] =\n            static_cast<u16>(base + 1U);",
            "+ 2U] =\n            static_cast<u16>(base + 2U);",
            "+ 3U] =\n            static_cast<u16>(base + 1U);",
            "+ 4U] =\n            static_cast<u16>(base + 2U);",
            "+ 5U] =\n            static_cast<u16>(base + 3U);",
        )
        for assignment in assignments:
            self.assertIn(assignment, initialize)

        indexed_boundary = self.append.index(
            "// An invalid indexed range is disarmed by FlushVertexBuffer"
        )
        indexed_append = self.append[:indexed_boundary]
        for corner in range(4):
            self.assertIn(
                f"this->vertexBufferEndPtr[{corner}] = vertices[{corner}];",
                indexed_append,
            )
        self.assertIn("this->vertexBufferEndPtr += 4;", indexed_append)
        self.assertIn("DrawIndexedPrimitiveUP(", self.flush)

    def test_radial_trails_keep_their_canonical_direct_strip_boundary(self) -> None:
        radial = function_body(
            self.effects, "i32 __fastcall DrawRadialTrail(Effect *effect)\n{"
        )
        draw_vertices = function_body(
            self.anm, "ZunResult AnmManager::DrawVertices("
        )
        self.assertIn("g_AnmManager->DrawVertices(", radial)
        self.assertLess(
            draw_vertices.index("this->FlushVertexBuffer();"),
            draw_vertices.index("DrawPrimitiveUP(D3DPT_TRIANGLESTRIP"),
        )
        self.assertNotIn(FEATURE, radial)

    def test_effect_simulation_and_replay_state_are_untouched(self) -> None:
        update = function_body(
            self.effects,
            "ChainCallbackResult EffectManager::OnUpdate(EffectManager *effectManager)",
        )
        self.assertNotIn(FEATURE, update)
        self.assertNotIn("IndexedQuad", update)
        for signature in (
            "Effect *EffectManager::SpawnEffect(",
            "Effect *EffectManager::SpawnEffectWithVelocity(",
            "Effect *EffectManager::SpawnEffectInFixedSlot(",
        ):
            spawn = function_body(self.effects, signature)
            self.assertNotIn(FEATURE, spawn)
            self.assertNotIn("IndexedQuad", spawn)

    def test_success_telemetry_counts_only_the_physical_6v_to_4v_saving(self) -> None:
        success = function_body(
            self.anm, "void NotePspEffectIndexedQuadSuccess(u32 quads)"
        )
        self.assertIn("++g_PspEffectIndexedQuadStats.batches;", success)
        self.assertIn(
            "g_PspEffectIndexedQuadStats.successfulOrdinaryQuads += quads;",
            success,
        )
        self.assertIn("const u32 savedVertices = quads * 2U;", success)
        self.assertIn(
            "savedVertices * sizeof(VertexTex1DiffuseXyzrhw)", success
        )

    def test_effect_telemetry_is_consumed_on_the_render_interval_boundary(self) -> None:
        prefix = self.telemetry.index("EFFECT_INDEXED_QUADS_TELEMETRY")
        record = self.telemetry[prefix : prefix + 2600]
        for declaration in (
            "counter_scope=stage_relative_interval",
            "cumulative=0 interval_consumed=1 owner=game_frame_thread",
            "marks_peek=0 ordinary_effect_only=1 radial_trail_excluded=1",
            "topology=6v_to_4v_indexed render_perf_vertices=logical_6v",
            "successfulQuads * 6U",
            "successfulQuads * 4U",
        ):
            self.assertIn(declaration, record)

        log_snapshot = function_body(
            self.telemetry,
            "void LogSnapshot(const char *kind, const char *phase, std::uint32_t elapsedFrames,",
        )
        effect_block = log_snapshot[log_snapshot.index(
            "#if TH08_PSP_EFFECT_INDEXED_QUADS_ENABLED"
        ) :]
        self.assertIn("if (renderPerf != nullptr)", effect_block)

    def test_renderperf_keeps_canonical_logical_six_vertex_workload(self) -> None:
        self.assertIn(
            "RenderPerfFpsOverlayDrawScope fpsOverlayDrawScope(\n"
            "        this->spritesToDraw * 6U);",
            self.flush,
        )
        self.assertIn("DrawIndexedPrimitiveUP(", self.flush)
        self.assertIn(
            "RenderPerfNoteDraw reports logical submitted vertices", self.flush
        )


if __name__ == "__main__":
    unittest.main()
