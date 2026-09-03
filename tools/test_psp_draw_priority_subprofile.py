#!/usr/bin/env python3
"""Host and source-contract tests for the PSP draw-priority subprofiler."""

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


class PspDrawPrioritySubprofileTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.makefile = read("Makefile.psp")
        cls.header = read("psp/draw_priority_subprofile.hpp")
        cls.math = read("psp/draw_priority_subprofile_math.hpp")
        cls.probe = read("psp/draw_priority_subprofile.cpp")
        cls.global_cpp = read("src/Global.cpp")
        cls.global_hpp = read("src/Global.hpp")
        cls.background = read("src/Background.cpp")
        cls.perf = read("psp/perf_attribution.cpp")
        cls.psp_main = read("psp/main.cpp")

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
            cwd=ROOT,
            check=True,
            capture_output=True,
            text=True,
        )

    def test_default_off_stamp_dependency_and_fingerprint(self) -> None:
        feature = "TH08_PSP_DRAW_PRIORITY_SUBPROFILE"
        self.assertIn(f"{feature} ?= 0", self.makefile)
        self.assertIn(
            f"$(error {feature}=1 requires TH08_PSP_PERF_ATTRIBUTION=1)",
            self.makefile,
        )
        self.assertIn(
            "PSP_DRAW_PRIORITY_SUBPROFILE_SRCS := "
            "psp/draw_priority_subprofile.cpp",
            self.makefile,
        )
        self.assertIn("$(PSP_DRAW_PRIORITY_SUBPROFILE_SRCS)", self.makefile)
        self.assertIn("draw-priority-subprofile-0.stamp", self.makefile)
        self.assertIn("draw-priority-subprofile-1.stamp", self.makefile)
        self.assertIn(f"CXXFLAGS += -D{feature}=1", self.makefile)
        self.assertNotIn(f"-D{feature}=$({feature})", self.makefile)
        for consumer in (
            "src/Global.o",
            "src/Background.o",
            "psp/draw_priority_subprofile.o",
            "psp/perf_attribution.o",
            "psp/main.o",
        ):
            self.assertIn(consumer, self.makefile)

        self.assertIn(
            "#define TH08_PSP_FEATURE_DRAW_PRIORITY_SUBPROFILE 1",
            self.psp_main,
        )
        self.assertIn(
            "#define TH08_PSP_FEATURE_DRAW_PRIORITY_SUBPROFILE 0",
            self.psp_main,
        )
        self.assertIn("DRAW_PRIORITY_SUBPROFILE=%d", self.psp_main)
        self.assertIn("TH08_PSP_FEATURE_DRAW_PRIORITY_SUBPROFILE,", self.psp_main)

        off_unit = textwrap.dedent(
            """
            #define PSP 1
            #include "psp/draw_priority_subprofile.hpp"
            #if TH08_PSP_DRAW_PRIORITY_SUBPROFILE_ENABLED
            #error draw-priority probe unexpectedly enabled
            #endif
            int main() { return TH08_PSP_DRAW_PRIORITY_SUBPROFILE_ENABLED; }
            """
        )
        with tempfile.TemporaryDirectory() as directory:
            exe = pathlib.Path(directory) / "off"
            self.compile_host(off_unit, exe)
            symbols = subprocess.run(
                ["nm", "-C", str(exe)],
                cwd=ROOT,
                check=True,
                capture_output=True,
                text=True,
            ).stdout
            self.assertNotIn("DrawPrioritySubprofile", symbols)
            subprocess.run([str(exe)], check=True)

    def test_private_sample_gate_is_exact_one_in_sixteen_and_not_48_locked(self) -> None:
        harness = textwrap.dedent(
            r"""
            #include "psp/draw_priority_subprofile_math.hpp"
            #include <cstdint>
            using namespace th08::psp;
            static_assert(DrawPriorityBinFor(0) == 0);
            static_assert(DrawPriorityBinFor(21) == 21);
            static_assert(DrawPriorityBinFor(-1) == kDrawPriorityOtherBin);
            static_assert(DrawPriorityBinFor(22) == kDrawPriorityOtherBin);
            static_assert(DrawPrioritySubtract(137, 130).value == 7);
            static_assert(!DrawPrioritySubtract(137, 130).underflow);
            static_assert(DrawPrioritySubtract(5, 6).value == 0);
            static_assert(DrawPrioritySubtract(5, 6).underflow);
            int main() {
                bool phaseSeen[48]{};
                for (std::uint64_t block = 0; block < 4096; ++block) {
                    unsigned int selected = 0;
                    for (std::uint64_t slot = 0; slot < 16; ++slot) {
                        const std::uint64_t ordinal = block * 16 + slot;
                        if (DrawPriorityShouldSampleOrdinal(ordinal)) {
                            ++selected;
                            phaseSeen[ordinal % 48] = true;
                        }
                    }
                    if (selected != 1) return 1;
                }
                for (bool seen : phaseSeen) if (!seen) return 2;

                DrawPriorityDurationStat stat{};
                if (DrawPriorityAccumulate(stat, 7)) return 3;
                if (DrawPriorityAccumulate(stat, 11)) return 4;
                if (stat.totalUs != 18 || stat.maxUs != 11 || stat.calls != 2)
                    return 5;
                return 0;
            }
            """
        )
        with tempfile.TemporaryDirectory() as directory:
            exe = pathlib.Path(directory) / "gate"
            self.compile_host(harness, exe)
            subprocess.run([str(exe)], check=True)

    def test_callback_timer_is_exclusive_and_execute_again_is_a_new_call(self) -> None:
        body = function_body(self.global_cpp, "int Chain::RunDrawChain()")
        self.assertEqual(body.count("DrawPrioritySubprofileBeginDrawChain"), 1)
        self.assertEqual(body.count("current->callback(current->arg)"), 1)
        self.assertIn("execute_again:", body)
        self.assertIn("goto execute_again;", body)

        label = body.index("execute_again:")
        snapshot = body.index("current->priority", label)
        leave = body.index("LeaveCriticalSectionWrapper(0)", snapshot)
        begin = body.index("DrawPrioritySubprofileReadClock()", leave)
        callback = body.index("current->callback(current->arg)", begin)
        record = body.index("DrawPrioritySubprofileRecordCallback", callback)
        enter = body.index("EnterCriticalSectionWrapper(0)", record)
        self.assertLess(snapshot, leave)
        self.assertLess(leave, begin)
        self.assertLess(begin, callback)
        self.assertLess(callback, record)
        self.assertLess(record, enter)

        loop_exit = body.index("loop_exit:")
        final_leave = body.index("LeaveCriticalSectionWrapper(0)", loop_exit)
        chain_end = body.index("DrawPrioritySubprofileEndDrawChain", final_leave)
        self.assertLess(final_leave, chain_end)
        self.assertNotIn("DrawPrioritySubprofile", self.global_hpp)

    def test_background_nested_scope_and_parent_window_are_identical(self) -> None:
        body = function_body(
            self.background,
            "ChainCallbackResult Background::OnDrawLowPrio(Background *background)",
        )
        nested_begin = body.index("DrawPrioritySubprofileBeginEffectBackground")
        canonical = body.index("g_EffectManager.DrawBackgroundEffects()")
        nested_end = body.index("DrawPrioritySubprofileEndEffectBackground")
        self.assertLess(nested_begin, canonical)
        self.assertLess(canonical, nested_end)
        self.assertEqual(body.count("g_EffectManager.DrawBackgroundEffects()"), 1)

        start = function_body(self.perf, "void StartWindow")
        emit = function_body(self.perf, "void EmitWindow")
        after = function_body(self.perf, "void PerfAttributionAfterPresent")
        self.assertIn("DrawPrioritySubprofileResetWindow(gWindowActive)", start)
        self.assertIn("DrawPrioritySubprofileEmitWindow", emit)
        self.assertIn("gBaselineStageFrame", emit)
        self.assertIn("gDraws", emit)
        self.assertIn("DrawPrioritySubprofileCancelWindow", after)

    def test_fixed_storage_buffered_report_and_exact_runtime_accounting(self) -> None:
        self.assertIn(
            "DrawPriorityDurationStat gPriorityStats[kDrawPriorityBinCount]{}",
            self.probe,
        )
        for forbidden in ("malloc(", "calloc(", "realloc(", "new ", "std::vector"):
            self.assertNotIn(forbidden, self.probe)
        for forbidden in ("g_Rng", "GetRandom", "rand(", "srand("):
            self.assertNotIn(forbidden, self.probe)
        emit = function_body(self.probe, "void DrawPrioritySubprofileEmitWindow")
        self.assertEqual(emit.count("BootLog("), 1)
        self.assertNotIn("FlushBootLog", emit)
        self.assertIn("dispatchResidual", emit)
        self.assertIn("priority7Exclusive", emit)
        for priority in range(22):
            self.assertIn(f"p{priority}=", emit)
        self.assertIn("po=", emit)

        harness = textwrap.dedent(
            r"""
            #include <cstdarg>
            #include <cstdint>
            #include <cstdio>
            #include <cstring>
            static std::uint64_t fakeNow;
            static char lastLog[4096];
            extern "C" std::uint64_t sceKernelGetSystemTimeWide() {
                return fakeNow;
            }
            #define PSP 1
            #define TH08_PSP_PERF_ATTRIBUTION 1
            #define TH08_PSP_DRAW_PRIORITY_SUBPROFILE 1
            #include "psp/draw_priority_subprofile.cpp"
            namespace th08::psp {
            void BootLog(const char *format, ...) {
                va_list arguments;
                va_start(arguments, format);
                std::vsnprintf(lastLog, sizeof(lastLog), format, arguments);
                va_end(arguments);
            }
            }
            int main() {
                using namespace th08::psp;
                DrawPrioritySubprofileResetWindow(true);
                unsigned int sampled = 0;
                for (unsigned int frame = 0; frame < 16; ++frame) {
                    std::uint64_t chainStart = 0;
                    if (!DrawPrioritySubprofileBeginDrawChain(chainStart))
                        continue;
                    ++sampled;

                    std::uint64_t callbackStart =
                        DrawPrioritySubprofileReadClock();
                    fakeNow += 10;
                    std::uint64_t nestedStart = 0;
                    if (!DrawPrioritySubprofileBeginEffectBackground(nestedStart))
                        return 1;
                    fakeNow += 30;
                    DrawPrioritySubprofileEndEffectBackground(nestedStart);
                    fakeNow += 60;
                    DrawPrioritySubprofileRecordCallback(7, callbackStart);

                    callbackStart = DrawPrioritySubprofileReadClock();
                    fakeNow += 25;
                    DrawPrioritySubprofileRecordCallback(7, callbackStart);

                    callbackStart = DrawPrioritySubprofileReadClock();
                    fakeNow += 5;
                    DrawPrioritySubprofileRecordCallback(99, callbackStart);

                    fakeNow += 7;
                    DrawPrioritySubprofileEndDrawChain(chainStart);
                }
                if (sampled != 1) return 2;
                DrawPrioritySubprofileEmitWindow(4, 1, 601, 200, 2);
                if (!std::strstr(lastLog, "DRAW_PRIO V1 st=4 sf=1-601")) return 3;
                if (!std::strstr(lastLog, "presented=200 cadence_mode=2")) return 4;
                if (!std::strstr(lastLog, "sample_rule=presented_ordinal_private_block_hash_1of16")) return 12;
                if (!std::strstr(lastLog, "ordinal_end=16")) return 13;
                if (!std::strstr(lastLog, "sampled=1 chain=137/137/1")) return 5;
                if (!std::strstr(lastLog, "cb=130/3 residual=7")) return 6;
                if (!std::strstr(lastLog, "effect_bg=30/30/1 p7x=95")) return 7;
                if (!std::strstr(lastLog, "timer_reads=10 cr=0 ov=0 uf=0x00")) return 8;
                if (!std::strstr(lastLog, "p7=125/100/2")) return 9;
                if (!std::strstr(lastLog, "po=5/5/1")) return 10;
                if (std::strlen(lastLog) >= sizeof(lastLog) - 1) return 11;
                return 0;
            }
            """
        )
        with tempfile.TemporaryDirectory() as directory:
            exe = pathlib.Path(directory) / "accounting"
            self.compile_host(harness, exe)
            subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    unittest.main(verbosity=2)
