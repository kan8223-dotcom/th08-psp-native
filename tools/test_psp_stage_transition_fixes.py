#!/usr/bin/env python3
"""Source-contract tests for TH08_PSP_REPLAY_RESERVE_RECYCLE and TH08_PSP_DIALOGUE_SNAPSHOT_NO_PROMOTE."""

from __future__ import annotations

import pathlib
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


class PspStageTransitionFixTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.makefile = read("Makefile.psp")
        cls.replay = read("src/ReplayManager.cpp")
        cls.compat = read("src/modern/linux/d3d8_compat.cpp")
        cls.psp_main = read("psp/main.cpp")

    def test_makefile_and_fingerprint(self) -> None:
        m = self.makefile
        for switch in ("TH08_PSP_REPLAY_RESERVE_RECYCLE", "TH08_PSP_DIALOGUE_SNAPSHOT_NO_PROMOTE"):
            self.assertIn(f"{switch} ?= 0", m)
            self.assertIn(f"$(error {switch} must be 0 or 1)", m)
            self.assertIn(f"CXXFLAGS += -D{switch}=1", m)
        for stamp in ("replay-reserve-recycle-0.stamp", "replay-reserve-recycle-1.stamp",
                      "dialogue-snapshot-no-promote-0.stamp", "dialogue-snapshot-no-promote-1.stamp"):
            self.assertIn(stamp, m)
        self.assertIn("src/ReplayManager.o psp/main.o: \\\n\t$(REPLAY_RESERVE_RECYCLE_CONFIG_STAMP)", m)
        self.assertIn('"REPLAY_RESERVE_RECYCLE=%d DIALOGUE_SNAPSHOT_NO_PROMOTE=%d "', self.psp_main)
        self.assertIn("        TH08_PSP_FEATURE_REPLAY_RESERVE_RECYCLE,", self.psp_main)
        self.assertIn("        TH08_PSP_FEATURE_DIALOGUE_SNAPSHOT_NO_PROMOTE,", self.psp_main)
        self.assertIn("#define TH08_PSP_REPLAY_RESERVE_RECYCLE_ENABLED 0", read("psp/replay_reserve_recycle.hpp"))
        self.assertIn("#define TH08_PSP_DIALOGUE_SNAPSHOT_NO_PROMOTE_ENABLED 0", read("psp/dialogue_snapshot_no_promote.hpp"))

    def test_recycle_keeps_capacity_blocks_and_moves_end_pointers(self) -> None:
        body = function_body(self.replay, "void ReplayManager::CompactRecordedStage(i32 stage)")
        start = body.index("#if TH08_PSP_REPLAY_RESERVE_RECYCLE_ENABLED")
        recycle = body[start:body.index("#endif", start)]
        self.assertIn("if (gPreparedReplayInput == NULL && gPreparedReplayFps == NULL &&\n        inputCapacity == kReplayInputCapacity && fpsCapacity == kReplayFpsCapacity)", recycle)
        self.assertIn("memcpy(compactInput, inputBase, inputUsed);", recycle)
        self.assertIn("gPreparedReplayInput = inputBase;\n            gPreparedReplayFps = fpsBase;", recycle)
        self.assertNotIn("th08_psp_tracked_realloc", recycle)
        self.assertNotIn("g_ZunMemory.Free(inputBase)", recycle)
        for moved in ("mgr->replayInputEnds[stage] = reinterpret_cast<u8 *>(compactInput) + inputUsed;",
                      "mgr->replayFpsSampleEnds[stage] = compactFps + fpsUsed;",
                      "gReplayInputCapacities[stage] = compactInputCapacity;",
                      "gReplayFpsCapacities[stage] = fpsUsed;"):
            self.assertIn(moved, recycle)
        self.assertLess(start, body.index("th08_psp_tracked_realloc(inputBase"))
        prep = function_body(self.replay, "ZunResult ReplayManager::PrepareRecordingStageBuffers()")
        self.assertIn("if (gPreparedReplayInput != NULL && gPreparedReplayFps != NULL)\n        return ZUN_SUCCESS;", prep)

    def test_snapshot_upload_skips_promotion(self) -> None:
        c = self.compat
        self.assertIn('#include "dialogue_snapshot_no_promote.hpp"', c)
        self.assertIn("            discardCpuCopy && !surface->pixels.empty()\n#if TH08_PSP_DIALOGUE_SNAPSHOT_NO_PROMOTE_ENABLED\n            && !pspSuppressStaticUploadPromotion\n#endif", c)
        restore = function_body(c, "void RestoreDialogueSnapshot()")
        self.assertIn("pspSuppressStaticUploadPromotion = true;", restore)
        self.assertIn("pspSuppressStaticUploadPromotion = false;", restore)
        self.assertLess(restore.index("pspSuppressStaticUploadPromotion = true;"), restore.index("DrawPspSurfaceCache(dialogueSnapshotSurface"))
        self.assertLess(restore.index("DrawPspSurfaceCache(dialogueSnapshotSurface"), restore.index("pspSuppressStaticUploadPromotion = false;"))
        # The capture path (native capture texture and arena fallback) is covered too.
        capture = function_body(c, "void CaptureDialogueSnapshot()")
        self.assertLess(capture.index("pspSuppressStaticUploadPromotion = true;"), capture.index("th08_linux_surface_capture_native("))
        self.assertLess(capture.index("th08_linux_surface_capture_native("), capture.index("pspSuppressStaticUploadPromotion = false;"))
        self.assertEqual(c.count("PspGe4StaticUploadScope staticUpload(!pspSuppressStaticUploadPromotion);"), 2)


if __name__ == "__main__":
    unittest.main()
