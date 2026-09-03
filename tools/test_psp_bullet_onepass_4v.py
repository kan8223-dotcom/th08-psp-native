#!/usr/bin/env python3
"""Host geometry and source-contract gates for Bullet one-pass 4V."""

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


class PspBulletOnePass4VTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.makefile = read("Makefile.psp")
        cls.header = read("src/AnmManager.hpp")
        cls.anm = read("src/AnmManager.cpp")
        cls.bullet = read("src/BulletManager.cpp")
        cls.main = read("psp/main.cpp")
        cls.geometry = read("psp/bullet_onepass_4v.hpp")
        cls.audit = function_body(
            cls.anm, "ZunResult AnmManager::DrawPspBulletOnePass4VAudit"
        )
        cls.product = function_body(
            cls.anm, "bool AnmManager::TryDrawPspBulletOnePass4V"
        )
        cls.draw = function_body(cls.bullet, "ZunResult Bullet::DrawSingleBullet")

    def test_actual_geometry_header_matches_independent_host_model(self) -> None:
        with tempfile.TemporaryDirectory(prefix="th08-bullet-onepass-4v-") as temp:
            binary = pathlib.Path(temp) / "bullet-onepass-4v"
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
                    str(ROOT / "tools" / "psp_bullet_onepass_4v_harness.cpp"),
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
            self.assertIn("bullet-onepass-4v: PASS samples=250032", result.stdout)

    def test_modes_are_default_off_stamped_isolated_and_fingerprinted(self) -> None:
        self.assertIn("TH08_PSP_BULLET_ONEPASS_4V_AUDIT ?= 0", self.makefile)
        self.assertIn("TH08_PSP_BULLET_ONEPASS_4V_FASTPATH ?= 0", self.makefile)
        self.assertIn("-DTH08_PSP_BULLET_ONEPASS_4V_AUDIT=1", self.makefile)
        self.assertIn("-DTH08_PSP_BULLET_ONEPASS_4V_FASTPATH=1", self.makefile)
        self.assertIn(
            "bullet-onepass-4v-audit-$(TH08_PSP_BULLET_ONEPASS_4V_AUDIT).stamp",
            self.makefile,
        )
        self.assertIn(
            "bullet-onepass-4v-fastpath-$(TH08_PSP_BULLET_ONEPASS_4V_FASTPATH).stamp",
            self.makefile,
        )
        self.assertIn(
            "TH08_PSP_BULLET_ONEPASS_4V_AUDIT and TH08_PSP_BULLET_ONEPASS_4V_FASTPATH are mutually exclusive",
            self.makefile,
        )
        self.assertIn("requires TH08_PSP_BULLET_FASTPATH=1", self.makefile)
        self.assertIn("requires TH08_PSP_BULLET_UNIFIED_QUADS=1", self.makefile)
        self.assertIn("requires TH08_PSP_BULLET_DIRECT_GE=1", self.makefile)
        self.assertIn("isolated from packed/mixed Bullet experiments", self.makefile)
        self.assertIn("BULLET_ONEPASS_4V_AUDIT=%d", self.main)
        self.assertIn("BULLET_ONEPASS_4V_FASTPATH=%d", self.main)
        self.assertIn("test-psp-bullet-onepass-4v", self.makefile)

    def test_m0_keeps_canonical_draw_authoritative_and_compares_all_bytes(self) -> None:
        build = self.audit.index("BuildPspBulletOnePass4V(candidate")
        canonical = self.audit.index("this->Draw2DWithPrecomputedRotation", build)
        compare = self.audit.index(
            "memcmp(candidate, g_QuadVertices, sizeof(candidate))"
        )
        self.assertLess(build, canonical)
        self.assertLess(canonical, compare)
        self.assertIn("memcmp(endBefore, candidate, sizeof(candidate))", self.audit)
        self.assertIn("u8 vmBefore[sizeof(AnmVm)]", self.audit)
        self.assertIn("memcmp(vmBefore, vm, sizeof(vmBefore))", self.audit)
        self.assertIn("renderStateChangesBefore + (visible ? 1U : 0U)", self.audit)
        self.assertIn("this->flushesThisFrame == flushesBefore", self.audit)
        self.assertNotIn("AddSpriteToDrawBuffer", self.audit)
        self.assertNotIn("DrawIndexedPrimitive", self.audit)
        self.assertIn("quadMismatches", self.audit)
        self.assertIn("bufferMismatches", self.audit)
        self.assertIn("vmMismatches", self.audit)
        self.assertIn("stateMismatches", self.audit)

    def test_product_fails_closed_and_uses_existing_4v_stream(self) -> None:
        proof = self.product.index("PspBulletOnePass4VProductCapacityReady")
        first_write = self.product.index("BuildPspBulletOnePass4V(g_QuadVertices")
        self.assertLess(proof, first_write)
        self.assertIn("return false;", self.product[:first_write])
        finite = self.product.index("PspBulletOnePass4VQuadFinite", first_write)
        append = self.product.index("this->vertexBufferEndPtr[0]", finite)
        self.assertLess(first_write, finite)
        self.assertLess(finite, append)
        self.assertIn("this->vertexBufferEndPtr[0] = g_QuadVertices[0]", self.product)
        self.assertIn("this->vertexBufferEndPtr[3] = g_QuadVertices[3]", self.product)
        self.assertIn("this->vertexBufferEndPtr += 4", self.product)
        self.assertIn("++this->spritesToDraw", self.product)
        self.assertIn("++this->renderStateChangesThisFrame", self.product)
        for forbidden in (
            "FlushVertexBuffer",
            "SetRenderStateForVm",
            "SetTexture(",
            "DrawPrimitive",
            "DrawIndexedPrimitive",
            "Random",
            "Rng",
        ):
            self.assertNotIn(forbidden, self.product)

        attempt = self.draw.index("TryDrawPspBulletOnePass4V")
        fallback = self.draw.index("Draw2DWithPrecomputedRotation", attempt)
        self.assertLess(attempt, fallback)
        self.assertIn("this->state == BULLET_STATE_FIRED", self.draw[:attempt])

    def test_vm_mutations_remain_before_audit_product_and_fallback(self) -> None:
        pos = self.draw.index("vm->pos.operator float *()[0]")
        white = self.draw.index("vm->color1.d3dColor")
        rotation = self.draw.index("vm->SetZRotation(renderAngle)")
        audit = self.draw.index("DrawPspBulletOnePass4VAudit")
        product = self.draw.index("TryDrawPspBulletOnePass4V")
        fallback = self.draw.index("Draw2DWithPrecomputedRotation", product)
        self.assertLess(pos, white)
        self.assertLess(white, rotation)
        self.assertLess(rotation, audit)
        self.assertLess(rotation, product)
        self.assertLess(product, fallback)
        for forbidden in ("g_Rng", "GetRandom", "AdvanceFrame", "Update"):
            self.assertNotIn(forbidden, self.product)

    def test_geometry_preserves_persistent_fields_and_exact_cull_order(self) -> None:
        self.assertNotIn(".w =", self.geometry)
        self.assertNotIn(".diffuse =", self.geometry)
        self.assertIn("BulletOnePassTranslateRotation(&quad[0], -x, -y", self.geometry)
        self.assertIn("quad[0].pos.x += shakeX", self.geometry)
        self.assertIn("vm.loadedSprite->uvStart.x + vm.uvScrollPos.x", self.geometry)
        self.assertIn("maxX = BulletOnePassMax(quad[2].pos.x, maxX)", self.geometry)
        self.assertIn("minY = BulletOnePassMin(quad[3].pos.y, minY)", self.geometry)
        self.assertIn("minX > viewportRight", self.geometry)
        self.assertIn("minY > viewportBottom", self.geometry)
        self.assertIn("BulletOnePassFiniteCarry(quad[3].pos.y)", self.anm)
        self.assertIn("BulletOnePassFiniteCarry(quad[2].textureUV.y)", self.anm)
        self.assertIn("(bits & 0x7fffffffU) + 0x00800000U", self.geometry)
        self.assertIn("(carry & 0x80000000U) == 0U", self.anm)
        self.assertIn("g_Supervisor.cfg.opts.disableDepthTest", self.anm)

    def test_stats_reservation_is_fixed_and_outside_gameplay_objects(self) -> None:
        self.assertIn("PspBulletOnePass4VStats g_PspBulletOnePass4VStats", self.anm)
        self.assertIn("__attribute__((used))", self.anm)
        self.assertIn("sizeof(PspBulletOnePass4VStats) == 80U", self.header)
        manager_storage = self.header[
            self.header.index("    ZunColor color;", self.header.index("struct AnmManager")) :
            self.header.index("C_ASSERT(sizeof(AnmManager)")
        ]
        self.assertNotIn("PspBulletOnePass4VStats", manager_storage)
        # Product deliberately has no hot telemetry write; M0 owns all counters.
        self.assertNotIn("g_PspBulletOnePass4VStats", self.product)


if __name__ == "__main__":
    unittest.main()
