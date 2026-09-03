#!/usr/bin/env python3
"""Source-contract tests for the default-off PSP soft-float census."""

from __future__ import annotations

import pathlib
import re
import subprocess
import tempfile
import textwrap
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
FEATURE = "TH08_PSP_SOFTFLOAT_CENSUS"


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


class PspSoftfloatCensusTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.makefile = read("Makefile.psp")
        cls.header = read("psp/softfloat_census.hpp")
        cls.probe = read("psp/softfloat_census.cpp")
        cls.perf = read("psp/perf_attribution.cpp")
        cls.perf_hpp = read("psp/perf_attribution.hpp")
        cls.psp_main = read("psp/main.cpp")

    def test_makefile_and_fingerprint(self) -> None:
        self.assertIn(f"{FEATURE} ?= 0", self.makefile)
        self.assertIn(f"$(error {FEATURE}=1 requires TH08_PSP_PERF_ATTRIBUTION=1)", self.makefile)
        self.assertIn("PSP_SOFTFLOAT_CENSUS_SRCS := psp/softfloat_census.cpp", self.makefile)
        self.assertIn("$(PSP_SOFTFLOAT_CENSUS_SRCS)", self.makefile)
        self.assertIn("softfloat-census-0.stamp", self.makefile)
        self.assertIn("softfloat-census-1.stamp", self.makefile)
        self.assertIn(f"CXXFLAGS += -D{FEATURE}=1", self.makefile)
        self.assertIn("SOFTFLOAT_CENSUS_WRAPS := __adddf3 __subdf3 __muldf3 __divdf3", self.makefile)
        self.assertIn("LDFLAGS += $(foreach sym,$(SOFTFLOAT_CENSUS_WRAPS),-Wl$(comma),--wrap=$(sym))".replace("$(comma),", "$(comma)"), self.makefile)
        self.assertIn("psp/softfloat_census.o psp/perf_attribution.o psp/main.o: \\\n\t$(SOFTFLOAT_CENSUS_CONFIG_STAMP)", self.makefile)
        self.assertIn("#define TH08_PSP_FEATURE_SOFTFLOAT_CENSUS 1", self.psp_main)
        self.assertIn("#define TH08_PSP_FEATURE_SOFTFLOAT_CENSUS 0", self.psp_main)
        self.assertIn('"SOFTFLOAT_CENSUS=%d "', self.psp_main)
        self.assertIn("        TH08_PSP_FEATURE_SOFTFLOAT_CENSUS,", self.psp_main)
        # Every wrapped symbol has a matching __wrap_ definition in the observer.
        wraps = re.search(r"SOFTFLOAT_CENSUS_WRAPS := (.*?)\nLDFLAGS", self.makefile, re.S).group(1)
        symbols = wraps.replace("\\\n", " ").split()
        self.assertGreater(len(symbols), 30)
        for sym in symbols:
            self.assertRegex(self.probe, r"\b(TH08_WRAP\w*\(" + re.escape(sym) + r",|" + re.escape(sym) + r"\))")

    def test_scope_tracks_innermost_phase_only_when_enabled(self) -> None:
        self.assertIn("std::uint8_t previousPhase_;", self.perf_hpp)
        ctor = function_body(self.perf, "PerfAttributionScope::PerfAttributionScope(PerfAttributionPhase phase)")
        dtor = function_body(self.perf, "PerfAttributionScope::~PerfAttributionScope()")
        self.assertIn("#if TH08_PSP_SOFTFLOAT_CENSUS_ENABLED\n    previousPhase_ = gSoftfloatCurrentPhase;\n    gSoftfloatCurrentPhase = static_cast<std::uint8_t>(phase);\n#endif", ctor)
        self.assertIn("#if TH08_PSP_SOFTFLOAT_CENSUS_ENABLED\n    gSoftfloatCurrentPhase = previousPhase_;\n#endif", dtor)
        for hook in ("SoftfloatCensusResetWindow(gWindowActive);", "SoftfloatCensusEmitWindow(gStage, gBaselineStageFrame, stageFrame);", "SoftfloatCensusCancelWindow();"):
            self.assertIn(hook, self.perf)
        # No timer read, heap, or per-call log in the observer.
        for forbidden in ("sceKernelGetSystemTimeWide", "malloc", "new ", "FlushBootLog"):
            self.assertNotIn(forbidden, self.probe)
        self.assertEqual(self.probe.count("BootLog("), 1)
        # The wrappers evaluate no binary64 arithmetic (which would recurse into themselves).
        wrappers = self.probe[self.probe.index("// Linker wrappers."):]
        self.assertNotRegex(wrappers, r"\b(a|b|r)\s*[-+*/]\s*(a|b|r|[0-9])")
        fmt = re.search(r'BootLog\(\s*((?:"[^"]*"\s*)+),', self.probe).group(1)
        fmt_text = "".join(re.findall(r'"([^"]*)"', fmt))
        specifiers = re.findall(r"%(?:0\d)?(?:ll|l)?[dusx]", fmt_text)
        # st, sf x2, 4 totals, 23 phases x3, 23 op classes = 99
        self.assertEqual(len(specifiers), 99)
        call = function_body(self.probe, "void SoftfloatCensusEmitWindow(")
        self.assertEqual(call.count("static_cast<"), 7 + 23 + 3)  # 7 scalars + 23 op classes + the 3-arg phase macro body
        self.assertEqual(self.probe.count("TH08_SOFTFLOAT_PHASE_ARGS("), 23 + 1)  # 23 uses + the #define (the #undef has no parenthesis)

    def test_default_off_and_host_compile(self) -> None:
        off = textwrap.dedent(
            """
            #define PSP 1
            #include "psp/softfloat_census.hpp"
            #if TH08_PSP_SOFTFLOAT_CENSUS_ENABLED
            #error census unexpectedly enabled
            #endif
            int main() { return 0; }
            """
        )
        with tempfile.TemporaryDirectory() as directory:
            d = pathlib.Path(directory)
            (d / "off.cpp").write_text(off, encoding="utf-8")
            subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT), str(d / "off.cpp"), "-o", str(d / "off")], cwd=ROOT, check=True, capture_output=True, text=True)
            (d / "fileio.hpp").write_text("#pragma once\nnamespace th08::psp { void BootLog(const char *fmt, ...); }\n", encoding="utf-8")
            subprocess.run(["g++", "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", "-DPSP=1", f"-D{FEATURE}=1", "-I", str(d), "-I", str(ROOT / "psp"), "-I", str(ROOT), "-c", str(ROOT / "psp" / "softfloat_census.cpp"), "-o", str(d / "census.o")], cwd=ROOT, check=True, capture_output=True, text=True)
            symbols = subprocess.run(["nm", str(d / "census.o")], cwd=ROOT, check=True, capture_output=True, text=True).stdout
            for sym in ("__wrap___muldf3", "__wrap_sin", "__wrap_atan2", "__wrap_sinf", "__wrap___extendsfdf2"):
                self.assertIn(f" T {sym}", symbols)


if __name__ == "__main__":
    unittest.main()
