#!/usr/bin/env python3
"""Host and source-contract tests for the ZunMath trig double-float fast paths."""

from __future__ import annotations

import pathlib
import re
import subprocess
import tempfile
import textwrap
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
AUDIT = "TH08_PSP_TRIG_DF_FASTPATH_AUDIT"
PRODUCT = "TH08_PSP_TRIG_DF_FASTPATH"


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
            if stack:
                stack[-1] = any(m in stripped for m in macros) if stripped.startswith("#elif") else False
        if needle in line and not stripped.startswith("#") and not any(stack):
            offenders.append(number)
    return offenders


# Host harness: the product wrappers must equal the canonical binary64 helpers
# bit for bit whenever they accept, across every ZunMath entry point.
HARNESS = r'''
#define TH08_MODERN_PORT 1
#define PSP 1
#define TH08_PSP_TRIG_DF_FASTPATH 1
#include "ZunMath.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
using namespace th08;
static unsigned bits(float v) { unsigned b; std::memcpy(&b, &v, 4); return b; }
static float canonCosMul(float a, float m) { return (float)(cos((double)a) * (double)m); }
static float canonSinMul(float a, float m) { return (float)(sin((double)a) * (double)m); }
int main() {
    std::mt19937 rng(41u);
    std::uniform_real_distribution<float> ang(-4.2f, 4.2f), mag(-300.0f, 300.0f), coord(-512.0f, 512.0f);
    unsigned long long n = 0, bad = 0;
    for (int i = 0; i < 400000; ++i) {
        const float a = ang(rng), m = (i % 7 == 0) ? 1.0f : mag(rng), vx = mag(rng), vy = mag(rng);
        Float3 v; v.FromAngleMagnitude(a, m);
        if (bits(v.x) != bits(canonCosMul(a, m)) || bits(v.y) != bits(canonSinMul(a, m))) ++bad;
        Float3 w; w.FromRotatedVec2(a, vx, vy);
        if (bits(w.x) != bits(canonCosMul(a, vx)) || bits(w.y) != bits(canonSinMul(a, vy))) ++bad;
        if (bits(X87CompatibleSinMul(a, m)) != bits(canonSinMul(a, m))) ++bad;
        if (bits(X87CompatibleCosMul(a, m)) != bits(canonCosMul(a, m))) ++bad;
        if (bits(X87CompatibleSin(a)) != bits((float)sin((double)a))) ++bad;
        if (bits(X87CompatibleCos(a)) != bits((float)cos((double)a))) ++bad;
        const float y = coord(rng), x = coord(rng);
        if (bits(X87CompatibleAtan2(y, x)) != bits((float)atan2((double)y, (double)x))) ++bad;
        n += 7;
    }
    const float specials[] = {0.0f, -0.0f, 1e-30f, -1e-30f, 3.4e38f, 1.0f/0.0f, 0.0f/0.0f, 65536.0f};
    for (float y : specials) for (float x : specials) {
        if (bits(X87CompatibleAtan2(y, x)) != bits((float)atan2((double)y, (double)x))) ++bad;
        Float3 v; v.FromAngleMagnitude(y, x);
        if (bits(v.x) != bits(canonCosMul(y, x)) || bits(v.y) != bits(canonSinMul(y, x))) ++bad;
        n += 2;
    }
    std::printf("checks=%llu mismatches=%llu\n", n, bad);
    return bad == 0 ? 0 : 1;
}
'''

# Product-mode host stand-ins for the PSP-only observer dependencies.
STUBS = {
    "fileio.hpp": "#pragma once\nnamespace th08::psp { void BootLog(const char *fmt, ...); }\n",
    "x87_trig_cache.hpp": "#pragma once\n",
    "inttypes.hpp": "#pragma once\n#include <cstdint>\ntypedef float f32; typedef double f64; typedef std::int32_t i32; typedef std::uint32_t u32;\n",
}
STUB_CPP = "#include <cstdarg>\n#include <cstdio>\nnamespace th08::psp { void BootLog(const char *fmt, ...) { va_list a; va_start(a, fmt); std::vfprintf(stderr, fmt, a); va_end(a); } }\n"


class PspTrigDfFastpathTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.makefile = read("Makefile.psp")
        cls.header = read("psp/trig_df_fastpath.hpp")
        cls.probe = read("psp/trig_df_fastpath.cpp")
        cls.zunmath = read("src/ZunMath.hpp")
        cls.perf = read("psp/perf_attribution.cpp")
        cls.psp_main = read("psp/main.cpp")

    def _compile(self, directory: pathlib.Path, defines: list[str], sources: list[pathlib.Path], output: pathlib.Path, syntax_only: bool = False) -> None:
        for name, text in STUBS.items():
            (directory / name).write_text(text, encoding="utf-8")
        cmd = ["g++", "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", "-ffp-contract=off", "-I", str(directory), "-I", str(ROOT / "psp"), "-I", str(ROOT)]
        cmd += defines
        if syntax_only:
            cmd += ["-fsyntax-only"] + [str(s) for s in sources]
        else:
            cmd += [str(s) for s in sources] + ["-o", str(output)]
        subprocess.run(cmd, cwd=ROOT, check=True, capture_output=True, text=True)

    def test_host_bit_exactness_through_zunmath(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            d = pathlib.Path(tmp)
            (d / "h.cpp").write_text(HARNESS, encoding="utf-8")
            (d / "stub.cpp").write_text(STUB_CPP, encoding="utf-8")
            # ZunMath.hpp lives in src/, but src/inttypes.hpp is a Win32 header: copy
            # ZunMath.hpp next to the stub inttypes.hpp so the stub wins the include search.
            (d / "ZunMath.hpp").write_text(self.zunmath, encoding="utf-8")
            self._compile(d, ["-DPSP=1", f"-D{PRODUCT}=1", "-DTH08_MODERN_PORT=1"], [d / "h.cpp", ROOT / "psp" / "trig_df_fastpath.cpp", d / "stub.cpp"], d / "h")
            result = subprocess.run([str(d / "h")], cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("mismatches=0", result.stdout)

    def test_default_off_and_both_modes_compile(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            d = pathlib.Path(tmp)
            (d / "ZunMath.hpp").write_text(self.zunmath, encoding="utf-8")
            off = textwrap.dedent(
                """
                #define TH08_MODERN_PORT 1
                #define PSP 1
                #include "ZunMath.hpp"
                #if TH08_PSP_TRIG_DF_STATS_ENABLED || TH08_PSP_TRIG_DF_FASTPATH_ENABLED || TH08_PSP_TRIG_DF_FASTPATH_AUDIT_ENABLED
                #error trig fast-path switches unexpectedly enabled
                #endif
                int main() { th08::Float3 v; v.FromAngleMagnitude(0.5f, 2.0f); return 0; }
                """
            )
            (d / "off.cpp").write_text(off, encoding="utf-8")
            self._compile(d, [], [d / "off.cpp"], d / "off", syntax_only=True)
            for define in (f"-D{AUDIT}=1", f"-D{PRODUCT}=1"):
                (d / "probe.cpp").write_text('#include "trig_df_fastpath.cpp"\n', encoding="utf-8")
                self._compile(d, ["-DPSP=1", define, "-c"], [ROOT / "psp" / "trig_df_fastpath.cpp"], d / "p.o")
                (d / "use.cpp").write_text('#define TH08_MODERN_PORT 1\n#define PSP 1\n#include "ZunMath.hpp"\nint main() { th08::Float3 v; v.FromRotatedVec2(0.5f, 2.0f, 3.0f); return (int)th08::X87CompatibleAtan2(v.x, v.y); }\n', encoding="utf-8")
                self._compile(d, ["-DPSP=1", define], [d / "use.cpp"], d / "u", syntax_only=True)

    def test_zunmath_hooks_and_canonical_paths(self) -> None:
        z = self.zunmath
        self.assertIn('#if defined(PSP)\n#include "trig_df_fastpath.hpp"\n#endif', z)
        macros = ("TH08_PSP_TRIG_DF_FASTPATH_ENABLED", "TH08_PSP_TRIG_DF_FASTPATH_AUDIT_ENABLED")
        self.assertEqual(unguarded_lines(z, "TrigDf", macros), [])
        # The canonical single-rounding expressions stay in place for the OFF build.
        for canon in ("    return static_cast<f32>(X87CompatibleSin64(angle));",
                      "    return static_cast<f32>(X87CompatibleCos64(angle));",
                      "        X87CompatibleSin64(angle) * static_cast<f64>(magnitude));",
                      "        X87CompatibleCos64(angle) * static_cast<f64>(magnitude));",
                      "        atan2(static_cast<f64>(y), static_cast<f64>(x)));",
                      "        this->x = X87CompatibleCosMul(angle, magnitude);",
                      "        this->y = X87CompatibleSinMul(angle, magnitude);",
                      "        this->x = X87CompatibleCosMul(angle, vecX);",
                      "        this->y = X87CompatibleSinMul(angle, vecY);"):
            self.assertIn(canon, z, canon)
        # MulAdd / MulInt keep their distinct rounding boundary: no fast path there.
        for fn in ("inline f32 X87CompatibleSinMulAdd(", "inline f32 X87CompatibleCosMulAdd(", "inline f32 X87CompatibleSinMulInt(", "inline f32 X87CompatibleCosMulInt(", "inline f32 X87CompatibleMulSub(", "inline f32 X87CompatibleMulAdd("):
            self.assertNotIn("TrigDf", function_body(z, fn), fn)
        # Every hooked site names its TrigDfSite.
        for site in ("SinCosUnit", "SinCosMul", "FromAngleMagnitude", "FromRotatedVec2"):
            self.assertGreaterEqual(z.count(f"psp::TrigDfSite::{site}"), 2, site)
        self.assertEqual(z.count("psp::TrigDfAtan2(y, x, &angle)"), 1)
        self.assertEqual(z.count("psp::TrigDfAuditAtan2(y, x, canonicalAngle)"), 1)

    def test_makefile_perf_and_fingerprint(self) -> None:
        for feature in (AUDIT, PRODUCT):
            self.assertIn(f"{feature} ?= 0", self.makefile)
            self.assertIn(f"$(error {feature} must be 0 or 1)", self.makefile)
            self.assertIn(f"CXXFLAGS += -D{feature}=1", self.makefile)
        self.assertIn(f"$(error {AUDIT} and {PRODUCT} are mutually exclusive)", self.makefile)
        self.assertIn(f"$(error {AUDIT}=1 requires TH08_PSP_PERF_ATTRIBUTION=1)", self.makefile)
        self.assertIn("PSP_TRIG_DF_SRCS := psp/trig_df_fastpath.cpp", self.makefile)
        self.assertIn("$(PSP_TRIG_DF_SRCS)", self.makefile)
        self.assertIn("psp/trig_df_fastpath.o: CXXFLAGS += -ffp-contract=off", self.makefile)
        self.assertIn("$(filter src/%.o,$(OBJS)) psp/trig_df_fastpath.o psp/perf_attribution.o psp/main.o: \\\n\t$(TRIG_DF_FASTPATH_AUDIT_CONFIG_STAMP) $(TRIG_DF_FASTPATH_CONFIG_STAMP)", self.makefile)
        for stamp in ("trig-df-fastpath-audit-0.stamp", "trig-df-fastpath-audit-1.stamp", "trig-df-fastpath-0.stamp", "trig-df-fastpath-1.stamp"):
            self.assertIn(stamp, self.makefile)
        for hook in ("TrigDfStatsResetWindow(gWindowActive);", "TrigDfStatsEmitWindow(gStage, gBaselineStageFrame, stageFrame);", "TrigDfStatsCancelWindow();"):
            self.assertIn(hook, self.perf)
        self.assertEqual(unguarded_lines(self.perf, "TrigDfStats", ("TH08_PSP_TRIG_DF_STATS_ENABLED",)), [])
        self.assertIn('"TRIG_DF_FASTPATH_AUDIT=%d TRIG_DF_FASTPATH=%d "', self.psp_main)
        for macro in ("TH08_PSP_FEATURE_TRIG_DF_FASTPATH_AUDIT", "TH08_PSP_FEATURE_TRIG_DF_FASTPATH"):
            self.assertIn(f"#define {macro} 1", self.psp_main)
            self.assertIn(f"        {macro},", self.psp_main)
        # Observer: bounded, timer-free record with the documented field count.
        for forbidden in ("sceKernelGetSystemTimeWide", "malloc", "FlushBootLog"):
            self.assertNotIn(forbidden, self.probe)
        self.assertEqual(self.probe.count("BootLog("), 1)
        fmt = "".join(re.findall(r'"([^"]*)"', re.search(r'BootLog\(\s*((?:"[^"]*"\s*)+),', self.probe).group(1)))
        self.assertEqual(len(re.findall(r"%(?:0\d)?(?:ll|l)?[dusx]", fmt)), 44)
        self.assertEqual(function_body(self.probe, "void TrigDfStatsEmitWindow(").count("static_cast<"), 43)


if __name__ == "__main__":
    unittest.main()
