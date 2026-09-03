#!/usr/bin/env python3
"""Source-contract and host-compile tests for the PERF_ENV window record."""

from __future__ import annotations

import pathlib
import re
import subprocess
import tempfile
import textwrap
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SWITCH = "TH08_PSP_PERF_ENV"


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


SDK_STUBS = {
    "psppower.h": "#pragma once\nint scePowerGetCpuClockFrequencyInt(void);\nint scePowerGetBusClockFrequencyInt(void);\n",
    "pspthreadman.h": textwrap.dedent(
        """
        #pragma once
        #include <cstdint>
        typedef unsigned int SceSize; typedef unsigned int SceUInt; typedef int SceUID;
        typedef struct SceKernelSysClock { uint32_t low; uint32_t hi; } SceKernelSysClock;
        typedef struct SceKernelThreadRunStatus { SceSize size; int status; int currentPriority; int waitType;
            int waitId; int wakeupCount; SceKernelSysClock runClocks; SceUInt intrPreemptCount;
            SceUInt threadPreemptCount; SceUInt releaseCount; } SceKernelThreadRunStatus;
        int sceKernelReferThreadRunStatus(SceUID thid, SceKernelThreadRunStatus *status);
        long long sceKernelGetSystemTimeWide(void);
        """
    ),
    "fileio.hpp": "#pragma once\nnamespace th08::psp { void BootLog(const char *fmt, ...); }\n",
    "pspge.h": "#pragma once\ntypedef struct PspGeListArgs PspGeListArgs;\nenum PspGeListState { PSP_GE_LIST_DONE = 0, PSP_GE_LIST_QUEUED, PSP_GE_LIST_DRAWING_DONE, PSP_GE_LIST_STALL_REACHED, PSP_GE_LIST_CANCEL_DONE };\nint sceGeListEnQueue(const void *list, void *stall, int cbid, PspGeListArgs *arg);\nint sceGeListSync(int qid, int syncType);\n",
}


class PspPerfEnvTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.makefile = read("Makefile.psp")
        cls.header = read("psp/perf_env.hpp")
        cls.probe = read("psp/perf_env.cpp")
        cls.perf = read("psp/perf_attribution.cpp")
        cls.draw = read("psp/draw_priority_subprofile.cpp")
        cls.draw_hpp = read("psp/draw_priority_subprofile.hpp")
        cls.global_cpp = read("src/Global.cpp")
        cls.psp_main = read("psp/main.cpp")

    def test_makefile_and_fingerprint(self) -> None:
        self.assertIn(f"{SWITCH} ?= 0", self.makefile)
        self.assertIn(f"$(error {SWITCH}=1 requires TH08_PSP_PERF_ATTRIBUTION=1)", self.makefile)
        self.assertIn(f"$(error {SWITCH} must be 0 or 1)", self.makefile)
        self.assertIn("PSP_PERF_ENV_SRCS := psp/perf_env.cpp", self.makefile)
        self.assertIn("$(PSP_PERF_ENV_SRCS)", self.makefile)
        self.assertIn(f"CXXFLAGS += -D{SWITCH}=1", self.makefile)
        self.assertIn("ifneq ($(TH08_PSP_PERF_ENV)$(TH08_PSP_SWAP_TRIPLE),00)\n# GE queue observer / triple-buffer flip poll: every PSPGL list submission.\nLDFLAGS += -Wl,--wrap=sceGeListEnQueue\nendif", self.makefile)
        self.assertIn("PSP_GE_LIST_HOOK_SRCS := psp/ge_list_hook.cpp", self.makefile)
        self.assertIn("psp/perf_env.o psp/perf_attribution.o psp/draw_priority_subprofile.o psp/main.o: \\\n\t$(PERF_ENV_CONFIG_STAMP)", self.makefile)
        for stamp in ("perf-env-0.stamp", "perf-env-1.stamp"):
            self.assertIn(stamp, self.makefile)
        self.assertIn('"PERF_ENV=%d "', self.psp_main)
        self.assertIn("#define TH08_PSP_FEATURE_PERF_ENV 1", self.psp_main)
        self.assertIn("        TH08_PSP_FEATURE_PERF_ENV,", self.psp_main)

    def test_attribution_hooks(self) -> None:
        start = function_body(self.perf, "void StartWindow")
        self.assertIn("#if TH08_PSP_PERF_ENV_ENABLED\n    PerfEnvWindowStart(gWindowActive);\n#endif", start)
        emit = function_body(self.perf, "void EmitWindow(")
        self.assertIn("PerfEnvEmitWindow(gStage, gBaselineStageFrame, stageFrame, wallUs);", emit)
        self.assertEqual(len(re.findall(r"(?<!Flush)BootLog\(", emit)), 1)
        after = function_body(self.perf, "void PerfAttributionAfterPresent(")
        self.assertIn("PerfEnvCancelWindow();", after)
        init = function_body(self.perf, "void PerfAttributionInitialize()")
        self.assertIn("PerfEnvInitialize(gMainThreadId);", init)
        # Unscoped waits never enter the phase totals; they are gated on the
        # None wait context and the main thread, and timed with the same clock.
        gate = function_body(self.perf, "bool BeginUnscopedGeWait(")
        self.assertIn("gWaitContext != PerfAttributionWaitContext::None", gate)
        self.assertIn("sceKernelGetThreadId() != gMainThreadId", gate)
        wrapper = function_body(self.perf, "extern \"C\" int __wrap_sceGeListSync(")
        self.assertIn("!measured && th08::psp::BeginUnscopedGeWait(startUs)", wrapper)
        self.assertIn("PerfEnvNoteUnscopedGeWait(th08::psp::gPerfEnvCurrentPhase,", wrapper)
        self.assertIn("DrawPrioritySubprofileNoteGeWait(durationUs);", wrapper)
        self.assertEqual(wrapper.count("__real_sceGeListSync(qid, syncType)"), 1)
        # The swap's GE wait feeds the GE queue observer (certain busy time).
        self.assertIn("if (phase == th08::psp::PerfAttributionPhase::GeWaitSwap)\n            th08::psp::PerfEnvNoteGeSwapWait(", wrapper)
        # Frame end right after the swap, before the flip bookkeeping.
        compat = read("src/modern/linux/d3d8_compat.cpp")
        present = function_body(compat, "HRESULT Present(const RECT *, const RECT *, HWND, const RGNDATA *)")
        swap = present.index("SDL_GL_SwapWindow(window);")
        self.assertIn("th08::psp::PerfEnvNoteGeFrameEnd(", present[swap:swap + 400])
        # Scope tracks the innermost phase for the environment record.
        ctor = function_body(self.perf, "PerfAttributionScope::PerfAttributionScope(")
        self.assertIn("gPerfEnvCurrentPhase = static_cast<std::uint8_t>(phase);", ctor)
        dtor = function_body(self.perf, "PerfAttributionScope::~PerfAttributionScope()")
        self.assertIn("gPerfEnvCurrentPhase = previousPhase_;", dtor)

    def test_main_loop_slots(self) -> None:
        loop = read("src/main.cpp")
        for call in ("g_SoundPlayer.ProcessQueues();", "th08::psp::MemoryTelemetrySampleGameFrame();", "Sleep(0);"):
            self.assertEqual(loop.count(call), 2, call)  # timed and untimed variants
        for slot in ("PerfEnvNoteMain(0U,", "PerfEnvNoteMain(1U,", "PerfEnvNoteMain(2U,", "PerfEnvNoteMain(3U,",
                     "PerfEnvNoteMain(4U,", "PerfEnvNoteMain(5U,", "PerfEnvNoteMain(6U,", "PerfEnvNoteMain(7U,"):
            self.assertEqual(loop.count(slot), 1, slot)
        render = function_body(loop, "RenderResult GameWindow::Render()")
        # Slot 4 uses the previous exit time, recorded on the common return.
        self.assertIn("gPspRenderExitUs = static_cast<std::uint64_t>(sceKernelGetSystemTimeWide());", render)
        self.assertLess(render.index("PerfEnvNoteMain(4U,"), render.index("RunCalcChain()"))
        self.assertLess(render.index("Present();"), render.index("PerfEnvNoteMain(6U,"))

    def test_draw_priority_ge_wait_bins(self) -> None:
        self.assertIn("void DrawPrioritySubprofileNoteCallbackPriority(int priority);", self.draw_hpp)
        self.assertIn("void DrawPrioritySubprofileNoteGeWait(std::uint64_t durationUs);", self.draw_hpp)
        body = function_body(self.global_cpp, "int Chain::RunDrawChain()")
        snapshot = body.index("drawPriorityCallbackPriority = current->priority;")
        note = body.index("psp::DrawPrioritySubprofileNoteCallbackPriority(drawPriorityCallbackPriority);")
        leave = body.index("LeaveCriticalSectionWrapper(0)", snapshot)
        self.assertLess(snapshot, note)
        self.assertLess(note, leave)
        end = function_body(self.draw, "void DrawPrioritySubprofileEndDrawChain(")
        self.assertIn("gCurrentPriority = -1;", end)
        emit = function_body(self.draw, "void DrawPrioritySubprofileEmitGeWindow(")
        self.assertEqual(emit.count("BootLog("), 1)
        self.assertIn('"gwf=%lu gwc=%llu/%lu "', emit)

    def test_record_shape_and_host_compiles(self) -> None:
        for forbidden in ("malloc", "FlushBootLog"):
            self.assertNotIn(forbidden, self.probe)
        self.assertEqual(self.probe.count("BootLog("), 1)
        fmt = "".join(re.findall(r'"([^"]*)"', re.search(r'BootLog\(\s*((?:"[^"]*"\s*)+),', self.probe).group(1)))
        self.assertEqual(len(re.findall(r"%(?:0\d)?(?:ll|l)?[dusx]", fmt)), 19 + 24 + 8 + 23 * 3)
        self.assertIn('"gq=%lu/%llu/%lu/%llu/%lu/%llu/%llu/%lu "', self.probe)
        wrap = function_body(self.probe, "int PerfEnvGeListEnqueue(")
        self.assertIn("__real_sceGeListSync(gGeLastQid, 1)", wrap)
        self.assertEqual(wrap.count("__real_sceGeListEnQueue(list, stall, cbid, arg)"), 1)
        hook = read("psp/ge_list_hook.cpp")
        self.assertIn('extern "C" int __wrap_sceGeListEnQueue(', hook)
        self.assertIn("th08::psp::PerfEnvGeListEnqueue(list, stall, cbid, arg)", hook)
        self.assertNotIn("__wrap_sceGeListEnQueue", self.probe)
        self.assertEqual(function_body(self.probe, "void PerfEnvEmitWindow(").count("TH08_PERF_ENV_WAIT_ARGS("), 23 + 1)
        with tempfile.TemporaryDirectory() as tmp:
            d = pathlib.Path(tmp)
            for name, text in SDK_STUBS.items():
                (d / name).write_text(text, encoding="utf-8")
            base = ["g++", "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", "-I", str(d), "-I", str(ROOT / "psp"), "-I", str(ROOT)]
            subprocess.run(base + ["-DPSP=1", f"-D{SWITCH}=1", "-c", str(ROOT / "psp" / "perf_env.cpp"), "-o", str(d / "pe.o")], cwd=ROOT, check=True, capture_output=True, text=True)
            (d / "off.cpp").write_text(f'#define PSP 1\n#include "perf_env.hpp"\n#if TH08_PSP_PERF_ENV_ENABLED\n#error unexpectedly enabled\n#endif\nint main() {{ return 0; }}\n', encoding="utf-8")
            subprocess.run(base + ["-fsyntax-only", str(d / "off.cpp")], cwd=ROOT, check=True, capture_output=True, text=True)


if __name__ == "__main__":
    unittest.main()
