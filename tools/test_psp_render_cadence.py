#!/usr/bin/env python3
"""Determinism and integration gates for PSP SELECT render cadence."""

from __future__ import annotations

import pathlib
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
HEADER = ROOT / "psp" / "render_cadence.hpp"
SOURCE = ROOT / "psp" / "render_cadence.cpp"
HARNESS = ROOT / "tools" / "psp_render_cadence_harness.cpp"
MAIN = ROOT / "src" / "main.cpp"
SUPERVISOR = ROOT / "src" / "Supervisor.cpp"
INPUT = ROOT / "src" / "modern" / "linux" / "linux_compat.cpp"
REPLAY = ROOT / "src" / "ReplayManager.cpp"
PLATFORM = ROOT / "psp" / "platform.cpp"
PSP_MAIN = ROOT / "psp" / "main.cpp"
MAKEFILE = ROOT / "Makefile.psp"


class PspRenderCadenceTests(unittest.TestCase):
    def test_exact_scheduler_harness(self) -> None:
        with tempfile.TemporaryDirectory(prefix="th08-render-cadence-") as temp:
            for initial_mode in (0, 2):
                binary = pathlib.Path(temp) / f"render-cadence-{initial_mode}"
                subprocess.run(
                    [
                        "g++",
                        "-std=c++17",
                        "-O2",
                        "-Wall",
                        "-Wextra",
                        "-Werror",
                        f"-DTH08_PSP_RENDER_CADENCE_INITIAL_MODE={initial_mode}",
                        "-I",
                        str(ROOT),
                        str(SOURCE),
                        str(HARNESS),
                        "-o",
                        str(binary),
                    ],
                    check=True,
                    cwd=ROOT,
                )
                result = subprocess.run(
                    [str(binary)], check=True, text=True, capture_output=True
                )
                self.assertIn(
                    f"render-cadence: initial_mode={initial_mode} PASS",
                    result.stdout,
                )

            invalid = subprocess.run(
                [
                    "g++",
                    "-std=c++17",
                    "-DTH08_PSP_RENDER_CADENCE_INITIAL_MODE=3",
                    "-I",
                    str(ROOT),
                    str(SOURCE),
                    str(HARNESS),
                    "-o",
                    str(pathlib.Path(temp) / "render-cadence-invalid"),
                ],
                text=True,
                capture_output=True,
                cwd=ROOT,
            )
            self.assertNotEqual(invalid.returncode, 0)
            self.assertIn(
                "TH08_PSP_RENDER_CADENCE_INITIAL_MODE must be 0, 1 or 2",
                invalid.stderr,
            )

    def test_scheduler_is_pure_and_allocation_free(self) -> None:
        source = SOURCE.read_text(encoding="utf-8")
        header = HEADER.read_text(encoding="utf-8")
        combined = source + header
        for forbidden in (
            "malloc",
            "new ",
            "g_Supervisor",
            "g_GameManager",
            "g_Rng",
            "g_SoundPlayer",
            "g_CurFrameInput",
            "ReplayManager",
            "ReplaySyncAudit",
            "frameskipConfig",
            "sceCtrl",
            "sceDisplay",
        ):
            self.assertNotIn(forbidden, combined)

    def test_select_is_out_of_band_from_game_input(self) -> None:
        source = INPUT.read_text(encoding="utf-8")
        psp_block = source.split("void FillKeyboard", 1)[1].split(
            "#else", 1
        )[0]
        self.assertNotIn("PSP_CTRL_SELECT", psp_block)
        self.assertNotIn("MAP_PSP_KEY(DIK_P", psp_block)
        self.assertNotIn("MAP_PSP_KEY('P'", psp_block)
        self.assertIn("PSP_CTRL_SELECT", PLATFORM.read_text(encoding="utf-8"))

    def test_gameplay_and_replay_do_not_own_cadence(self) -> None:
        replay = REPLAY.read_text(encoding="utf-8")
        self.assertNotIn("render_cadence", replay)
        self.assertNotIn("PlatformSelectButtonDown", replay)

    def test_render_only_is_divided(self) -> None:
        main = MAIN.read_text(encoding="utf-8")
        tick = main.index("psp::TickRenderCadence")
        draw = main.index("g_Chain.RunDrawChain", tick)
        present = main.index("Present();", draw)
        self.assertLess(tick, draw)
        self.assertLess(draw, present)
        self.assertIn("g_Chain.RunCalcChain()", main[:tick])
        self.assertIn("g_SoundPlayer.ProcessQueues()", main[:tick])
        self.assertIn("screenTransitionCountdown--", main[present:])
        self.assertIn("&& psp::PlatformRunning()", main)

    def test_mode_persists_across_game_transitions(self) -> None:
        main = MAIN.read_text(encoding="utf-8")
        self.assertEqual(main.count("psp::ResetRenderCadence("), 1)
        self.assertIn("RENDER_CADENCE init_mode=%u mode=%u", main)
        self.assertIn("select_edge_count=%lu mode=%u", main)
        self.assertIn(
            "RENDER_CADENCE_SUMMARY initial_mode=%u final_mode=%u", main
        )
        self.assertIn("select_edge_count=%lu", main)

    def test_fps_counts_exact_covered_simulation_ticks(self) -> None:
        supervisor = SUPERVISOR.read_text(encoding="utf-8")
        self.assertIn("psp::CurrentDrawSimulationTicks()", supervisor)
        self.assertIn("shouldDraw ? psp::CurrentDrawSimulationTicks() : 1U", supervisor)

    def test_psp_build_and_test_targets_include_scheduler(self) -> None:
        makefile = MAKEFILE.read_text(encoding="utf-8")
        self.assertIn("psp/render_cadence.cpp", makefile)
        self.assertIn("test-psp-render-cadence", makefile)

    def test_diagnostic_initial_mode_build_contract(self) -> None:
        makefile = MAKEFILE.read_text(encoding="utf-8")
        psp_main = PSP_MAIN.read_text(encoding="utf-8")
        self.assertIn("TH08_PSP_RENDER_CADENCE_INITIAL_MODE ?= 0", makefile)
        self.assertIn(
            "TH08_PSP_RENDER_CADENCE_INITIAL_MODE must be 0, 1 or 2", makefile
        )
        self.assertIn(
            "-DTH08_PSP_RENDER_CADENCE_INITIAL_MODE="
            "$(TH08_PSP_RENDER_CADENCE_INITIAL_MODE)",
            makefile,
        )
        self.assertIn("render-cadence-initial-mode-0.stamp", makefile)
        self.assertIn("render-cadence-initial-mode-2.stamp", makefile)
        self.assertIn(
            "psp/render_cadence.o: $(RENDER_CADENCE_INITIAL_MODE_CONFIG_STAMP)",
            makefile,
        )
        self.assertIn("RENDER_CADENCE_INITIAL_MODE=%u", psp_main)


if __name__ == "__main__":
    unittest.main()
