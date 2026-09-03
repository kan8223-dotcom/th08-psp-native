#!/usr/bin/env python3
"""Host-only contracts for owner-specific Bullet/Item mixed-GE accounting."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


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
                return source[start : index + 1]
    raise AssertionError(f"unterminated function: {signature}")


class MixedGeBackendStatsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.internal = read("src/modern/linux/d3d8_internal.hpp")
        cls.backend = read("src/modern/linux/d3d8_compat.cpp")
        cls.telemetry = read("psp/memory_telemetry.cpp")
        cls.draw = body(cls.backend, "bool DrawPspBulletMixedQuads(")
        cls.query = body(cls.backend, "bool QueryPspMixedGeBackendStats(")
        cls.begin_scene = body(cls.backend, "HRESULT BeginScene()")

    def test_public_snapshot_has_separate_owner_and_arena_fields(self) -> None:
        for token in (
            "bulletAttempts",
            "bulletSubmittedBatches",
            "bulletSubmittedQuads",
            "bulletFallbacks",
            "bulletArenaExhaustions",
            "itemAttempts",
            "itemSubmittedBatches",
            "itemSubmittedQuads",
            "itemFallbacks",
            "itemArenaExhaustions",
            "sharedArenaHighWaterVertices",
            "sharedArenaCapacityVertices",
            "th08_psp_query_mixed_ge_backend_stats",
        ):
            self.assertIn(token, self.internal)

    def test_query_is_read_only_and_reports_the_shared_capacity(self) -> None:
        for token in (
            "stats->bulletAttempts = pspBulletMixedGeAttempts",
            "stats->itemAttempts = pspItemMixedGeAttempts",
            "stats->sharedArenaHighWaterVertices",
            "stats->sharedArenaCapacityVertices",
            "kPspBulletDirectGeVertexCapacity",
        ):
            self.assertIn(token, self.query)
        for forbidden in ("++", "--", "memset", "PrepareState", "Present("):
            self.assertNotIn(forbidden, self.query)

    def test_attempt_reject_and_success_are_routed_by_owner(self) -> None:
        attempt_if = self.draw.index("if (itemOwner)")
        reject = self.draw.index("const auto reject", attempt_if)
        capacity = self.draw.index("pspBulletDirectGeVertexCursor >", reject)
        success = self.draw.index("if (itemOwner)", capacity)

        self.assertIn("++pspItemMixedGeAttempts", self.draw[attempt_if:reject])
        self.assertIn("++pspBulletMixedGeAttempts", self.draw[attempt_if:reject])
        reject_body = self.draw[reject:capacity]
        self.assertIn("++pspItemMixedGeFallbacks", reject_body)
        self.assertIn("++pspBulletMixedGeFallbacks", reject_body)
        self.assertIn("++pspBulletDirectGeFallbacks", reject_body)
        self.assertNotIn("++pspBulletDirectGeFallbacks", reject_body.split("else", 1)[0])

        success_body = self.draw[success:]
        self.assertIn("++pspItemMixedGeSubmittedBatches", success_body)
        self.assertIn("pspItemMixedGeSubmittedQuads +=", success_body)
        self.assertIn("++pspBulletMixedGeSubmittedBatches", success_body)
        self.assertIn("pspBulletMixedGeSubmittedQuads +=", success_body)

    def test_arena_exhaustion_is_owner_specific_and_fails_before_submit(self) -> None:
        capacity = self.draw.index("pspBulletDirectGeVertexCursor >")
        submit = self.draw.index("__pspgl_th08_draw_native_mixed_quads", capacity)
        guarded = self.draw[capacity:submit]
        self.assertIn("++pspItemMixedGeArenaExhaustions", guarded)
        self.assertIn("++pspBulletMixedGeArenaExhaustions", guarded)
        self.assertIn("return reject();", guarded)

    def test_shared_arena_recycles_only_across_present_fence(self) -> None:
        reset = self.begin_scene.index("pspBulletDirectGeVertexCursor = 0U")
        self.assertIn(
            "pspBulletDirectGeArenaPresent != presentCount",
            self.begin_scene[:reset],
        )
        self.assertIn(
            "a repeated\n        // BeginScene without Present must keep the append-only arena intact",
            self.begin_scene,
        )

    def test_logs_expose_both_owners_and_zero_fallback_acceptance_gate(self) -> None:
        for token in (
            "MIXED_GE_BACKEND bullet_attempts=%lu",
            "item_attempts=%lu",
            "item_fallbacks=%lu",
            "item_arena_exhaustions=%lu",
            "MIXED_GE_BACKEND_TELEMETRY kind=%s",
            "owner_counters=separate",
            "accept_requires_owner_fallbacks_and_arena_exhaustions_zero=1",
        ):
            self.assertIn(token, self.backend + self.telemetry)

    def test_item_savings_use_six_vertex_canonical_authority(self) -> None:
        self.assertIn(
            "potential_frontend_saved=pair_x4_plus_general_x2",
            self.telemetry,
        )
        self.assertIn("potential_ge_saved=pair_x4", self.telemetry)
        self.assertIn(
            "itemMixedQuads.submittedGeneralQuads) * 2ULL",
            self.telemetry,
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
