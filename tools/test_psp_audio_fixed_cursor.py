#!/usr/bin/env python3
"""Host and source-contract tests for the SC audio mixer fixed-point cursor."""

from __future__ import annotations

import pathlib
import re
import subprocess
import tempfile
import textwrap
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
AUDIT = "TH08_PSP_AUDIO_FIXED_CURSOR_AUDIT"
PRODUCT = "TH08_PSP_AUDIO_FIXED_CURSOR"


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


def unguarded_lines(source: str, needle: str, macros: tuple[str, ...]) -> list[int]:
    stack: list[bool] = []
    offenders: list[int] = []
    for number, line in enumerate(source.splitlines(), start=1):
        stripped = line.strip()
        if stripped.startswith("#if"):
            stack.append(any(m in stripped for m in macros))
        elif stripped.startswith("#endif"):
            if stack:
                stack.pop()
        elif stripped.startswith("#else") or stripped.startswith("#elif"):
            if stack and stack[-1]:
                stack[-1] = False
        if needle in line and not stripped.startswith("#") and not any(stack):
            offenders.append(number)
    return offenders


HARNESS = r'''
#include "psp/audio_fixed_cursor_math.hpp"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
using namespace th08::psp;
struct Canon { double cursor; bool playing; };
static bool canon_step(Canon &c, double step, std::uint32_t sourceFrames, bool looping, std::uint32_t *sourceFrame, bool *wrapped) {
    std::uint32_t sf = static_cast<std::uint32_t>(c.cursor);
    if (sf >= sourceFrames) {
        if (!looping) { c.playing = false; return false; }
        c.cursor -= sourceFrames; sf = static_cast<std::uint32_t>(c.cursor); *wrapped = true;
    }
    *sourceFrame = sf; c.cursor += step; return true;
}
int main() {
    std::mt19937_64 rng(20260903ULL);
    const unsigned rates[] = {44100u, 22050u, 11025u, 32000u, 48000u, 8000u, 5512u};
    unsigned long long frames = 0, mismatches = 0, cursorMismatch = 0, ineligible = 0;
    for (int scenario = 0; scenario < 2000; ++scenario) {
        const unsigned rate = rates[rng() % 7]; unsigned shift = 0;
        if (!AudioFixedCursorEligible(rate, &shift)) { ++ineligible; if (rate == 44100u || rate == 22050u || rate == 11025u) return 10; continue; }
        if ((rate == 44100u && shift != 0u) || (rate == 22050u && shift != 1u) || (rate == 11025u && shift != 2u)) return 11;
        const double step = static_cast<double>(rate) / 44100.0;
        const std::uint32_t sourceFrames = 1u + static_cast<std::uint32_t>(rng() % 200000u);
        const bool looping = (rng() & 1u) != 0u;
        Canon c{ static_cast<double>(rng() % sourceFrames), true };
        if (rng() & 1u) c.cursor += static_cast<double>(rng() % (1u << shift)) * step;
        for (int mix = 0; mix < 30 && c.playing; ++mix) {
            if ((rng() % 13u) == 0u) c.cursor = static_cast<double>(rng() % sourceFrames);
            const int outputFrames = 1 + static_cast<int>(rng() % 2048u);
            std::uint64_t fixed = 0; if (!AudioFixedCursorFromDouble(c.cursor, shift, &fixed)) return 12;
            bool wrappedC = false, wrappedF = false;
            for (int f = 0; f < outputFrames && c.playing; ++f) {
                std::uint32_t sfC = 0, sfF = 0;
                const bool okC = canon_step(c, step, sourceFrames, looping, &sfC, &wrappedC);
                const bool okF = AudioFixedCursorStep(&fixed, shift, sourceFrames, looping, &sfF, &wrappedF);
                if (okC != okF) { ++mismatches; break; }
                if (!okC) break;
                ++frames; if (sfC != sfF) ++mismatches;
            }
            if (wrappedC != wrappedF) ++mismatches;
            if (c.playing && AudioFixedCursorToDouble(fixed, shift) != c.cursor) ++cursorMismatch;
            if (c.cursor >= sourceFrames) { if (looping) { c.cursor = std::fmod(c.cursor, static_cast<double>(sourceFrames)); } else { c.cursor = sourceFrames; c.playing = false; } }
        }
    }
    std::uint64_t f2 = 0;
    if (AudioFixedCursorFromDouble(-1.0, 1u, &f2)) return 13;
    if (AudioFixedCursorFromDouble(0.25, 1u, &f2)) return 14;   // not a multiple of 2^-1
    if (!AudioFixedCursorFromDouble(0.25, 2u, &f2) || f2 != 1u) return 15;
    std::printf("frames=%llu mismatches=%llu cursor_mismatch=%llu ineligible=%llu\n", frames, mismatches, cursorMismatch, ineligible);
    return (mismatches == 0 && cursorMismatch == 0 && frames > 10000000ULL) ? 0 : 1;
}
'''


class PspAudioFixedCursorTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.makefile = read("Makefile.psp")
        cls.math = read("psp/audio_fixed_cursor_math.hpp")
        cls.header = read("psp/audio_cursor_audit.hpp")
        cls.probe = read("psp/audio_cursor_audit.cpp")
        cls.mixer = read("src/modern/linux/linux_compat.cpp")
        cls.perf = read("psp/perf_attribution.cpp")
        cls.psp_main = read("psp/main.cpp")

    def test_host_cursor_equivalence(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            d = pathlib.Path(directory)
            (d / "h.cpp").write_text(HARNESS, encoding="utf-8")
            subprocess.run(["g++", "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT), str(d / "h.cpp"), "-o", str(d / "h")], cwd=ROOT, check=True, capture_output=True, text=True)
            result = subprocess.run([str(d / "h")], cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("mismatches=0", result.stdout)

    def test_makefile_and_fingerprint(self) -> None:
        for feature in (AUDIT, PRODUCT):
            self.assertIn(f"{feature} ?= 0", self.makefile)
            self.assertIn(f"$(error {feature} must be 0 or 1)", self.makefile)
            self.assertIn(f"CXXFLAGS += -D{feature}=1", self.makefile)
        self.assertIn(f"$(error {AUDIT} and {PRODUCT} are mutually exclusive)", self.makefile)
        self.assertIn(f"$(error {AUDIT}=1 requires TH08_PSP_PERF_ATTRIBUTION=1)", self.makefile)
        self.assertIn("PSP_AUDIO_CURSOR_SRCS := psp/audio_cursor_audit.cpp", self.makefile)
        self.assertIn("$(PSP_AUDIO_CURSOR_SRCS)", self.makefile)
        for stamp in ("audio-fixed-cursor-audit-0.stamp", "audio-fixed-cursor-audit-1.stamp", "audio-fixed-cursor-0.stamp", "audio-fixed-cursor-1.stamp"):
            self.assertIn(stamp, self.makefile)
        self.assertIn("src/modern/linux/linux_compat.o psp/audio_cursor_audit.o psp/perf_attribution.o psp/main.o: \\\n\t$(AUDIO_FIXED_CURSOR_AUDIT_CONFIG_STAMP) $(AUDIO_FIXED_CURSOR_CONFIG_STAMP)", self.makefile)
        for macro in ("TH08_PSP_FEATURE_AUDIO_FIXED_CURSOR_AUDIT", "TH08_PSP_FEATURE_AUDIO_FIXED_CURSOR"):
            self.assertIn(f"#define {macro} 1", self.psp_main)
            self.assertIn(f"#define {macro} 0", self.psp_main)
            self.assertIn(f"        {macro},", self.psp_main)
        self.assertIn('"AUDIO_FIXED_CURSOR_AUDIT=%d AUDIO_FIXED_CURSOR=%d "', self.psp_main)
        self.assertIn('#error "AUDIO_FIXED_CURSOR audit and product switches are mutually exclusive"', self.header)

    def test_mixer_paths(self) -> None:
        mix = function_body(self.mixer, "void Mix(Sint16 *output, int outputFrames)")
        self.assertIn('#include "audio_cursor_audit.hpp"', self.mixer)
        # The canonical bookkeeping survives verbatim on the OFF path and on the ineligible product path.
        canon = "            DWORD sourceFrame = static_cast<DWORD>(cursorFrame);\n            if (sourceFrame >= sourceFrames)\n            {\n                if (!looping) { playing = false; break; }\n                cursorFrame -= sourceFrames; sourceFrame = static_cast<DWORD>(cursorFrame); wrapped = true;\n            }"
        self.assertIn("#else\n" + canon, mix)
        self.assertIn("                sourceFrame = static_cast<DWORD>(cursorFrame);\n                if (sourceFrame >= sourceFrames)\n                {\n                    if (!looping) { playing = false; break; }\n                    cursorFrame -= sourceFrames; sourceFrame = static_cast<DWORD>(cursorFrame); wrapped = true;\n                }", mix)
        self.assertIn("#else\n            cursorFrame += step;\n#endif", mix)
        self.assertIn("AudioFixedCursorEligible(format.nSamplesPerSec, &fixedShift)", mix)
        self.assertIn("AudioFixedCursorFromDouble(cursorFrame, fixedShift, &fixedCursor)", mix)
        self.assertIn("cursorFrame = th08::psp::AudioFixedCursorToDouble(fixedCursor, fixedShift);", mix)
        self.assertIn("AudioCursorAuditCompareFrame(fixedContinues, fixedFrame, sourceFrame);", mix)
        self.assertIn("AudioCursorAuditEndMix(", mix)
        # Sample math is untouched: the gain/mix/clamp lines remain exactly once.
        self.assertEqual(mix.count("int mixedLeft = output[frame * 2] + static_cast<int>(left * leftGain);"), 1)
        self.assertEqual(mix.count("if (mixedLeft < -32768) mixedLeft = -32768; else if (mixedLeft > 32767) mixedLeft = 32767;"), 1)
        macros = ("TH08_PSP_AUDIO_FIXED_CURSOR_ENABLED", "TH08_PSP_AUDIO_FIXED_CURSOR_AUDIT_ENABLED")
        for needle in ("fixedEligible", "fixedCursor", "fixedShift", "AudioCursorAudit", "AudioCursorProduct", "AudioFixedCursor"):
            self.assertEqual(unguarded_lines(self.mixer, needle, macros), [], needle)
        for hook in ("AudioCursorStatsResetWindow(gWindowActive);", "AudioCursorStatsEmitWindow(gStage, gBaselineStageFrame, stageFrame);", "AudioCursorStatsCancelWindow();"):
            self.assertIn(hook, self.perf)
        self.assertEqual(unguarded_lines(self.perf, "AudioCursorStats", ("TH08_PSP_AUDIO_CURSOR_STATS_ENABLED",)), [])
        # Observer: no timers/heap/logging per call; one bounded record.
        for forbidden in ("sceKernelGetSystemTimeWide", "malloc", "FlushBootLog"):
            self.assertNotIn(forbidden, self.probe)
        self.assertEqual(self.probe.count("BootLog("), 1)
        fmt = re.search(r'BootLog\(\s*((?:"[^"]*"\s*)+),', self.probe).group(1)
        fmt_text = "".join(re.findall(r'"([^"]*)"', fmt))
        self.assertEqual(len(re.findall(r"%(?:0\d)?(?:ll|l)?[dusx]", fmt_text)), 18)
        self.assertEqual(function_body(self.probe, "void AudioCursorStatsEmitWindow(").count("static_cast<"), 17)

    def test_default_off_and_host_compiles(self) -> None:
        off = textwrap.dedent(
            """
            #define PSP 1
            #include "psp/audio_cursor_audit.hpp"
            #if TH08_PSP_AUDIO_CURSOR_STATS_ENABLED || TH08_PSP_AUDIO_FIXED_CURSOR_ENABLED || TH08_PSP_AUDIO_FIXED_CURSOR_AUDIT_ENABLED
            #error audio cursor switches unexpectedly enabled
            #endif
            int main() { return 0; }
            """
        )
        with tempfile.TemporaryDirectory() as directory:
            d = pathlib.Path(directory)
            (d / "off.cpp").write_text(off, encoding="utf-8")
            subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT), str(d / "off.cpp"), "-o", str(d / "off")], cwd=ROOT, check=True, capture_output=True, text=True)
            (d / "fileio.hpp").write_text("#pragma once\nnamespace th08::psp { void BootLog(const char *fmt, ...); }\n", encoding="utf-8")
            for define in (f"-D{AUDIT}=1", f"-D{PRODUCT}=1"):
                subprocess.run(["g++", "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", "-DPSP=1", define, "-I", str(d), "-I", str(ROOT / "psp"), "-I", str(ROOT), "-c", str(ROOT / "psp" / "audio_cursor_audit.cpp"), "-o", str(d / "o.o")], cwd=ROOT, check=True, capture_output=True, text=True)


if __name__ == "__main__":
    unittest.main()
