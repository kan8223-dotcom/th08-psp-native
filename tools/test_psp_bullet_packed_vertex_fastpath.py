#!/usr/bin/env python3
"""Focused static/differential gates for Bullet packed-vertex M1."""

from __future__ import annotations

import random
import struct
import unittest
from pathlib import Path

try:
    from tools.test_psp_bullet_packed_vertex_audit import (
        effective_color as canonical_effective_color,
        pack_final as canonical_pack_final,
    )
except ModuleNotFoundError:  # Direct `python3 tools/test_*.py` invocation.
    from test_psp_bullet_packed_vertex_audit import (  # type: ignore[no-redef]
        effective_color as canonical_effective_color,
        pack_final as canonical_pack_final,
    )


ROOT = Path(__file__).resolve().parents[1]
ANM = (ROOT / "src/AnmManager.cpp").read_text(encoding="utf-8")
ASCII = (ROOT / "src/AsciiManager.cpp").read_text(encoding="utf-8")
ASCII_HPP = (ROOT / "src/AsciiManager.hpp").read_text(encoding="utf-8")
SUPERVISOR = (ROOT / "src/Supervisor.cpp").read_text(encoding="utf-8")
D3D = (ROOT / "src/modern/linux/d3d8_compat.cpp").read_text(encoding="utf-8")
INTERNAL = (ROOT / "src/modern/linux/d3d8_internal.hpp").read_text(
    encoding="utf-8"
)
MAKEFILE = (ROOT / "Makefile.psp").read_text(encoding="utf-8")
MAIN = (ROOT / "psp/main.cpp").read_text(encoding="utf-8")
MEMORY = (ROOT / "psp/memory_telemetry.cpp").read_text(encoding="utf-8")
RENDER_PERF = (ROOT / "psp/render_perf_telemetry.cpp").read_text(
    encoding="utf-8"
)
RENDER_PERF_HPP = (ROOT / "psp/render_perf_telemetry.hpp").read_text(
    encoding="utf-8"
)
FEATURE = "TH08_PSP_BULLET_PACKED_VERTEX_FASTPATH"


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


def f32(value: float) -> float:
    return struct.unpack("<f", struct.pack("<f", value))[0]


def effective_color_forced_diffuse_arg2(
    diffuse: int,
    factor: int,
    color_op: int,
    color_arg1: int,
    alpha_op: int,
    alpha_arg1: int,
) -> int:
    texture, tfactor, diffuse_arg = 1, 2, 3

    def argument(kind: int) -> int:
        if kind == texture:
            return 0xFFFFFFFF
        if kind == tfactor:
            return factor
        if kind == diffuse_arg:
            return diffuse
        raise AssertionError(kind)

    color1 = argument(color_arg1)
    color2 = diffuse
    alpha1 = argument(alpha_arg1)
    alpha2 = diffuse
    if color_op == 4:
        r = (((color1 >> 16) & 255) * ((color2 >> 16) & 255) + 127) // 255
        g = (((color1 >> 8) & 255) * ((color2 >> 8) & 255) + 127) // 255
        b = ((color1 & 255) * (color2 & 255) + 127) // 255
        rgb = (r << 16) | (g << 8) | b
    else:
        rgb = color1 & 0x00FFFFFF
    if alpha_op == 4:
        alpha = (
            (((alpha1 >> 24) & 255) * ((alpha2 >> 24) & 255) + 127) // 255
        ) << 24
    else:
        alpha = alpha1 & 0xFF000000
    return alpha | rgb


def pack_product(source: bytes, color: int) -> bytes:
    x, y, z, _rhw, _raw, u, v = struct.unpack("<ffffIff", source)
    return struct.pack(
        "<ffBBBBfff",
        u,
        v,
        (color >> 16) & 255,
        (color >> 8) & 255,
        color & 255,
        (color >> 24) & 255,
        f32(f32(x) + f32(0.5)),
        f32(f32(y) + f32(0.5)),
        f32(f32(1.0) - f32(f32(2.0) * f32(z))),
    )


