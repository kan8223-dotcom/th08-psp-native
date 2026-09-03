#!/usr/bin/env python3
"""Source-contract tests for TH08_PSP_GUI_BORDER_REPLAY (HUD border tile replay)."""

from __future__ import annotations

import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
AUDIT = "TH08_PSP_GUI_BORDER_REPLAY_AUDIT"
PRODUCT = "TH08_PSP_GUI_BORDER_REPLAY"


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


CANONICAL_LOOPS = """        for (yPos = 0.0f; yPos < 464.0f; yPos += 32.0f)
        {
            vm->pos = Float3(0.0f, yPos, 0.49f);
            g_AnmManager->DrawNoRotation(vm);
        }
        for (xPos = 416.0f; xPos < 624.0f; xPos += 32.0f)
        {
            for (yPos = 16.0f; yPos < 464.0f; yPos += 32.0f)
            {
                vm->pos = Float3(xPos, yPos, 0.49f);
                g_AnmManager->DrawNoRotation(vm);
            }
        }
        vm = &this->impl->frontVms[14];
        for (xPos = 0.0f; xPos < 624.0f; xPos += 128.0f)
        {
            vm->pos = Float3(xPos, 0.0f, 0.49f);
            g_AnmManager->DrawNoRotation(vm);
            vm->pos = Float3(xPos, 464.0f, 0.49f);
            g_AnmManager->DrawNoRotation(vm);
        }
"""


class PspGuiBorderReplayTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.makefile = read("Makefile.psp")
        cls.header = read("psp/gui_border_replay.hpp")
        cls.module = read("psp/gui_border_replay.cpp")
        cls.gui = read("src/Gui.cpp")
        cls.perf = read("psp/perf_attribution.cpp")
        cls.psp_main = read("psp/main.cpp")

    def test_makefile_and_fingerprint(self) -> None:
        for feature in (AUDIT, PRODUCT):
            self.assertIn(f"{feature} ?= 0", self.makefile)
            self.assertIn(f"$(error {feature} must be 0 or 1)", self.makefile)
            self.assertIn(f"CXXFLAGS += -D{feature}=1", self.makefile)
        self.assertIn(f"$(error {AUDIT} and {PRODUCT} are mutually exclusive)", self.makefile)
        self.assertIn(f"$(error {AUDIT}=1 requires TH08_PSP_PERF_ATTRIBUTION=1)", self.makefile)
        self.assertIn("PSP_GUI_BORDER_SRCS := psp/gui_border_replay.cpp", self.makefile)
        self.assertIn("src/Gui.o psp/gui_border_replay.o psp/perf_attribution.o psp/main.o: \\\n\t$(GUI_BORDER_REPLAY_AUDIT_CONFIG_STAMP) $(GUI_BORDER_REPLAY_CONFIG_STAMP)", self.makefile)
        self.assertIn('"GUI_BORDER_REPLAY_AUDIT=%d GUI_BORDER_REPLAY=%d "', self.psp_main)
        self.assertIn('#error "GUI_BORDER_REPLAY audit and product switches are mutually exclusive"', self.header)
        for hook in ("GuiBorderStatsResetWindow(gWindowActive);", "GuiBorderStatsEmitWindow(gStage, gBaselineStageFrame, stageFrame);", "GuiBorderStatsCancelWindow();"):
            self.assertIn(hook, self.perf)

    def test_gui_keeps_canonical_loops_and_replay_mirrors_them(self) -> None:
        scene = function_body(self.gui, "void Gui::DrawGameScene()")
        # OFF build: the verbatim loops; ON build: the replay entry with the same sprites.
        self.assertIn("#else\n" + CANONICAL_LOOPS + "#endif", scene)
        self.assertIn("th08::psp::GuiBorderDrawTiles(vm, &this->impl->frontVms[14]);\n        vm = &this->impl->frontVms[14];", scene)
        before = scene.index("vm = &this->impl->frontVms[13];")
        self.assertLess(before, scene.index("GuiBorderDrawTiles"))
        canon = function_body(self.module, "bool RunCanonical(AnmVm *vm13, AnmVm *vm14, Record *record)")
        # The module's loops are the canonical ones (positions, z, call order).
        for line in ("vm->pos = Float3(0.0f, yPos, 0.49f);", "for (xPos = 416.0f; xPos < 624.0f; xPos += 32.0f)",
                     "for (yPos = 16.0f; yPos < 464.0f; yPos += 32.0f)", "vm->pos = Float3(xPos, yPos, 0.49f);",
                     "for (xPos = 0.0f; xPos < 624.0f; xPos += 128.0f)", "vm->pos = Float3(xPos, 0.0f, 0.49f);",
                     "vm->pos = Float3(xPos, 464.0f, 0.49f);"):
            self.assertIn(line, canon, line)
        self.assertEqual(canon.count("g_AnmManager->DrawNoRotation(vm);"), 4)
        self.assertEqual(self.module.count("kTileCount = kLeftColumnTiles + kRightPanelTiles + kStripTiles"), 1)
        self.assertIn("kLeftColumnTiles = 15U", self.module)
        self.assertIn("kRightPanelTiles = 98U", self.module)
        self.assertIn("kStripTiles = 10U", self.module)

    def test_replay_reproduces_first_tile_state_calls(self) -> None:
        replay = function_body(self.module, "bool Replay(AnmVm *vm13, AnmVm *vm14)")
        for line in ("if (manager->currentTexture != vm->loadedSprite->texture)", "manager->FlushVertexBuffer();",
                     "g_Supervisor.d3dDevice->SetTexture(0, manager->currentTexture);", "if (manager->currentVertexShader != 1)",
                     "manager->SetRenderStateForVm(vm);", "manager->renderStateChangesThisFrame += kTileCount - 2U;",
                     "GuiBorderBatchCanAppend(manager, kVertexCount)", "manager->spritesToDraw += kTileCount;"):
            self.assertIn(line, replay, line)
        self.assertLess(replay.index("manager->SetRenderStateForVm(vm);"), replay.index("std::memcpy(manager->vertexBufferEndPtr"))
        self.assertIn("GuiBorderBatchActive()", replay)
        # The audit path compares the canonical vertices with the stored record bit for bit.
        draw = function_body(self.module, "void GuiBorderDrawTiles(AnmVm *vm13, AnmVm *vm14)")
        self.assertIn("std::memcmp(gScratch.vertices, gRecord.vertices,", draw)
        self.assertEqual(self.module.count("BootLog("), 1)
        fmt = "".join(re.findall(r'"([^"]*)"', re.search(r'BootLog\(\s*((?:"[^"]*"\s*)+),', self.module).group(1)))
        self.assertEqual(len(re.findall(r"%(?:0\d)?(?:ll|l)?[dusx]", fmt)), 12)


if __name__ == "__main__":
    unittest.main()
