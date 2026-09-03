#!/usr/bin/env python3
"""Host differential/static gates for the PSP Bullet collision audit.

The PSP audit is observation-only.  This suite proves the standalone negative
predicate and binds it to the canonical single-call sites without requiring a
PSP executable or emulator run.
"""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
BULLET_CPP = (ROOT / "src" / "BulletManager.cpp").read_text(encoding="utf-8")
BULLET_HPP = (ROOT / "src" / "BulletManager.hpp").read_text(encoding="utf-8")
PLAYER_CPP = (ROOT / "src" / "Player.cpp").read_text(encoding="utf-8")
PLAYER_HPP = (ROOT / "src" / "Player.hpp").read_text(encoding="utf-8")
GATE_HPP = (ROOT / "src" / "PspBulletCollisionGate.hpp").read_text(
    encoding="utf-8"
)
MAKEFILE = (ROOT / "Makefile.psp").read_text(encoding="utf-8")
MAIN_CPP = (ROOT / "psp" / "main.cpp").read_text(encoding="utf-8")
MEMORY_TELEMETRY_CPP = (
    ROOT / "psp" / "memory_telemetry.cpp"
).read_text(encoding="utf-8")
HARNESS = ROOT / "tools" / "psp_bullet_collision_gate_harness.cpp"
FEATURE = "TH08_PSP_BULLET_COLLISION_GATE_AUDIT"


def function_body(source: str, signature: str) -> str:
    start = source.find(signature)
    if start < 0:
        raise AssertionError(f"missing function: {signature}")
    opening = source.find("{", start)
    if opening < 0:
        raise AssertionError(f"missing body: {signature}")
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening : index + 1]
    raise AssertionError(f"unterminated body: {signature}")


class BulletCollisionGateAuditTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        compiler = None
        for candidate in (os.environ.get("CXX"), "g++", "clang++"):
            if candidate and shutil.which(candidate):
                compiler = candidate
                break
        if compiler is None:
            raise unittest.SkipTest("no host C++17 compiler")

        cls.temporary = tempfile.TemporaryDirectory(
            prefix="th08-bullet-collision-gate-"
        )
        cls.executable = Path(cls.temporary.name) / "collision_gate_harness"
        result = subprocess.run(
            [
                compiler,
                "-std=c++17",
                "-O2",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(ROOT),
                str(HARNESS),
                "-o",
                str(cls.executable),
            ],
            text=True,
            capture_output=True,
            check=False,
        )
        if result.returncode:
            raise AssertionError(
                "host harness compilation failed:\n"
                + result.stdout
                + result.stderr
            )

    @classmethod
    def tearDownClass(cls) -> None:
        if hasattr(cls, "temporary"):
            cls.temporary.cleanup()

    def test_host_differential_implication(self) -> None:
        result = subprocess.run(
            [str(self.executable)],
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(
            result.returncode,
            0,
            msg=result.stdout + result.stderr,
        )
        self.assertIn("PASS directed=13 random=200000", result.stdout)

    def test_feature_is_psp_only_value_gated_and_requires_sidecar(self) -> None:
        gate = (
            f"defined(PSP) && defined({FEATURE}) && \\\n    {FEATURE}"
        )
        self.assertIn(gate, BULLET_CPP)
        self.assertIn(gate, PLAYER_CPP)
        self.assertIn("requires TH08_PSP_PLAYER_SCAN_SIDECAR", BULLET_CPP)
        self.assertIn("requires TH08_PSP_PLAYER_SCAN_SIDECAR", PLAYER_CPP)
        self.assertIn(f"{FEATURE} ?= 0", MAKEFILE)
        self.assertIn(f"-D{FEATURE}=1", MAKEFILE)
        self.assertIn(
            f"{FEATURE}=1 requires TH08_PSP_PLAYER_SCAN_SIDECAR=1",
            MAKEFILE,
        )
        self.assertIn(f"{FEATURE} must be 0 or 1", MAKEFILE)
        self.assertIn("TH08_PSP_BULLET_COLLISION_GATE_AUDIT_ENABLED", BULLET_CPP)

    def test_build_stamp_clean_and_all_consumers_are_bound(self) -> None:
        stamp = "BULLET_COLLISION_GATE_AUDIT_CONFIG_STAMP"
        stamps = "BULLET_COLLISION_GATE_AUDIT_CONFIG_STAMPS"
        self.assertIn("bullet-collision-gate-audit-0.stamp", MAKEFILE)
        self.assertIn("bullet-collision-gate-audit-1.stamp", MAKEFILE)
        self.assertIn(f"$({stamps})", MAKEFILE)
        self.assertIn(f"$({stamp}):", MAKEFILE)
        dependency = (
            "src/BulletManager.o src/Player.o psp/memory_telemetry.o "
            "psp/main.o: \\\n\t$(BULLET_COLLISION_GATE_AUDIT_CONFIG_STAMP)"
        )
        self.assertIn(dependency, MAKEFILE)

    def test_boot_fingerprint_is_value_gated_and_logged(self) -> None:
        fingerprint = "TH08_PSP_FEATURE_BULLET_COLLISION_GATE_AUDIT"
        self.assertIn(f"#define {fingerprint} 1", MAIN_CPP)
        self.assertIn(f"#define {fingerprint} 0", MAIN_CPP)
        self.assertIn("BULLET_COLLISION_GATE_AUDIT=%d", MAIN_CPP)
        self.assertIn(fingerprint + ",", MAIN_CPP)

    def test_interval_telemetry_peeks_marks_takes_samples_and_resets(self) -> None:
        self.assertIn(
            "PspTakeBulletCollisionGateAuditTelemetry()",
            MEMORY_TELEMETRY_CPP,
        )
        self.assertIn(
            "PspPeekBulletCollisionGateAuditTelemetry()",
            MEMORY_TELEMETRY_CPP,
        )
        # Initialization plus every stage-relative baseline must discard stale
        # counts before an accepted comparison window begins.
        self.assertGreaterEqual(
            MEMORY_TELEMETRY_CPP.count(
                "PspResetBulletCollisionGateAuditTelemetry();"
            ),
            2,
        )
        self.assertIn("BULLET_COLLISION_GATE_AUDIT_POLICY", MEMORY_TELEMETRY_CPP)
        self.assertIn("mark_is_non_destructive=1", MEMORY_TELEMETRY_CPP)
        self.assertIn("canonical_always_runs=1", MEMORY_TELEMETRY_CPP)

    def test_log_exposes_acceptance_witnesses_and_partitions(self) -> None:
        for field in (
            "cancel_false_empty_witnesses=%llu",
            "snapshot_mutation_witnesses=%llu",
            "false_clear_witnesses=%llu",
            "item_type_witnesses=%llu",
            "collision_eligible=%llu",
            "canonical_zero=%llu",
            "canonical_one=%llu",
            "canonical_two=%llu",
            "canonical_result_sum=%llu",
            "canonical_partition_ok=%d",
            "decision_bucket_sum=%llu",
            "decision_partition_ok=%d",
            "acceptance_witnesses_zero=%d",
        ):
            self.assertIn(field, MEMORY_TELEMETRY_CPP)
        for bucket in (
            "clearGrazeSuppressed",
            "clearGrazeSeparate",
            "clearLethalSeparate",
            "cancelUnknownFallbacks",
            "invalidSnapshotFallbacks",
            "invalidBulletFallbacks",
            "touchOrOverlapFallbacks",
        ):
            self.assertIn(
                f"bulletCollisionGateAudit.{bucket}", MEMORY_TELEMETRY_CPP
            )

    def test_frame_snapshot_is_after_item_update_and_before_traversal(self) -> None:
        update = function_body(
            BULLET_CPP, "ChainCallbackResult BulletManager::OnUpdate("
        )
        item = update.index("g_ItemManager.OnUpdate();")
        capture = update.index("PspCapturePlayerBulletCollisionAuditSnapshot")
        traversal = update.index("for (i = 0; i < 0x600; i++)")
        self.assertLess(item, capture)
        self.assertLess(capture, traversal)

    def test_snapshot_scans_all_authoritative_cancel_slots(self) -> None:
        capture = function_body(
            PLAYER_CPP, "PspCapturePlayerBulletCollisionAuditSnapshot"
        )
        self.assertIn("PspEnsurePlayerScanSidecar(player);", capture)
        self.assertIn("activeCancelRegionBits[wordIndex]", capture)
        self.assertIn("ARRAY_SIZE_SIGNED(player->cancelRegions)", capture)
        self.assertIn("player->cancelRegions[index].active", capture)
        self.assertIn("snapshot.sidecarClaimsEmpty", capture)
        self.assertIn("snapshot.authoritativeEmpty", capture)
        self.assertIn("snapshot.knownEmpty", capture)
        self.assertIn("u32 authoritativeActiveCount;", PLAYER_HPP)

    def test_gate_is_ordered_strict_and_fail_closed(self) -> None:
        gate = function_body(GATE_HPP, "PspBulletCollisionDefinitelyClear")
        for token in (
            "sizeX < 0.0f",
            "sizeY < 0.0f",
            "std::isfinite(positionX)",
            "std::isfinite(positionY)",
            "positionX - halfX",
            "halfX + positionX",
            "bulletLeft - 20.0f",
            "bulletRight + 20.0f",
            "grazeLeft > expandedRight",
            "grazeRight < expandedLeft",
            "hurtLeft > bulletRight",
            "hurtRight < bulletLeft",
        ):
            self.assertIn(token, gate)
        self.assertNotIn(">= expandedRight", gate)
        self.assertNotIn("<= expandedLeft", gate)
        self.assertNotIn(">= bulletRight", gate)
        self.assertNotIn("<= bulletLeft", gate)

    def test_audit_observes_existing_call_once_without_shortcut(self) -> None:
        update = function_body(
            BULLET_CPP, "ChainCallbackResult BulletManager::OnUpdate("
        )
        self.assertEqual(update.count("g_Player.CheckGrazeCollision("), 1)
        self.assertEqual(update.count("g_Player.CheckBulletCollision("), 1)
        graze_call = update.index("g_Player.CheckGrazeCollision(")
        graze_observe = update.index(
            "PspBulletCollisionGateAuditObserveCanonical(", graze_call
        )
        lethal_call = update.index("g_Player.CheckBulletCollision(")
        lethal_observe = update.index(
            "PspBulletCollisionGateAuditObserveCanonical(", lethal_call
        )
        self.assertLess(graze_call, graze_observe)
        self.assertLess(lethal_call, lethal_observe)
        audit_begin = function_body(
            BULLET_CPP, "PspBulletCollisionGateAuditBegin"
        )
        self.assertNotIn("CheckGrazeCollision", audit_begin)
        self.assertNotIn("CheckBulletCollision", audit_begin)
        self.assertNotIn("goto executeBulletScript", audit_begin)

    def test_negative_item_type_side_effect_and_witnesses_are_checked(self) -> None:
        observe = function_body(
            BULLET_CPP, "PspBulletCollisionGateAuditObserveCanonical"
        )
        self.assertIn("canonicalResult != 0", observe)
        self.assertIn("falseClearWitnesses", observe)
        self.assertIn("canonicalResult == 0", observe)
        self.assertIn("g_Player.bulletCancelItemType != 6", observe)
        self.assertIn("itemTypeWitnesses", observe)
        note_frame = function_body(
            BULLET_CPP, "PspBulletCollisionGateAuditNoteFrame"
        )
        self.assertIn("cancelFalseEmptyWitnesses", note_frame)
        self.assertIn("cancelStalePositiveFrames", note_frame)

    def test_bss_reservation_and_peek_take_reset_exist_with_audit_off(self) -> None:
        declaration = BULLET_CPP.index(
            "g_PspBulletCollisionGateAuditTelemetry{};"
        )
        enabled_helpers = BULLET_CPP.index(
            "#if defined(TH08_PSP_BULLET_COLLISION_GATE_AUDIT_ENABLED)"
        )
        self.assertLess(declaration, enabled_helpers)
        for field in (
            "collisionEligibleBullets",
            "clearGrazeSeparate",
            "clearLethalSeparate",
            "cancelFalseEmptyWitnesses",
            "snapshotMutationWitnesses",
            "falseClearWitnesses",
            "itemTypeWitnesses",
        ):
            self.assertIn(f"unsigned long long {field};", BULLET_HPP)
        self.assertIn("PspPeekBulletCollisionGateAuditTelemetry", BULLET_HPP)
        self.assertIn("PspTakeBulletCollisionGateAuditTelemetry", BULLET_HPP)
        self.assertIn("PspResetBulletCollisionGateAuditTelemetry", BULLET_HPP)
        take = function_body(
            BULLET_CPP, "PspTakeBulletCollisionGateAuditTelemetry"
        )
        self.assertLess(
            take.index("PspPeekBulletCollisionGateAuditTelemetry()"),
            take.index("PspResetBulletCollisionGateAuditTelemetry();"),
        )


if __name__ == "__main__":
    unittest.main()
