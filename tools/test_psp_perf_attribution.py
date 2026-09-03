#!/usr/bin/env python3
"""Host/static contracts for the default-off PSP attribution probe."""

from __future__ import annotations

import pathlib
import subprocess
import tempfile
import textwrap
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


class PspPerfAttributionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.makefile = read("Makefile.psp")
        cls.header = read("psp/perf_attribution.hpp")
        cls.math = read("psp/perf_attribution_math.hpp")
        cls.probe = read("psp/perf_attribution.cpp")
        cls.main = read("src/main.cpp")
        cls.psp_main = read("psp/main.cpp")
        cls.backend = read("src/modern/linux/d3d8_compat.cpp")
        cls.bullet = read("src/BulletManager.cpp")
        cls.item = read("src/ItemManager.cpp")
        cls.effect = read("src/EffectManager.cpp")
        cls.enemy = read("src/EnemyManager.cpp")
        cls.enemy_update = read("src/EnemyManagerUpdate.cpp")
        cls.player = read("src/Player.cpp")

    def compile_host(self, source: str, output: pathlib.Path) -> None:
        unit = output.with_suffix(".cpp")
        unit.write_text(source, encoding="utf-8")
        subprocess.run(
            [
                "g++",
                "-std=c++17",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(ROOT),
                str(unit),
                "-o",
                str(output),
            ],
            check=True,
            cwd=ROOT,
            capture_output=True,
            text=True,
        )

    def test_default_off_removes_source_wrappers_and_calls(self) -> None:
        self.assertIn("TH08_PSP_PERF_ATTRIBUTION ?= 0", self.makefile)
        self.assertIn(
            "ifeq ($(TH08_PSP_PERF_ATTRIBUTION),1)\n"
            "PSP_PERF_ATTRIBUTION_SRCS := psp/perf_attribution.cpp",
            self.makefile,
        )
        self.assertIn("$(PSP_PERF_ATTRIBUTION_SRCS)", self.makefile)
        self.assertIn(
            "-DTH08_PSP_PERF_ATTRIBUTION=$(TH08_PSP_PERF_ATTRIBUTION)",
            self.makefile,
        )
        link_gate = self.makefile.index(
            "ifeq ($(TH08_PSP_PERF_ATTRIBUTION),1)",
            self.makefile.index("LDFLAGS :="),
        )
        link_end = self.makefile.index("endif", link_gate)
        gated_link = self.makefile[link_gate:link_end]
        self.assertIn("--wrap=sceGeListSync", gated_link)
        self.assertIn("--wrap=sceDisplayWaitVblankStart", gated_link)
        self.assertIn("PERF_ATTRIBUTION=%d", self.psp_main)

        # Preprocessing an OFF consumer must expose neither API declarations
        # nor references/strings from the diagnostic implementation.
        off_unit = textwrap.dedent(
            """
            #define PSP 1
            #define TH08_PSP_PERF_ATTRIBUTION 0
            #include "psp/perf_attribution.hpp"
            #if TH08_PSP_PERF_ATTRIBUTION_ENABLED
            #error attribution unexpectedly enabled
            #endif
            int main() { return TH08_PSP_PERF_ATTRIBUTION_ENABLED; }
            """
        )
        with tempfile.TemporaryDirectory() as directory:
            exe = pathlib.Path(directory) / "off"
            self.compile_host(off_unit, exe)
            symbols = subprocess.run(
                ["nm", "-C", str(exe)], check=True, capture_output=True, text=True
            ).stdout
            self.assertNotIn("PerfAttribution", symbols)
            subprocess.run([str(exe)], check=True)

    def test_difference_math_is_host_proven_and_saturating(self) -> None:
        harness = textwrap.dedent(
            """
            #include "psp/perf_attribution_math.hpp"
            #include <cstdint>
            using th08::psp::PerfAttributionSubtract;
            static_assert(PerfAttributionSubtract(100, 20).value == 80);
            static_assert(!PerfAttributionSubtract(100, 20).underflow);
            static_assert(PerfAttributionSubtract(100, 20, 30).value == 50);
            static_assert(PerfAttributionSubtract(10, 20).value == 0);
            static_assert(PerfAttributionSubtract(10, 20).underflow);
            static_assert(PerfAttributionSubtract(UINT64_MAX, UINT64_MAX, 1).value == 0);
            static_assert(PerfAttributionSubtract(UINT64_MAX, UINT64_MAX, 1).underflow);
            int main() { return 0; }
            """
        )
        with tempfile.TemporaryDirectory() as directory:
            exe = pathlib.Path(directory) / "math"
            self.compile_host(harness, exe)
            subprocess.run([str(exe)], check=True)

    def test_runtime_harness_emits_one_render20_window(self) -> None:
        harness = textwrap.dedent(
            r"""
            #include <cstdarg>
            #include <cstdint>
            #include <cstdio>
            #include <cstring>
            static std::uint64_t fakeNow;
            static char lastLog[4096];
            extern "C" std::uint64_t sceKernelGetSystemTimeWide() {
                return ++fakeNow;
            }
            extern "C" int sceKernelGetThreadId() { return 7; }
            extern "C" int __real_sceGeListSync(int, int) {
                fakeNow += 10; return 0;
            }
            extern "C" int __real_sceDisplayWaitVblankStart() {
                fakeNow += 20; return 0;
            }
            #define PSP 1
            #define TH08_PSP_PERF_ATTRIBUTION 1
            #include "psp/perf_attribution.cpp"
            namespace th08::psp {
            void BootLog(const char *format, ...) {
                va_list arguments;
                va_start(arguments, format);
                std::vsnprintf(lastLog, sizeof(lastLog), format, arguments);
                va_end(arguments);
            }
            }
            using th08::psp::PerfAttributionPhase;
            using th08::psp::PerfAttributionScope;
            using th08::psp::PerfAttributionWaitContext;
            using th08::psp::PerfAttributionWaitContextScope;
            int main() {
                th08::psp::PerfAttributionInitialize();
                th08::psp::PerfAttributionAfterPresent(4, 3, 2, true, 5);
                for (std::uint32_t frame = 4; frame <= 603; ++frame) {
                    {
                        PerfAttributionScope calc(PerfAttributionPhase::CalcChain);
                        { PerfAttributionScope s(PerfAttributionPhase::PlayerUpdate); }
                        { PerfAttributionScope s(PerfAttributionPhase::EnemyUpdate); }
                        { PerfAttributionScope s(PerfAttributionPhase::EffectUpdate); }
                        {
                            PerfAttributionScope bullet(
                                PerfAttributionPhase::BulletUpdateInclusive);
                            { PerfAttributionScope item(PerfAttributionPhase::ItemUpdate); }
                        }
                    }
                    if (frame % 3 != 0) continue;
                    {
                        PerfAttributionScope drawFrame(PerfAttributionPhase::DrawFrame);
                        PerfAttributionScope drawChain(PerfAttributionPhase::DrawChain);
                        { PerfAttributionScope s(PerfAttributionPhase::PlayerDraw); }
                        { PerfAttributionScope s(PerfAttributionPhase::EnemyDraw); }
                        { PerfAttributionScope s(PerfAttributionPhase::EffectDrawMain); }
                        { PerfAttributionScope s(PerfAttributionPhase::EffectDrawBackground); }
                        {
                            PerfAttributionScope bullet(
                                PerfAttributionPhase::BulletDrawInclusive);
                            { PerfAttributionScope item(PerfAttributionPhase::ItemDraw); }
                            { PerfAttributionScope effect(
                                PerfAttributionPhase::EffectDrawBullet); }
                        }
                    }
                    {
                        PerfAttributionWaitContextScope cadence(
                            PerfAttributionWaitContext::Cadence);
                        __wrap_sceDisplayWaitVblankStart();
                        __wrap_sceDisplayWaitVblankStart();
                    }
                    {
                        PerfAttributionScope present(PerfAttributionPhase::PresentOuter);
                        { PerfAttributionScope pre(PerfAttributionPhase::PresentPreSwap); }
                        {
                            PerfAttributionScope swap(PerfAttributionPhase::PresentSwap);
                            PerfAttributionWaitContextScope context(
                                PerfAttributionWaitContext::Swap);
                            __wrap_sceGeListSync(1, 0);
                            __wrap_sceDisplayWaitVblankStart();
                        }
                        { PerfAttributionScope post(PerfAttributionPhase::PresentPostSwap); }
                    }
                    th08::psp::PerfAttributionAfterPresent(
                        4, frame, 2, true, 5);
                }
                if (std::strlen(lastLog) >= sizeof(lastLog) - 1) return 1;
                if (!std::strstr(lastLog, "PERF_ATTR V1")) return 2;
                if (!std::strstr(lastLog, "sf=3-603")) return 3;
                if (!std::strstr(lastLog, "sim_frames=600 sim_hz=60")) return 4;
                if (!std::strstr(lastLog, "rendered_frames=200")) return 5;
                if (!std::strstr(lastLog, "render_target_fps=20 cadence_mode=2")) return 6;
                if (!std::strstr(lastLog, "replay=1 demo=5")) return 7;
                if (!std::strstr(lastLog, "ge=")) return 8;
                if (!std::strstr(lastLog, "vbs=")) return 9;
                if (!std::strstr(lastLog, "vbc=")) return 10;
                if (!std::strstr(lastLog, "uf=0x00")) return 11;
                return 0;
            }
            """
        )
        with tempfile.TemporaryDirectory() as directory:
            exe = pathlib.Path(directory) / "runtime"
            self.compile_host(harness, exe)
            subprocess.run([str(exe)], check=True)

    def test_callback_coverage_uses_entry_raii_only(self) -> None:
        expected = (
            (self.bullet, "BulletUpdateInclusive", 1),
            (self.bullet, "BulletDrawInclusive", 1),
            (self.item, "ItemUpdate", 1),
            (self.item, "ItemDraw", 1),
            (self.effect, "EffectUpdate", 1),
            (self.effect, "EffectDrawMain", 1),
            (self.effect, "EffectDrawBullet", 1),
            (self.effect, "EffectDrawBackground", 1),
            (self.enemy_update, "EnemyUpdate", 1),
            (self.enemy, "EnemyDraw", 2),
            (self.player, "PlayerUpdate", 1),
            (self.player, "PlayerDraw", 2),
        )
        for source, phase, count in expected:
            self.assertEqual(source.count(f"PerfAttributionPhase::{phase}"), count)
        self.assertEqual(self.main.count("PerfAttributionPhase::CalcChain"), 1)
        self.assertEqual(self.main.count("PerfAttributionPhase::DrawFrame"), 1)
        self.assertEqual(self.main.count("PerfAttributionPhase::DrawChain"), 1)
        self.assertEqual(self.main.count("PerfAttributionPhase::PresentOuter"), 1)

    def test_bullet_exclusive_is_derived_without_call_graph_change(self) -> None:
        bullet_update = function_body(
            self.bullet,
            "ChainCallbackResult BulletManager::OnUpdate(BulletManager *bulletManager)",
        )
        bullet_draw = function_body(
            self.bullet,
            "ChainCallbackResult BulletManager::OnDraw(BulletManager *bulletManager)",
        )
        self.assertEqual(bullet_update.count("g_ItemManager.OnUpdate()"), 1)
        self.assertEqual(bullet_draw.count("g_ItemManager.OnDraw()"), 1)
        self.assertEqual(
            bullet_draw.count("g_EffectManager.DrawBulletLayerEffects()"), 1
        )
        emit = function_body(self.probe, "void EmitWindow")
        self.assertIn("BulletUpdateInclusive", emit)
        self.assertIn("ItemUpdate", emit)
        self.assertIn("BulletDrawInclusive", emit)
        self.assertIn("ItemDraw", emit)
        self.assertIn("EffectDrawBullet", emit)
        self.assertNotIn("g_ItemManager", self.probe)
        self.assertNotIn("g_BulletManager", self.probe)

    def test_present_ge_and_vblank_waits_are_separate(self) -> None:
        present = function_body(
            self.backend,
            "HRESULT Present(const RECT *, const RECT *, HWND, const RGNDATA *)",
        )
        for phase in ("PresentPreSwap", "PresentSwap", "PresentPostSwap"):
            self.assertIn(f"PerfAttributionPhase::{phase}", present)
        swap = present.index("PerfAttributionWaitContext::Swap")
        swap_window = present.index("SDL_GL_SwapWindow(window)")
        self.assertLess(swap, swap_window)
        cadence = function_body(self.main, "static void WaitForPspRenderCadence")
        self.assertIn("PerfAttributionWaitContext::Cadence", cadence)
        self.assertIn('__wrap_sceGeListSync', self.probe)
        self.assertIn('__real_sceGeListSync', self.probe)
        self.assertIn('__wrap_sceDisplayWaitVblankStart', self.probe)
        self.assertIn('GeWaitSwap', self.probe)
        self.assertIn('VblankWaitSwap', self.probe)
        self.assertIn('VblankWaitCadence', self.probe)

    def test_600_tick_window_labels_simulation_and_render_distinctly(self) -> None:
        after = function_body(self.probe, "void PerfAttributionAfterPresent")
        self.assertIn("kWindowStageFrames = 600U", self.probe)
        self.assertIn("stageFrame <= gLastStageFrame", after)
        self.assertIn("cadenceMode != gCadenceMode", after)
        self.assertIn("replay != gReplay", after)
        self.assertIn("demoReplay != gDemoReplay", after)
        self.assertIn("stageFrame > gTargetStageFrame", after)
        self.assertIn("PERF_ATTR V1", self.probe)
        self.assertIn("sim_frames=600 sim_hz=60", self.probe)
        self.assertIn("rendered_frames=%lu render_target_fps=%u", self.probe)
        self.assertIn("60U / (gCadenceMode + 1U)", self.probe)

    def test_fixed_memory_self_calibration_and_buffered_logging(self) -> None:
        self.assertIn("PhaseStat gStats[kPhaseCount]{}", self.probe)
        for forbidden in ("malloc(", "calloc(", "realloc(", "new ", "std::vector"):
            self.assertNotIn(forbidden, self.probe)
        initialize = function_body(self.probe, "void PerfAttributionInitialize")
        self.assertIn("kTimerCalibrationReads", initialize)
        self.assertIn("RawNow()", initialize)
        self.assertIn("clock=sceKernelGetSystemTimeWide", initialize)
        emit = function_body(self.probe, "void EmitWindow")
        self.assertIn("EstimateTimerOverheadUs()", emit)
        self.assertNotIn("\n    FlushBootLog(", emit)
        self.assertIn("log_flush_per_sample=0", self.probe)
        self.assertIn("runtime_telemetry_independent=1", self.probe)


if __name__ == "__main__":
    unittest.main(verbosity=2)
