#!/usr/bin/env python3
"""Static/build-provenance gates for the PSP Bullet mixed-quad integration."""

from __future__ import annotations

import hashlib
import re
import shutil
import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAKEFILE = (ROOT / "Makefile.psp").read_text(encoding="utf-8")
MAIN = (ROOT / "psp/main.cpp").read_text(encoding="utf-8")
TELEMETRY = (ROOT / "psp/memory_telemetry.cpp").read_text(encoding="utf-8")
ANM = (ROOT / "src/AnmManager.cpp").read_text(encoding="utf-8")
ANM_HEADER = (ROOT / "src/AnmManager.hpp").read_text(encoding="utf-8")
ARCHIVE = ROOT / "deps/pspgl-ge4/libGL_th08_ge4_mixed_v1.a"
PATCH = ROOT / "deps/pspgl-ge4/pspgl-th08-native-mixed-submit-v1.patch"


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


class BulletMixedIntegrationTests(unittest.TestCase):
    def test_makefile_exposes_item_and_combined_mixed_test_targets(self) -> None:
        for token in (
            ".PHONY: test-psp-item-mixed-quads",
            "test-psp-item-mixed-quads: tools/test_psp_item_mixed_quads.py",
            "python3 tools/test_psp_item_mixed_quads.py",
            ".PHONY: test-psp-mixed-quads",
            "test-psp-mixed-quads: test-psp-bullet-mixed-quads",
            "test-psp-item-mixed-quads",
            "exactly these five translation",
        ):
            self.assertIn(token, MAKEFILE)

    def test_modes_are_default_off_exclusive_and_dependency_gated(self) -> None:
        for token in (
            "TH08_PSP_BULLET_MIXED_QUADS_AUDIT ?= 0",
            "TH08_PSP_BULLET_MIXED_QUADS_FASTPATH ?= 0",
            "TH08_PSP_BULLET_MIXED_QUADS_AUDIT and TH08_PSP_BULLET_MIXED_QUADS_FASTPATH are mutually exclusive",
            "TH08_PSP_BULLET_MIXED_QUADS_AUDIT=1 requires TH08_PSP_BULLET_UNIFIED_QUADS=1",
            "TH08_PSP_BULLET_MIXED_QUADS_FASTPATH=1 requires TH08_PSP_BULLET_UNIFIED_QUADS=1",
            "TH08_PSP_BULLET_MIXED_QUADS_FASTPATH=1 requires TH08_PSP_BULLET_DIRECT_GE=1",
            "-DTH08_PSP_BULLET_MIXED_QUADS_AUDIT=1",
            "-DTH08_PSP_BULLET_MIXED_QUADS_FASTPATH=1",
        ):
            self.assertIn(token, MAKEFILE)

    def test_stamps_clean_and_all_mixed_consumers_are_bound(self) -> None:
        for token in (
            "bullet-mixed-quads-audit-0.stamp",
            "bullet-mixed-quads-audit-1.stamp",
            "bullet-mixed-quads-fastpath-0.stamp",
            "bullet-mixed-quads-fastpath-1.stamp",
            "$(BULLET_MIXED_QUADS_AUDIT_CONFIG_STAMPS)",
            "$(BULLET_MIXED_QUADS_FASTPATH_CONFIG_STAMPS)",
        ):
            self.assertIn(token, MAKEFILE)
        dependency = re.compile(
            r"src/AnmManager\.o src/modern/linux/d3d8_compat\.o psp/main\.o\s+"
            r"\\\s+psp/memory_telemetry\.o src/BulletManager\.o:\s+\\\s+"
            r"\$\(BULLET_MIXED_QUADS_AUDIT_CONFIG_STAMP\)\s+\\\s+"
            r"\$\(BULLET_MIXED_QUADS_FASTPATH_CONFIG_STAMP\)",
            re.MULTILINE,
        )
        self.assertRegex(MAKEFILE, dependency)

    def test_only_product_selects_frozen_mixed_archive(self) -> None:
        selection = MAKEFILE[
            MAKEFILE.index("# Audit/OFF retain the accepted v3 archive") :
            MAKEFILE.index("# SC-only is an intentional link contract")
        ]
        self.assertIn(
            "PSPGL_GE4_SELECTED_ARCHIVE := $(PSPGL_GE4_ARCHIVE)", selection
        )
        product = selection[selection.index(
            "ifneq ($(filter 1,$(TH08_PSP_BULLET_MIXED_QUADS_FASTPATH) $(TH08_PSP_ITEM_MIXED_QUADS_FASTPATH)),)"
        ) :]
        self.assertIn(
            "PSPGL_GE4_SELECTED_ARCHIVE := $(PSPGL_GE4_MIXED_ARCHIVE)",
            product,
        )
        self.assertIn("$(PSPGL_GE4_SELECTED_ARCHIVE)", MAKEFILE)
        self.assertIn(
            "src/modern/linux/d3d8_compat.o: $(PSPGL_GE4_SELECTED_ARCHIVE_STAMP)",
            MAKEFILE,
        )
        self.assertIn(
            "grep -Fq '$(PSPGL_GE4_SELECTED_ARCHIVE)' TH08PSP.map", MAKEFILE
        )

    def test_archive_hash_size_markers_and_mixed_symbol_are_exact(self) -> None:
        self.assertEqual(ARCHIVE.stat().st_size, 1_683_644)
        self.assertEqual(
            hashlib.sha256(ARCHIVE.read_bytes()).hexdigest(),
            "b401e0f924ffdaffca62ac62e16f58000dd7b0b7d862675124f5289252ead530",
        )
        self.assertEqual(
            hashlib.sha256(PATCH.read_bytes()).hexdigest(),
            "26e76be666d31e9adea5660edcf67290cf747e8104260484345c596cca4e828a",
        )
        nm_tool = shutil.which("psp-nm") or "/usr/local/pspdev/bin/psp-nm"
        nm = subprocess.run(
            [nm_tool, "-S", "-g", "--defined-only", str(ARCHIVE)],
            check=True,
            capture_output=True,
            text=True,
        ).stdout
        self.assertRegex(
            nm,
            r"(?m)^[0-9a-f]+ 000002d8 T __pspgl_th08_draw_native_mixed_quads$",
        )
        self.assertIn("__pspgl_th08_ge4_fork_marker", nm)
        self.assertIn("__pspgl_th08_native_submit_marker", nm)
        archive_strings = subprocess.run(
            ["strings", str(ARCHIVE)],
            check=True,
            capture_output=True,
            text=True,
        ).stdout
        self.assertIn("pspGL TH08 GE4 fork v1 upstream", archive_strings)
        self.assertIn("pspGL TH08 native submit v3", archive_strings)
        self.assertIn("000002d8", MAKEFILE)

    def test_boot_fingerprint_has_both_unambiguous_bits(self) -> None:
        for feature in ("AUDIT", "FASTPATH"):
            macro = f"TH08_PSP_FEATURE_BULLET_MIXED_QUADS_{feature}"
            self.assertIn(f"#define {macro} 1", MAIN)
            self.assertIn(f"#define {macro} 0", MAIN)
            self.assertIn(f"BULLET_MIXED_QUADS_{feature}=%d", MAIN)
            self.assertIn(macro + ",", MAIN)

    def test_telemetry_reports_every_public_stats_field_and_mode(self) -> None:
        fields = {
            "passes": "passes=%lu",
            "ownerConflictPasses": "owner_conflict_passes=%lu",
            "stateRuns": "state_runs=%lu",
            "batches": "batches=%lu",
            "candidates": "candidates=%lu",
            "eligiblePrefixQuads": "eligible_prefix_quads=%lu",
            "generalQuads": "general_quads=%lu",
            "stickyGeneralQuads": "sticky_general_quads=%lu",
            "nonfiniteFallbacks": "nonfinite_fallbacks=%lu",
            "axisFallbacks": "axis_fallbacks=%lu",
            "areaOrMirrorFallbacks": "area_or_mirror_fallbacks=%lu",
            "zOrWFallbacks": "z_or_w_fallbacks=%lu",
            "uvFallbacks": "uv_fallbacks=%lu",
            "diffuseFallbacks": "diffuse_fallbacks=%lu",
            "submittedBatches": "submitted_batches=%lu",
            "submittedPairQuads": "submitted_pair_quads=%lu",
            "submittedGeneralQuads": "submitted_general_quads=%lu",
            "backendFallbackBatches": "backend_fallback_batches=%lu",
            "failClosedBatches": "fail_closed_batches=%lu",
            "missingRunBatches": "missing_run_batches=%lu",
            "invalidRangeBatches": "invalid_range_batches=%lu",
            "canonicalRecoveryDrawFailures": "canonical_recovery_draw_failures=%lu",
            "canonicalRecoveryQuads": "canonical_recovery_quads=%lu",
            "frontendVerticesSaved": "potential_frontend_vertices_saved=%lu",
            "geVerticesSaved": "potential_ge_vertices_saved=%lu",
            "maxPairPrefix": "max_pair_prefix=%lu",
            "maxGeneralSuffix": "max_general_suffix=%lu",
        }
        self.assertIn('bulletMixedQuadsMode = "audit"', TELEMETRY)
        self.assertIn('bulletMixedQuadsMode = "product"', TELEMETRY)
        stats_declaration = ANM_HEADER[
            ANM_HEADER.index("struct BulletMixedQuadStats") :
            ANM_HEADER.index("const BulletMixedQuadStats &GetBulletMixedQuadStats")
        ]
        public_fields = set(re.findall(r"\bu32\s+(\w+)\s*;", stats_declaration))
        self.assertEqual(public_fields, set(fields))
        for member, label in fields.items():
            self.assertIn(label, TELEMETRY)
            self.assertIn(f"bulletMixedQuads.{member}", TELEMETRY)
        self.assertIn("submitted_frontend_vertices_saved=%llu", TELEMETRY)
        self.assertIn("submitted_ge_vertices_saved=%llu", TELEMETRY)
        self.assertGreaterEqual(
            TELEMETRY.count("bulletMixedQuads.submittedPairQuads"), 3
        )

    def test_marks_peek_and_interval_boundaries_reset_only_bullet_stats(self) -> None:
        helper = function_body(TELEMETRY, "void ResetBulletMixedQuadIntervalIfReady")
        self.assertIn("ResetPspBulletMixedQuadStats", helper)
        self.assertNotIn("ResetPspItemTimeDrawPairStats", helper)
        baseline = function_body(TELEMETRY, "void BeginStageRelativePerfBaseline")
        initialize = function_body(TELEMETRY, "void MemoryTelemetryInitialize")
        mark = function_body(TELEMETRY, "void MemoryTelemetryMarkPhase")
        sample = function_body(TELEMETRY, "void MemoryTelemetryAfterPresent")
        self.assertIn("ResetBulletMixedQuadIntervalIfReady", baseline)
        self.assertIn("ResetBulletMixedQuadIntervalIfReady", initialize)
        self.assertEqual(sample.count("ResetBulletMixedQuadIntervalIfReady"), 1)
        self.assertNotIn("ResetBulletMixedQuadIntervalIfReady", mark)
        self.assertIn("mark_is_non_destructive=1", TELEMETRY)
        self.assertIn("telemetry_reset_scope=bullet_stats_only", TELEMETRY)
        self.assertIn(
            "accept_requires_fail_closed_missing_run_invalid_range_", TELEMETRY
        )

    def test_off_audit_product_keep_the_fixed_sidecar_reservation(self) -> None:
        self.assertIn("kPspItemTimeDrawPairSidecarBytes = 512U", ANM)
        self.assertIn("g_PspItemTimeDrawPairSidecarReservation", ANM)
        self.assertIn("__attribute__((used))", ANM)
        self.assertIn("C_ASSERT(sizeof(AnmManager) == 0x2a2570)", ANM_HEADER)
        self.assertIn("shared_sidecar_bytes=512 fixed_bss=1", TELEMETRY)


if __name__ == "__main__":
    unittest.main(verbosity=2)
