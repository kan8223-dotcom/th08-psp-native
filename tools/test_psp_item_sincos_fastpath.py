#!/usr/bin/env python3
"""Host and source-contract tests for the Item autocollect sin/cos fast path."""

from __future__ import annotations

import pathlib
import re
import shutil
import subprocess
import tempfile
import textwrap
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
AUDIT = "TH08_PSP_ITEM_SINCOS_FASTPATH_AUDIT"
PRODUCT = "TH08_PSP_ITEM_SINCOS_FASTPATH"


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
#include "psp/item_sincos_fastpath_math.hpp"
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <random>
using namespace th08::psp;
int main()
{
    std::mt19937_64 rng(20260903ULL);
    std::uniform_real_distribution<double> uang(-3.15, 3.15), umag(0.5, 64.0), px(-448.0, 448.0), py(-464.0, 464.0);
    const float mags[] = {8.0f, 10.0f, 12.5f, 16.0f, 4.0f, 6.5f, 3.0f, 20.0f};
    unsigned long long evaluated = 0, accepted = 0, mismatch = 0, boundary = 0, tiny = 0, other = 0;
    double maxRel = 0.0;
    auto check = [&](float a, float m) {
        DoubleFloat x{0.0f, 0.0f}, y{0.0f, 0.0f};
        if (ItemSinCosFastpathEvaluate(a, m, &x, &y) == ItemSinCosFastpathReason::Accepted) {
            ++evaluated;
            const double rx = std::cos(static_cast<double>(a)) * m, ry = std::sin(static_cast<double>(a)) * m;
            const double vx = static_cast<double>(x.hi) + static_cast<double>(x.lo), vy = static_cast<double>(y.hi) + static_cast<double>(y.lo);
            const double relx = std::fabs(vx - rx) / std::fabs(rx), rely = std::fabs(vy - ry) / std::fabs(ry);
            if (relx > maxRel) maxRel = relx;
            if (rely > maxRel) maxRel = rely;
        }
        float fx = 0.0f, fy = 0.0f;
        ItemSinCosFastpathReason r = ItemSinCosFastpathReason::Count;
        const bool ok = ItemSinCosFastpathTry(a, m, &fx, &fy, &r);
        if (!ok) {
            if (r == ItemSinCosFastpathReason::RoundingBoundary) ++boundary;
            else if (r == ItemSinCosFastpathReason::TinyReduced) ++tiny;
            else ++other;
            return;
        }
        ++accepted;
        const float cx = static_cast<float>(std::cos(static_cast<double>(a)) * static_cast<double>(m));
        const float cy = static_cast<float>(std::sin(static_cast<double>(a)) * static_cast<double>(m));
        std::uint32_t a1, b1, a2, b2;
        std::memcpy(&a1, &fx, 4); std::memcpy(&b1, &cx, 4); std::memcpy(&a2, &fy, 4); std::memcpy(&b2, &cy, 4);
        if (a1 != b1 || a2 != b2) { ++mismatch; if (mismatch <= 5) std::printf("MISMATCH a=%a m=%a fast=(%a,%a) canon=(%a,%a)\n", a, m, fx, fy, cx, cy); }
    };
    for (int i = 0; i < 1500000; ++i) {
        check(static_cast<float>(uang(rng)), static_cast<float>(umag(rng)));
        check(static_cast<float>(std::atan2(py(rng), px(rng))), mags[i & 7]);
    }
    const float fpi = 3.1415927410125732f, fhpi = 1.5707963705062866f;
    for (int e = -30; e <= 1; ++e) for (int m = 0; m < 100; ++m) {
        const float d = static_cast<float>(std::ldexp(1.0 + m / 100.0, e));
        for (float base : {0.0f, fhpi, -fhpi, fpi, -fpi}) { check(base + d, 10.0f); check(base - d, 10.0f); }
    }
    for (float a : {fpi, -fpi, fhpi, -fhpi, 1.0f, -1.0f, 2.0f, -2.0f, 3.0f, -3.0f}) for (float m : mags) check(a, m);
    float sx = 123.0f, sy = 456.0f; ItemSinCosFastpathReason r = ItemSinCosFastpathReason::Count;
    if (ItemSinCosFastpathTry(0.0f, 10.0f, &sx, &sy, &r) || r != ItemSinCosFastpathReason::ZeroInput) return 10;
    if (ItemSinCosFastpathTry(-0.0f, 10.0f, &sx, &sy, &r) || r != ItemSinCosFastpathReason::ZeroInput) return 11;
    if (ItemSinCosFastpathTry(1.0f, 0.0f, &sx, &sy, &r) || r != ItemSinCosFastpathReason::ZeroInput) return 12;
    if (ItemSinCosFastpathTry(std::nanf(""), 10.0f, &sx, &sy, &r) || r != ItemSinCosFastpathReason::NonFinite) return 13;
    if (ItemSinCosFastpathTry(1.0f, INFINITY, &sx, &sy, &r) || r != ItemSinCosFastpathReason::NonFinite) return 14;
    if (ItemSinCosFastpathTry(5.0f, 10.0f, &sx, &sy, &r) || r != ItemSinCosFastpathReason::MagnitudeRange) return 15;
    if (ItemSinCosFastpathTry(1.0f, 1e-40f, &sx, &sy, &r) || r != ItemSinCosFastpathReason::MagnitudeRange) return 16;
    if (ItemSinCosFastpathTry(fhpi, 10.0f, &sx, &sy, &r) || r != ItemSinCosFastpathReason::TinyReduced) return 17;
    if (sx != 123.0f || sy != 456.0f) return 18;
    std::printf("evaluated=%llu accepted=%llu mismatch=%llu boundary=%llu tiny=%llu other=%llu maxrel=%.3e log2=%.1f\n",
                evaluated, accepted, mismatch, boundary, tiny, other, maxRel, std::log2(maxRel));
    if (mismatch != 0) return 1;
    if (accepted < 2500000ULL) return 2;
    if (static_cast<double>(boundary) / static_cast<double>(accepted + boundary) > 2e-3) return 3;
    if (maxRel > 2.2737367544323206e-13) return 4; // 2^-42, four bits inside the 2^-38 bound
    return 0;
}
'''


class PspItemSinCosFastpathTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.makefile = read("Makefile.psp")
        cls.math = read("psp/item_sincos_fastpath_math.hpp")
        cls.header = read("psp/item_sincos_audit.hpp")
        cls.probe = read("psp/item_sincos_audit.cpp")
        cls.item_manager = read("src/ItemManager.cpp")
        cls.perf = read("psp/perf_attribution.cpp")
        cls.psp_main = read("psp/main.cpp")

    def compile_host(self, source: str, output: pathlib.Path, extra: tuple[str, ...] = ()) -> None:
        unit = output.with_suffix(".cpp")
        unit.write_text(source, encoding="utf-8")
        subprocess.run(
            ["g++", "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
             "-ffp-contract=off", *extra, "-I", str(ROOT), str(unit), "-o", str(output)],
            cwd=ROOT, check=True, capture_output=True, text=True,
        )

    def test_host_bit_exactness_and_error_bound(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            exe = pathlib.Path(directory) / "sincos"
            self.compile_host(HARNESS, exe)
            result = subprocess.run([str(exe)], cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("mismatch=0", result.stdout)

    def test_makefile_and_fingerprint_contracts(self) -> None:
        for feature in (AUDIT, PRODUCT):
            self.assertIn(f"{feature} ?= 0", self.makefile)
            self.assertIn(f"$(error {feature} must be 0 or 1)", self.makefile)
            self.assertIn(f"CXXFLAGS += -D{feature}=1", self.makefile)
            self.assertNotIn(f"-D{feature}=$({feature})", self.makefile)
        self.assertIn(f"$(error {AUDIT} and {PRODUCT} are mutually exclusive)", self.makefile)
        self.assertIn(f"$(error {AUDIT}=1 requires TH08_PSP_PERF_ATTRIBUTION=1)", self.makefile)
        self.assertIn("PSP_ITEM_SINCOS_SRCS := psp/item_sincos_audit.cpp", self.makefile)
        self.assertIn("$(PSP_ITEM_SINCOS_SRCS)", self.makefile)
        for stamp in ("item-sincos-fastpath-audit-0.stamp", "item-sincos-fastpath-audit-1.stamp",
                      "item-sincos-fastpath-0.stamp", "item-sincos-fastpath-1.stamp"):
            self.assertIn(stamp, self.makefile)
        self.assertIn("src/ItemManager.o psp/item_atan2_audit.o psp/item_sincos_audit.o: CXXFLAGS += -ffp-contract=off", self.makefile)
        self.assertIn(
            "src/ItemManager.o psp/item_sincos_audit.o psp/perf_attribution.o psp/main.o: \\\n"
            "\t$(ITEM_SINCOS_FASTPATH_AUDIT_CONFIG_STAMP) $(ITEM_SINCOS_FASTPATH_CONFIG_STAMP)",
            self.makefile,
        )
        for macro in ("TH08_PSP_FEATURE_ITEM_SINCOS_FASTPATH_AUDIT", "TH08_PSP_FEATURE_ITEM_SINCOS_FASTPATH"):
            self.assertIn(f"#define {macro} 1", self.psp_main)
            self.assertIn(f"#define {macro} 0", self.psp_main)
            self.assertIn(f"        {macro},", self.psp_main)
        self.assertIn("ITEM_SINCOS_FASTPATH_AUDIT=%d ITEM_SINCOS_FASTPATH=%d ", self.psp_main)
        self.assertIn("#error \"ITEM_SINCOS audit and product switches are mutually exclusive\"", self.header)

    def test_default_off_compiles_nothing(self) -> None:
        unit = textwrap.dedent(
            """
            #define PSP 1
            #include "psp/item_sincos_audit.hpp"
            #if TH08_PSP_ITEM_SINCOS_STATS_ENABLED || TH08_PSP_ITEM_SINCOS_FASTPATH_AUDIT_ENABLED || TH08_PSP_ITEM_SINCOS_FASTPATH_PRODUCT_ENABLED
            #error sincos switches unexpectedly enabled
            #endif
            int main() { return 0; }
            """
        )
        with tempfile.TemporaryDirectory() as directory:
            exe = pathlib.Path(directory) / "off"
            self.compile_host(unit, exe)
            symbols = subprocess.run(["nm", "-C", str(exe)], cwd=ROOT, check=True, capture_output=True, text=True).stdout
            self.assertNotIn("ItemSinCos", symbols)
            subprocess.run([str(exe)], check=True)

    def test_item_manager_hooks_and_canonical_path(self) -> None:
        self.assertIn('#include "item_sincos_audit.hpp"', self.item_manager)
        helper = function_body(self.item_manager, "inline void ItemAutocollectVelocityCompute(Item *item, f32 angle, f32 speed)")
        # Product path: fast pair or the canonical call; audit path: canonical call, then shadow compare.
        self.assertEqual(helper.count("item->startPositionOrVelocity.FromAngleMagnitude(angle, speed);"), 2)
        self.assertIn("ItemSinCosFastpathTry(angle, speed, &velocityX,", helper)
        self.assertIn("item->startPositionOrVelocity.x = velocityX;", helper)
        self.assertIn("item->startPositionOrVelocity.y = velocityY;", helper)
        self.assertLess(helper.index("ItemSinCosAuditBeginCall("), helper.index("FromAngleMagnitude(angle, speed);\n#if TH08_PSP_ITEM_SINCOS_FASTPATH_AUDIT_ENABLED"))
        self.assertIn("ItemSinCosAuditAfterCanonical(\n        angle, speed, item->startPositionOrVelocity.x,\n        item->startPositionOrVelocity.y, sinCosSampled, sinCosStartUs);", helper)
        # Every PSP velocity computation routes through the helper; the non-PSP canonical call survives.
        self.assertEqual(self.item_manager.count("ItemAutocollectVelocityCompute("), 4)  # definition + 3 call sites
        self.assertIn("#else\n                    item->startPositionOrVelocity.FromAngleMagnitude(\n                        angle, g_Player.primaryShtFile->itemAutoCollectSpeed);\n#endif", self.item_manager)
        macros = ("TH08_PSP_ITEM_SINCOS_FASTPATH_AUDIT_ENABLED", "TH08_PSP_ITEM_SINCOS_FASTPATH_PRODUCT_ENABLED")
        for needle in ("ItemSinCosAudit", "ItemSinCosFastpath", "ItemSinCosProduct", "sinCos"):
            self.assertEqual(unguarded_lines(self.item_manager, needle, macros), [], needle)
        observer = function_body(self.probe, "void ItemSinCosAuditAfterCanonical(")
        self.assertIn("ItemSinCosFastpathTry(angle, magnitude, &fastX, &fastY, &reason)", observer)
        for forbidden in ("g_Rng", "ReplayManager", "frameEventFlags", "PlaySound", "startPositionOrVelocity"):
            self.assertNotIn(forbidden, self.probe)
        audit_only = ("TH08_PSP_ITEM_SINCOS_FASTPATH_AUDIT_ENABLED",)
        for needle in ("ItemSinCosAuditAfterCanonical(", "ItemSinCosAuditBeginCall("):
            self.assertEqual(unguarded_lines(self.probe, needle, audit_only), [], needle)
            self.assertEqual(unguarded_lines(self.header, needle, audit_only), [], needle)

    def test_product_mode_host_compiles_without_audit_state(self) -> None:
        unit = textwrap.dedent(
            """
            #define PSP 1
            #define TH08_PSP_ITEM_SINCOS_FASTPATH 1
            #include "psp/item_sincos_audit.hpp"
            #if !TH08_PSP_ITEM_SINCOS_FASTPATH_PRODUCT_ENABLED || TH08_PSP_ITEM_SINCOS_FASTPATH_AUDIT_ENABLED
            #error product switch not selected
            #endif
            int main()
            {
                float x = 0.0f, y = 0.0f;
                th08::psp::ItemSinCosFastpathReason r = th08::psp::ItemSinCosFastpathReason::Count;
                const bool ok = th08::psp::ItemSinCosFastpathTry(1.0f, 10.0f, &x, &y, &r);
                th08::psp::ItemSinCosProductNote(r);
                return ok ? 0 : 1;
            }
            """
        )
        stub = textwrap.dedent(
            """
            #include <cstdint>
            #include <cstdarg>
            #include <cstdio>
            extern "C" std::uint64_t sceKernelGetSystemTimeWide(void) { return 0; }
            namespace th08::psp { void BootLog(const char *fmt, ...) { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); } }
            """
        )
        with tempfile.TemporaryDirectory() as directory:
            d = pathlib.Path(directory)
            (d / "main.cpp").write_text(unit, encoding="utf-8")
            (d / "stub.cpp").write_text(stub, encoding="utf-8")
            (d / "fileio.hpp").write_text("#pragma once\nnamespace th08::psp { void BootLog(const char *fmt, ...); }\n", encoding="utf-8")
            subprocess.run(
                ["g++", "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", "-ffp-contract=off",
                 "-DPSP=1", "-DTH08_PSP_ITEM_SINCOS_FASTPATH=1", "-I", str(d), "-I", str(ROOT / "psp"), "-I", str(ROOT),
                 str(d / "main.cpp"), str(d / "stub.cpp"), str(ROOT / "psp" / "item_sincos_audit.cpp"), "-o", str(d / "product")],
                cwd=ROOT, check=True, capture_output=True, text=True,
            )
            symbols = subprocess.run(["nm", "-C", str(d / "product")], cwd=ROOT, check=True, capture_output=True, text=True).stdout
            self.assertNotIn("ItemSinCosAuditAfterCanonical", symbols)
            self.assertIn("ItemSinCosProductNote", symbols)
            subprocess.run([str(d / "product")], check=True)

    def test_parent_window_hooks_and_record(self) -> None:
        self.assertIn('#include "item_sincos_audit.hpp"', self.perf)
        for hook in ("ItemSinCosStatsResetWindow(gWindowActive);", "ItemSinCosStatsEmitWindow(gStage, gBaselineStageFrame, stageFrame);", "ItemSinCosStatsCancelWindow();"):
            self.assertIn(hook, self.perf)
        self.assertEqual(unguarded_lines(self.perf, "ItemSinCosStats", ("TH08_PSP_ITEM_SINCOS_STATS_ENABLED",)), [])
        self.assertEqual(self.probe.count("BootLog("), 1)
        self.assertNotIn("FlushBootLog", self.probe)
        fmt = re.search(r'BootLog\(\s*((?:"[^"]*"\s*)+),', self.probe).group(1)
        fmt_text = "".join(re.findall(r'"([^"]*)"', fmt))
        specifiers = re.findall(r"%(?:0\d)?(?:ll|l)?[dusx]", fmt_text)
        self.assertEqual(len(specifiers), 34)
        call = function_body(self.probe, "void ItemSinCosStatsEmitWindow(")
        self.assertEqual(call.count("static_cast<"), 33)  # 34 args minus the literal mode string
        self.assertIn("mode=%s", fmt_text)

    def test_psp_codegen_has_no_fused_multiply_add(self) -> None:
        if shutil.which("psp-g++") is None:
            self.skipTest("psp-g++ not available")
        unit = textwrap.dedent(
            """
            #include "psp/item_sincos_fastpath_math.hpp"
            float probe(float angle, float magnitude, unsigned char *reason)
            {
                float x = 0.0f;
                float y = 0.0f;
                th08::psp::ItemSinCosFastpathReason r;
                const bool ok = th08::psp::ItemSinCosFastpathTry(angle, magnitude, &x, &y, &r);
                *reason = static_cast<unsigned char>(r);
                return ok ? x + y : -1.0f;
            }
            """
        )
        with tempfile.TemporaryDirectory() as directory:
            src = pathlib.Path(directory) / "probe.cpp"
            asm = pathlib.Path(directory) / "probe.s"
            src.write_text(unit, encoding="utf-8")
            subprocess.run(
                ["psp-g++", "-std=gnu++17", "-O2", "-G0", "-march=allegrex", "-mtune=allegrex",
                 "-ffp-contract=off", "-fno-exceptions", "-fno-rtti", "-I", str(ROOT), "-S", str(src), "-o", str(asm)],
                cwd=ROOT, check=True, capture_output=True, text=True,
            )
            text = asm.read_text(encoding="utf-8")
            self.assertNotRegex(text, r"\bmadd\.s\b|\bmsub\.s\b|\bnmadd\.s\b|\bnmsub\.s\b")
            self.assertRegex(text, r"\bmul\.s\b")


if __name__ == "__main__":
    unittest.main()
