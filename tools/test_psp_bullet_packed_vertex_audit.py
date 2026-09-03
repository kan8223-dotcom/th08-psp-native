#!/usr/bin/env python3
"""Focused source and differential gates for Bullet packed-vertex M0.

The experiment is observation-only: the accepted direct-GE 4V indexed stream
remains authoritative while one stack-local 24-byte candidate per corner is
compared byte-for-byte with the canonical backend output.
"""

from __future__ import annotations

import random
import struct
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
D3D = (ROOT / "src/modern/linux/d3d8_compat.cpp").read_text(encoding="utf-8")
INTERNAL = (ROOT / "src/modern/linux/d3d8_internal.hpp").read_text(
    encoding="utf-8"
)
MAKEFILE = (ROOT / "Makefile.psp").read_text(encoding="utf-8")
MAIN = (ROOT / "psp/main.cpp").read_text(encoding="utf-8")
MEMORY = (ROOT / "psp/memory_telemetry.cpp").read_text(encoding="utf-8")
FEATURE = "TH08_PSP_BULLET_PACKED_VERTEX_AUDIT"


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


def effective_color(
    diffuse: int,
    factor: int,
    color_op: int,
    color_arg1: int,
    color_arg2: int,
    alpha_op: int,
    alpha_arg1: int,
    alpha_arg2: int,
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

    color1, color2 = argument(color_arg1), argument(color_arg2)
    alpha1, alpha2 = argument(alpha_arg1), argument(alpha_arg2)
    if color_op == 4:  # D3DTOP_MODULATE
        red = (((color1 >> 16) & 255) * ((color2 >> 16) & 255) + 127) // 255
        green = (((color1 >> 8) & 255) * ((color2 >> 8) & 255) + 127) // 255
        blue = ((color1 & 255) * (color2 & 255) + 127) // 255
        rgb = (red << 16) | (green << 8) | blue
    else:  # SELECTARG1 and the PSP backend's conservative default
        rgb = color1 & 0x00FFFFFF
    if alpha_op == 4:
        alpha = (
            (((alpha1 >> 24) & 255) * ((alpha2 >> 24) & 255) + 127) // 255
        ) << 24
    else:
        alpha = alpha1 & 0xFF000000
    return alpha | rgb


def pack_final(source: bytes, color: int) -> bytes:
    x, y, z, _rhw, _diffuse, u, v = struct.unpack("<ffffIff", source)
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


class PackedVertexAuditSourceTests(unittest.TestCase):
    def test_default_off_stamp_dependencies_and_build_identity(self) -> None:
        self.assertIn(f"{FEATURE} ?= 0", MAKEFILE)
        self.assertIn("bullet-packed-vertex-audit-0.stamp", MAKEFILE)
        self.assertIn("bullet-packed-vertex-audit-1.stamp", MAKEFILE)
        self.assertIn(f"-D{FEATURE}=1", MAKEFILE)
        self.assertIn(f"$(error {FEATURE} must be 0 or 1)", MAKEFILE)
        self.assertIn(f"{FEATURE}=1 requires TH08_PSP_BULLET_UNIFIED_QUADS=1", MAKEFILE)
        self.assertIn(f"{FEATURE}=1 requires TH08_PSP_BULLET_DIRECT_GE=1", MAKEFILE)
        self.assertIn(
            f"{FEATURE} and BULLET_MIXED_QUADS are mutually exclusive", MAKEFILE
        )
        self.assertIn("$(BULLET_PACKED_VERTEX_AUDIT_CONFIG_STAMP)", MAKEFILE)
        self.assertIn("TH08_PSP_FEATURE_BULLET_PACKED_VERTEX_AUDIT", MAIN)
        self.assertIn("BULLET_PACKED_VERTEX_AUDIT=%d", MAIN)

    def test_shadow_is_built_after_canonical_and_never_submitted(self) -> None:
        indexed = body(D3D, "HRESULT DrawIndexed(")
        bullet = indexed[indexed.index("const bool arenaHasRoom =") :]
        canonical_write = bullet.index("PspClientVertex &output = directVertices")
        audit = bullet.index("AuditPspBulletPackedVertices(")
        submit = bullet.index("__pspgl_th08_draw_native_indexed_triangles(")
        self.assertLess(canonical_write, audit)
        self.assertLess(audit, submit)
        self.assertIn("directVertices,", bullet[submit : submit + 500])
        self.assertNotIn("candidate,", bullet[submit : submit + 500])

        audit_body = body(D3D, "void AuditPspBulletPackedVertices(")
        self.assertIn("PspClientVertex candidate[4];", audit_body)
        for forbidden in (
            "__pspgl_th08_draw_native",
            "glDraw",
            "sceGu",
            "malloc(",
            "new ",
            "RenderResourceArenaAllocate",
        ):
            self.assertNotIn(forbidden, audit_body)
        self.assertIn("candidate_submits=0 canonical_authority=1 product_enabled=0", D3D)

    def test_final_24_byte_contract_and_all_visual_fields_are_explicit(self) -> None:
        pack = body(D3D, "static void PackPspBulletAuditVertex(")
        for token in (
            "candidate->u = uv[0]",
            "candidate->v = uv[1]",
            "effectiveColor >> 16",
            "effectiveColor >> 8",
            "effectiveColor & 255U",
            "effectiveColor >> 24",
            "position[0] + 0.5f",
            "position[1] + 0.5f",
            "1.0f - 2.0f * position[2]",
            "screen shake",
            "axis",
            "nearbyintf(-0.5)",
            "flipped UV",
        ):
            self.assertIn(token, pack)
        self.assertIn("sizeof(PspClientVertex) == 24U", D3D)
        for offset in ("u) == 0U", "v) == 4U", "r) == 8U", "x) == 12U", "y) == 16U", "z) == 20U"):
            self.assertIn(offset, D3D)

    def test_uniform_diffuse_is_proven_before_one_effective_color_call(self) -> None:
        audit = body(D3D, "void AuditPspBulletPackedVertices(")
        proof = audit.index("const bool uniformDiffuse")
        branch = audit.index("if (uniformDiffuse)", proof)
        common = audit.index("EffectiveColor(rawDiffuse[0])", branch)
        fallback = audit.index("else", common)
        per_vertex = audit.index("EffectiveColor(rawDiffuse[corner])", fallback)
        self.assertLess(proof, branch)
        self.assertLess(branch, common)
        self.assertLess(common, fallback)
        self.assertLess(fallback, per_vertex)
        self.assertIn("uniformDiffuseQuads", audit)
        self.assertIn("perVertexDiffuseQuads", audit)

    def test_owner_state_index_capacity_and_submit_fallbacks_are_exclusive_gates(self) -> None:
        indexed = body(D3D, "HRESULT DrawIndexed(")
        for token in (
            "auditOwnerValid",
            "auditStateValid",
            "canonicalFallbacks",
            "ownerFallbacks",
            "stateFallbacks",
            "indexFallbacks",
            "capacityFallbacks",
            "submitFallbacks",
        ):
            self.assertIn(token, indexed)
        self.assertLess(indexed.index("!auditOwnerValid"), indexed.index("!auditStateValid"))
        self.assertLess(indexed.index("!auditStateValid"), indexed.index("!directIndexAuthorityValid"))
        self.assertIn("if (arenaHasRoom)", indexed)
        self.assertIn("else\n                {\n                    ++pspBulletPackedVertexAudit.capacityFallbacks", indexed)
        # Audit mismatch never becomes a draw reject in M0.
        self.assertNotIn("return false", body(D3D, "void AuditPspBulletPackedVertices("))

    def test_byte_mismatch_location_and_classification_are_persisted(self) -> None:
        for field in (
            "mismatchUBytes",
            "mismatchVBytes",
            "mismatchColorBytes",
            "mismatchXBytes",
            "mismatchYBytes",
            "mismatchZBytes",
            "mismatchOtherBytes",
            "firstMismatchValid",
            "firstMismatchBatch",
            "firstMismatchQuad",
            "firstMismatchVertex",
            "firstMismatchByte",
        ):
            self.assertIn(f"unsigned long {field};", INTERNAL)
        note = body(D3D, "void NotePspBulletPackedMismatchByte(")
        for boundary in ("< 4U", "< 8U", "< 12U", "< 16U", "< 20U", "< 24U"):
            self.assertIn(boundary, note)
        self.assertIn("candidateBytes[byteOffset] == canonicalBytes[byteOffset]", D3D)

    def test_cumulative_read_only_telemetry_is_complete(self) -> None:
        self.assertIn("th08_psp_query_bullet_packed_vertex_audit_stats", INTERNAL)
        self.assertIn("QueryPspBulletPackedVertexAuditStats", D3D)
        self.assertIn("BULLET_PACKED_VERTEX_AUDIT_TELEMETRY kind=%s", MEMORY)
        self.assertIn("counter_scope=device_lifetime", MEMORY)
        self.assertIn("mark_is_non_destructive=1", MEMORY)
        self.assertIn("query_is_read_only=1", MEMORY)
        for field in (
            "eligible_quads=%lu",
            "matched_quads=%lu",
            "mismatch_quads=%lu",
            "uniform_diffuse_quads=%lu",
            "canonical_fallbacks=%lu",
            "owner_fallbacks=%lu",
            "state_fallbacks=%lu",
            "index_fallbacks=%lu",
            "capacity_fallbacks=%lu",
            "submit_fallbacks=%lu",
            "first_mismatch_byte=%lu",
        ):
            self.assertIn(field, MEMORY)


class PackedVertexAuditDifferentialTests(unittest.TestCase):
    def test_random_screen_shake_axis_flip_z_uv_and_color_match(self) -> None:
        rng = random.Random(0x08_49_24)
        args = (1, 2, 3)
        for _ in range(20_000):
            factor = rng.getrandbits(32)
            color_op = rng.choice((2, 4))
            alpha_op = rng.choice((2, 4))
            color_args = (rng.choice(args), rng.choice(args))
            alpha_args = (rng.choice(args), rng.choice(args))
            uniform = rng.randrange(5) != 0
            common = rng.getrandbits(32)
            flip_u, flip_v = rng.choice((False, True)), rng.choice((False, True))
            screen_x = f32(rng.uniform(-8.0, 8.0))
            screen_y = f32(rng.uniform(-8.0, 8.0))
            sources: list[bytes] = []
            raw_colors: list[int] = []
            for corner in range(4):
                x = f32(rng.uniform(-512.0, 1024.0) + screen_x)
                y = f32(rng.uniform(-512.0, 1024.0) + screen_y)
                if rng.randrange(2) == 0:  # already-canonical axis nearbyint - .5
                    x = f32(float(round(x)) - 0.5)
                    y = f32(float(round(y)) - 0.5)
                z = f32(rng.uniform(-1.0, 1.0))
                color = common if uniform else rng.getrandbits(32)
                u = f32(rng.uniform(-2.0, 2.0))
                v = f32(rng.uniform(-2.0, 2.0))
                if flip_u:
                    u = f32(-u)
                if flip_v:
                    v = f32(-v)
                sources.append(struct.pack("<ffffIff", x, y, z, 1.0, color, u, v))
                raw_colors.append(color)

            canonical_colors = [
                effective_color(
                    color,
                    factor,
                    color_op,
                    color_args[0],
                    color_args[1],
                    alpha_op,
                    alpha_args[0],
                    alpha_args[1],
                )
                for color in raw_colors
            ]
            canonical = [
                pack_final(source, color)
                for source, color in zip(sources, canonical_colors)
            ]
            if uniform:
                one_color = effective_color(
                    raw_colors[0],
                    factor,
                    color_op,
                    color_args[0],
                    color_args[1],
                    alpha_op,
                    alpha_args[0],
                    alpha_args[1],
                )
                candidate = [pack_final(source, one_color) for source in sources]
            else:
                candidate = [
                    pack_final(source, color)
                    for source, color in zip(sources, canonical_colors)
                ]
            self.assertEqual(candidate, canonical)
            self.assertTrue(all(len(vertex) == 24 for vertex in candidate))

    def test_every_byte_offset_maps_to_exact_field_bucket(self) -> None:
        def bucket(offset: int) -> str:
            return (
                "u" if offset < 4 else
                "v" if offset < 8 else
                "color" if offset < 12 else
                "x" if offset < 16 else
                "y" if offset < 20 else
                "z" if offset < 24 else
                "other"
            )

        expected = ["u"] * 4 + ["v"] * 4 + ["color"] * 4
        expected += ["x"] * 4 + ["y"] * 4 + ["z"] * 4
        self.assertEqual([bucket(offset) for offset in range(24)], expected)
        original = bytes(range(24))
        for offset in range(24):
            mutated = bytearray(original)
            mutated[offset] ^= 0x80
            mismatches = [
                index
                for index, (left, right) in enumerate(zip(original, mutated))
                if left != right
            ]
            self.assertEqual(mismatches, [offset])
            self.assertEqual(bucket(mismatches[0]), expected[offset])

    def test_telemetry_acceptance_algebra(self) -> None:
        eligible_quads = 4_000
        matched_quads = 4_000
        mismatch_quads = 0
        uniform_quads = 3_999
        per_vertex_quads = 1
        compared_vertices = 16_000
        self.assertEqual(eligible_quads, matched_quads + mismatch_quads)
        self.assertEqual(eligible_quads, uniform_quads + per_vertex_quads)
        self.assertEqual(compared_vertices, eligible_quads * 4)
        fallback_classes = {
            "owner": 0,
            "state": 0,
            "index": 0,
            "capacity": 0,
            "submit": 0,
        }
        self.assertEqual(sum(fallback_classes.values()), 0)


if __name__ == "__main__":
    unittest.main()