class PackedVertexFastpathSourceTests(unittest.TestCase):
    def test_default_off_requirements_exclusions_stamp_and_identity(self) -> None:
        self.assertIn(f"{FEATURE} ?= 0", MAKEFILE)
        self.assertIn("bullet-packed-vertex-fastpath-0.stamp", MAKEFILE)
        self.assertIn("bullet-packed-vertex-fastpath-1.stamp", MAKEFILE)
        self.assertIn(f"-D{FEATURE}=1", MAKEFILE)
        self.assertIn(f"{FEATURE}=1 requires TH08_PSP_BULLET_UNIFIED_QUADS=1", MAKEFILE)
        self.assertIn(f"{FEATURE}=1 requires TH08_PSP_BULLET_DIRECT_GE=1", MAKEFILE)
        self.assertIn(f"{FEATURE} and TH08_PSP_BULLET_PACKED_VERTEX_AUDIT", MAKEFILE)
        self.assertIn(f"{FEATURE} and mixed 2V quad modes", MAKEFILE)
        self.assertIn("$(BULLET_PACKED_VERTEX_FASTPATH_CONFIG_STAMP)", MAKEFILE)
        self.assertIn("TH08_PSP_FEATURE_BULLET_PACKED_VERTEX_FASTPATH", MAIN)
        self.assertIn("BULLET_PACKED_VERTEX_FASTPATH=%d", MAIN)

    def test_off_on_renderer_and_frontend_storage_are_fixed(self) -> None:
        self.assertIn(
            "PspBulletPackedVertexFastpathStorageStats\n        "
            "pspBulletPackedVertexFastpath{};",
            D3D,
        )
        self.assertIn("Fixed in every Bullet direct-GE build", D3D)
        self.assertIn("fixed renderer reservation diverged", D3D)
        self.assertIn("g_PspBulletPackedVertexFastpathActive", ANM)
        self.assertIn("__attribute__((used)) = false;", ANM)
        declaration = ANM.index("bool g_PspBulletPackedVertexFastpathActive")
        self.assertNotIn(FEATURE, ANM[declaration - 220 : declaration])
        self.assertIn("M1 OFF/ON preserves BSS geometry", ANM)

    def test_fps_and_replay_overlay_owner_scope_is_narrow_and_nested_safe(self) -> None:
        self.assertIn("enum class PspAsciiRenderOwner", ASCII_HPP)
        self.assertIn("FpsCounter", ASCII_HPP)
        self.assertIn("ReplayFpsDiagnostic", ASCII_HPP)
        self.assertIn("PspAsciiRenderOwner previousOwner_", ASCII_HPP)
        owner_signature = "PspAsciiRenderOwnerScope::PspAsciiRenderOwnerScope("
        owner_start = ASCII.index(owner_signature)
        owner_constructor_declaration = ASCII[owner_start : owner_start + 320]
        owner_constructor = body(ASCII, owner_signature)
        owner_destructor = body(
            ASCII,
            "PspAsciiRenderOwnerScope::~PspAsciiRenderOwnerScope()",
        )
        self.assertIn(
            "previousOwner_(g_PspAsciiCurrentRenderOwner)",
            owner_constructor_declaration,
        )
        self.assertIn("g_PspAsciiCurrentRenderOwner = owner", owner_constructor)
        self.assertIn(
            "g_PspAsciiCurrentRenderOwner = previousOwner_", owner_destructor
        )

        calculate = body(SUPERVISOR, "void Supervisor::CalculateFps(")
        fps_scope = calculate.index("PspAsciiRenderOwner::FpsCounter")
        fps_add = calculate.index(
            "g_AsciiManager.AddString(&fpsCounterPos, g_SupervisorFpsBuffer)",
            fps_scope,
        )
        replay_scope = calculate.index(
            "PspAsciiRenderOwner::ReplayFpsDiagnostic", fps_add
        )
        replay_add = calculate.index(
            "g_AsciiManager.AddString(&debugCounterPos,", replay_scope
        )
        self.assertLess(fps_scope, fps_add)
        self.assertLess(replay_scope, replay_add)

    def test_ascii_owner_tags_only_accepted_and_actually_appended_glyphs(self) -> None:
        add = body(ASCII, "void AsciiManager::AddString(")
        self.assertLess(
            add.index("if (this->numStrings >="),
            add.index("g_PspAsciiRenderOwners.strings[stringIndex]"),
        )
        self.assertLess(
            add.index("this->numStrings++"),
            add.index("g_PspAsciiRenderOwners.strings[stringIndex]"),
        )

        draw = body(ASCII, "void AsciiManager::OnDrawLowPrioImpl()")
        newline = draw.index("if (*text == '\\n')")
        space = draw.index("else if (*text == ' ')", newline)
        owner_draw = draw.index("PspDrawFpsOverlayGlyph(&this->largeText)", space)
        self.assertLess(newline, space)
        self.assertLess(space, owner_draw)

        glyph = body(ASCII, "void PspDrawFpsOverlayGlyph(")
        self.assertIn("g_AnmManager->DrawNoRotation(vm)", glyph)
        self.assertIn("result != ZUN_SUCCESS", glyph)
        self.assertIn("appendedWithoutFlush", glyph)
        self.assertIn("appendedAfterFlush", glyph)
        self.assertIn("RenderPerfQueueFpsOverlayVertices(6U)", glyph)

    def test_overlay_counter_is_fixed_and_commits_only_a_submitted_batch(self) -> None:
        for token in (
            "std::uint64_t fpsOverlayVertices",
            "std::uint64_t gameVertices",
            "std::uint32_t fpsOverlayVertices",
            "std::uint32_t gameVertices",
            "class RenderPerfFpsOverlayDrawScope",
        ):
            self.assertIn(token, RENDER_PERF_HPP)
        declaration = RENDER_PERF_HPP.index("class RenderPerfFpsOverlayDrawScope")
        self.assertNotIn(FEATURE, RENDER_PERF_HPP[max(0, declaration - 400) : declaration])

        flush = body(ANM, "void AnmManager::FlushVertexBuffer()")
        self.assertIn("RenderPerfFpsOverlayDrawScope fpsOverlayDrawScope", flush)
        self.assertIn("this->spritesToDraw * 6U", flush)
        clear = body(ANM, "void AnmManager::ClearVertexBuffer()")
        self.assertIn("RenderPerfDiscardQueuedFpsOverlayVertices", clear)

        scope = body(
            RENDER_PERF,
            "RenderPerfFpsOverlayDrawScope::~RenderPerfFpsOverlayDrawScope()",
        )
        self.assertIn("gRenderPerfFpsOverlaySubmittedVertices", scope)
        self.assertIn(
            "gRenderPerfFpsOverlaySubmittedVertices ==", scope
        )
        self.assertIn("gRenderPerfCurrentFrame.fpsOverlayVertices", scope)
        end_frame = body(RENDER_PERF, "void RenderPerfTelemetryEndFrame()")
        self.assertIn("completed.vertices - completed.fpsOverlayVertices", end_frame)
        for field in (
            "render_fps_overlay_vertices_total=%llu",
            "render_fps_overlay_vertices_peak=%llu",
            "render_game_vertices_total=%llu",
            "render_game_vertices_peak=%llu",
        ):
            self.assertIn(field, MEMORY)

    def test_frontend_appends_once_and_skips_manager_28b_staging(self) -> None:
        add = body(ANM, "ZunResult AnmManager::AddSpriteToDrawBuffer(")
        product = add[
            add.index("if (g_PspBulletPackedVertexFastpathActive") :
            add.index("#if TH08_PSP_ANY_MIXED_QUADS_ENABLED")
        ]
        self.assertIn("th08_psp_bullet_packed_vertex_append(", product)
        self.assertIn("++this->spritesToDraw", product)
        self.assertNotIn("vertexBufferEndPtr[", product)
        self.assertNotIn("BulletManager", product)
        self.assertNotIn("Item", product)

    def test_append_models_flush_arg2_without_extra_state_requests(self) -> None:
        begin = body(ANM, "void AnmManager::BeginPspBulletUnifiedQuadBatch()")
        gate = begin[
            begin.index("Append's pure color evaluator") :
            begin.index("if (!PspBulletUnifiedQuadBufferCanAppend")
        ]
        self.assertNotIn("SetTextureStageState", gate)
        append = body(D3D, "bool AppendPspBulletPackedVertexQuad(")
        self.assertIn("EffectiveColor(rawDiffuse[0], true)", append)
        self.assertIn("EffectiveColor(rawDiffuse[corner], true)", append)
        effective = body(D3D, "D3DCOLOR EffectiveColor(")
        self.assertIn("forceDiffuseArg2", effective)
        self.assertIn("? D3DTA_DIFFUSE", effective)

    def test_exact_24b_visual_contract_and_uniform_color_gate(self) -> None:
        pack = body(D3D, "static void PackPspBulletProductVertex(")
        for token in (
            "destination->u = uv[0]",
            "destination->v = uv[1]",
            "effectiveColor >> 16",
            "effectiveColor >> 8",
            "effectiveColor & 255U",
            "effectiveColor >> 24",
            "position[0] + 0.5f",
            "position[1] + 0.5f",
            "1.0f - 2.0f * position[2]",
            "screen shake",
            "nearbyintf axis rounding",
            "flipped UV",
        ):
            self.assertIn(token, pack)
        append = body(D3D, "bool AppendPspBulletPackedVertexQuad(")
        self.assertLess(
            append.index("const bool uniformDiffuse"),
            append.index("EffectiveColor(rawDiffuse[0], true)"),
        )
        self.assertIn("sizeof(PspClientVertex) == 24U", D3D)

    def test_full_arena_preflight_makes_midrun_capacity_impossible(self) -> None:
        begin = body(D3D, "bool BeginPspBulletPackedVertexBatch()")
        self.assertIn("pspBulletDirectGeVertexCursor != 0U", begin)
        self.assertIn("kPspBulletDirectGeMaxQuads * 4U", begin)
        self.assertIn("complete 6144-vertex arena", begin)
        self.assertIn("0x600 logical slots", begin)

    def test_same_flush_same_4v_indices_and_no_normal_extra_draw(self) -> None:
        flush = body(ANM, "void AnmManager::FlushVertexBuffer()")
        product = flush[
            flush.index("if (g_PspBulletPackedVertexFastpathActive") :
            flush.index("#if TH08_PSP_ANY_MIXED_QUADS_PRODUCT_ENABLED")
        ]
        self.assertEqual(product.count("th08_psp_bullet_packed_vertex_submit("), 1)
        self.assertIn("g_PspBulletUnifiedQuadIndices", product)
        self.assertIn("this->spritesToDraw * 6U", product)
        self.assertNotIn("DrawPrimitive", product)
        self.assertNotIn("DrawIndexedPrimitive", product)
        self.assertNotIn("BeginPspBullet", product)

    def test_native_reject_draws_same_packed_bytes_once_and_cache_is_incremental(self) -> None:
        submit = body(D3D, "bool SubmitPspBulletPackedVertexRun(")
        self.assertIn("pspBulletDirectGeValidatedIndexCount", submit)
        self.assertIn("pspBulletDirectGeIndexAuthority", submit)
        self.assertIn("ordinal = pspBulletDirectGeValidatedIndexCount", submit)
        native = submit.index("__pspgl_th08_draw_native_indexed_triangles(")
        fallback = submit.index("glDrawElements(", native)
        self.assertIn("vertices", submit[native : native + 500])
        self.assertIn("&vertices[0].x", submit[fallback - 600 : fallback])
        self.assertIn("RenderPerfNoteStateEmitted(9U)", submit)
        self.assertIn("glDrawArrays(GL_TRIANGLE_STRIP", submit)

    def test_unexpected_midrun_append_preserves_prefix_then_canonical_suffix(self) -> None:
        add = body(ANM, "ZunResult AnmManager::AddSpriteToDrawBuffer(")
        recovery = add[
            add.index("const u32 packedQuadsBeforeFallback") :
            add.index("#if TH08_PSP_ANY_MIXED_QUADS_ENABLED")
        ]
        self.assertLess(
            recovery.index("this->FlushVertexBuffer()"),
            recovery.index("th08_psp_bullet_packed_vertex_end("),
        )
        self.assertIn("note_recovery_split", recovery)
        self.assertIn("split_to_canonical", recovery)
        self.assertNotIn("return ZUN_ERROR", recovery)
        self.assertNotIn("this->spritesToDraw = 0", recovery)

    def test_cumulative_telemetry_and_acceptance_algebra_are_explicit(self) -> None:
        self.assertIn("PspBulletPackedVertexFastpathStats", INTERNAL)
        self.assertIn("th08_psp_query_bullet_packed_vertex_fastpath_stats", INTERNAL)
        self.assertIn("BULLET_PACKED_VERTEX_FASTPATH_TELEMETRY kind=%s", MEMORY)
        for field in (
            "appended_quads=%lu",
            "packed_vertices=%lu",
            "submitted_runs=%lu",
            "native_submits=%lu",
            "client_fallback_submits=%lu",
            "owner_fallbacks=%lu",
            "state_fallbacks=%lu",
            "index_fallbacks=%lu",
            "capacity_fallbacks=%lu",
            "contract_fallbacks=%lu",
            "abandoned_quads=%lu",
            "recovery_split_runs=%lu",
            "recovery_split_quads=%lu",
            "frontend_28b_bytes_avoided=%lu",
        ):
            self.assertIn(field, MEMORY)
        for algebra in (
            "algebra_packed_vertices=4x_appended_quads",
            "algebra_appended_quads=submitted_quads_plus_abandoned_quads",
            "algebra_submitted_runs=native_plus_client_only_when_split_zero",
            "accept_requires_append_attempts_equal_appended_quads=1",
            "accept_requires_all_fallback_abandoned_split_per_vertex_zero=1",
        ):
            self.assertIn(algebra, MEMORY)


class PackedVertexFastpathDifferentialTests(unittest.TestCase):
    def test_random_final_stream_matches_m0_canonical_arithmetic(self) -> None:
        rng = random.Random(0x08_54_01)
        args = (1, 2, 3)
        for _ in range(20_000):
            factor = rng.getrandbits(32)
            color_op = rng.choice((2, 4))
            alpha_op = rng.choice((2, 4))
            color_arg1 = rng.choice(args)
            alpha_arg1 = rng.choice(args)
            common = rng.getrandbits(32)
            uniform = rng.randrange(8) != 0
            sources: list[bytes] = []
            raw_colors: list[int] = []
            for _corner in range(4):
                x = f32(rng.uniform(-1024.0, 1536.0))
                y = f32(rng.uniform(-1024.0, 1536.0))
                z = f32(rng.uniform(-4.0, 4.0))
                u = f32(rng.uniform(-2.0, 3.0))
                v = f32(rng.uniform(-2.0, 3.0))
                raw = common if uniform else rng.getrandbits(32)
                sources.append(
                    struct.pack("<ffffIff", x, y, z, 1.0, raw, u, v)
                )
                raw_colors.append(raw)

            if uniform:
                common_effective = effective_color_forced_diffuse_arg2(
                    raw_colors[0], factor, color_op, color_arg1,
                    alpha_op, alpha_arg1
                )
                product_colors = [common_effective] * 4
            else:
                product_colors = [
                    effective_color_forced_diffuse_arg2(
                        raw, factor, color_op, color_arg1,
                        alpha_op, alpha_arg1
                    )
                    for raw in raw_colors
                ]
            product_stream = [
                pack_product(source, color)
                for source, color in zip(sources, product_colors)
            ]

            # Independent M0/canonical oracle: evaluate the ordinary backend
            # with the Flush-time ARG2=DIFFUSE operands, then use its separate
            # final pack routine for every corner.
            canonical_stream = []
            for source, raw in zip(sources, raw_colors):
                canonical_color = canonical_effective_color(
                    raw, factor, color_op, color_arg1, 3,
                    alpha_op, alpha_arg1, 3
                )
                canonical_stream.append(
                    canonical_pack_final(source, canonical_color)
                )
            self.assertTrue(all(len(vertex) == 24 for vertex in product_stream))
            self.assertEqual(product_stream, canonical_stream)


if __name__ == "__main__":
    unittest.main()
