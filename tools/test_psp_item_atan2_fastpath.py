#!/usr/bin/env python3
"""Host and source-contract tests for the Item autocollect atan2 fast path."""

from __future__ import annotations

import pathlib
import re
import shutil
import subprocess
import tempfile
import textwrap
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
AUDIT = "TH08_PSP_ITEM_ATAN2_FASTPATH_AUDIT"
PRODUCT = "TH08_PSP_ITEM_ATAN2_FASTPATH"


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
#include "psp/item_atan2_fastpath_math.hpp"
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <random>
using namespace th08::psp;
int main()
{
    std::mt19937_64 rng(20260903ULL);
    std::uniform_real_distribution<double> uexp(-12.0, 14.0);
    std::uniform_real_distribution<double> ulin(-1.0, 1.0);
    std::uniform_real_distribution<double> px(-448.0, 448.0), py(-464.0, 464.0);
    unsigned long long accepted = 0, mismatch = 0, boundary = 0, evaluated = 0, tiny = 0, other = 0;
    double maxRel = 0.0;
    auto check = [&](float y, float x) {
        DoubleFloat a{0.0f, 0.0f};
        if (ItemAtan2FastpathEvaluate(y, x, &a) == ItemAtan2FastpathReason::Accepted) {
            ++evaluated;
            const double ref = atan2(static_cast<double>(y), static_cast<double>(x));
            const double val = static_cast<double>(a.hi) + static_cast<double>(a.lo);
            const double rel = std::fabs(val - ref) / std::fabs(ref);
            if (rel > maxRel) maxRel = rel;
        }
        float out = 0.0f;
        ItemAtan2FastpathReason r = ItemAtan2FastpathReason::Count;
        const bool ok = ItemAtan2FastpathTry(y, x, &out, &r);
        if (!ok) {
            if (r == ItemAtan2FastpathReason::RoundingBoundary) ++boundary;
            else if (r == ItemAtan2FastpathReason::TinyRatio) ++tiny;
            else ++other;
            return;
        }
        ++accepted;
        const float ref = static_cast<float>(atan2(static_cast<double>(y), static_cast<double>(x)));
        std::uint32_t ab, bb;
        std::memcpy(&ab, &out, 4); std::memcpy(&bb, &ref, 4);
        if (ab != bb) { ++mismatch; if (mismatch <= 5) std::printf("MISMATCH y=%a x=%a fast=%a canon=%a\n", y, x, out, ref); }
    };
    for (int i = 0; i < 1500000; ++i) {
        check(static_cast<float>(std::ldexp(ulin(rng), static_cast<int>(uexp(rng)))),
              static_cast<float>(std::ldexp(ulin(rng), static_cast<int>(uexp(rng)))));
        check(static_cast<float>(py(rng)), static_cast<float>(px(rng)));
    }
    for (int k = 0; k <= 16; ++k) for (int d = -1500; d <= 1500; ++d) {
        const float t = static_cast<float>(k / 16.0 + d * 1e-7);
        if (t <= 0.0f) continue;
        check(t, 1.0f); check(1.0f, t); check(-t, 1.0f); check(t, -1.0f); check(-t, -1.0f); check(t * 37.5f, 37.5f);
    }
    for (int e = -22; e <= -18; ++e) for (int m = 0; m < 1000; ++m) { const float t = static_cast<float>(std::ldexp(1.0 + m / 1000.0, e)); check(t, 1.0f); check(1.0f, t); check(-t, -1.0f); }
    for (int e = -10; e <= 10; ++e) { const float v = static_cast<float>(std::ldexp(1.0, e)); check(v, v); check(-v, v); check(v, -v); check(-v, -v); check(v, 3.0f * v); check(3.0f * v, v); }
    // guards: zero / non-finite / huge / denormal must decline without touching *out
    float sentinel = 123.0f; ItemAtan2FastpathReason r = ItemAtan2FastpathReason::Count;
    if (ItemAtan2FastpathTry(0.0f, 1.0f, &sentinel, &r) || r != ItemAtan2FastpathReason::ZeroInput || sentinel != 123.0f) return 10;
    if (ItemAtan2FastpathTry(-0.0f, -1.0f, &sentinel, &r) || r != ItemAtan2FastpathReason::ZeroInput) return 11;
    if (ItemAtan2FastpathTry(1.0f, 0.0f, &sentinel, &r) || r != ItemAtan2FastpathReason::ZeroInput) return 12;
    if (ItemAtan2FastpathTry(std::nanf(""), 1.0f, &sentinel, &r) || r != ItemAtan2FastpathReason::NonFinite) return 13;
    if (ItemAtan2FastpathTry(1.0f, INFINITY, &sentinel, &r) || r != ItemAtan2FastpathReason::NonFinite) return 14;
    if (ItemAtan2FastpathTry(1.0f, 1e-40f, &sentinel, &r) || r != ItemAtan2FastpathReason::MagnitudeRange) return 15;
    if (ItemAtan2FastpathTry(1.0e30f, 1.0f, &sentinel, &r) || r != ItemAtan2FastpathReason::MagnitudeRange) return 16;
    if (ItemAtan2FastpathTry(1.0f, 4194304.0f, &sentinel, &r) || r != ItemAtan2FastpathReason::TinyRatio) return 17;
    if (sentinel != 123.0f) return 18;
    std::printf("evaluated=%llu accepted=%llu mismatch=%llu boundary=%llu tiny=%llu other=%llu maxrel=%.3e log2=%.1f\n",
                evaluated, accepted, mismatch, boundary, tiny, other, maxRel, std::log2(maxRel));
    if (mismatch != 0) return 1;
    if (accepted < 2500000ULL) return 2;
    if (static_cast<double>(boundary) / static_cast<double>(accepted + boundary) > 1e-3) return 3;
    if (maxRel > 2.2737367544323206e-13) return 4; // 2^-42, four bits inside the 2^-38 bound
    if (other != 0) return 5;
    return 0;
}
'''


class PspItemAtan2FastpathTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.makefile = read("Makefile.psp")
        cls.math = read("psp/item_atan2_fastpath_math.hpp")
        cls.header = read("psp/item_atan2_audit.hpp")
        cls.probe = read("psp/item_atan2_audit.cpp")
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
            exe = pathlib.Path(directory) / "atan2"
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
        self.assertIn("PSP_ITEM_ATAN2_SRCS := psp/item_atan2_audit.cpp", self.makefile)
        self.assertIn("$(PSP_ITEM_ATAN2_SRCS)", self.makefile)
        for stamp in ("item-atan2-fastpath-audit-0.stamp", "item-atan2-fastpath-audit-1.stamp",
                      "item-atan2-fastpath-0.stamp", "item-atan2-fastpath-1.stamp"):
            self.assertIn(stamp, self.makefile)
        self.assertIn("src/ItemManager.o psp/item_atan2_audit.o psp/item_sincos_audit.o: CXXFLAGS += -ffp-contract=off", self.makefile)
        self.assertIn(
            "src/ItemManager.o psp/item_atan2_audit.o psp/perf_attribution.o psp/main.o: \\\n"
            "\t$(ITEM_ATAN2_FASTPATH_AUDIT_CONFIG_STAMP) $(ITEM_ATAN2_FASTPATH_CONFIG_STAMP)",
            self.makefile,
        )
        for macro in ("TH08_PSP_FEATURE_ITEM_ATAN2_FASTPATH_AUDIT", "TH08_PSP_FEATURE_ITEM_ATAN2_FASTPATH"):
            self.assertIn(f"#define {macro} 1", self.psp_main)
            self.assertIn(f"#define {macro} 0", self.psp_main)
            self.assertIn(f"        {macro},", self.psp_main)
        self.assertIn("ITEM_ATAN2_FASTPATH_AUDIT=%d ITEM_ATAN2_FASTPATH=%d ", self.psp_main)
        self.assertIn("#error \"ITEM_ATAN2 audit and product switches are mutually exclusive\"", self.header)

    def test_default_off_compiles_nothing(self) -> None:
        unit = textwrap.dedent(
            """
            #define PSP 1
            #include "psp/item_atan2_audit.hpp"
            #if TH08_PSP_ITEM_ATAN2_STATS_ENABLED || TH08_PSP_ITEM_ATAN2_FASTPATH_AUDIT_ENABLED || TH08_PSP_ITEM_ATAN2_FASTPATH_PRODUCT_ENABLED
            #error atan2 switches unexpectedly enabled
            #endif
            int main() { return 0; }
            """
        )
        with tempfile.TemporaryDirectory() as directory:
            exe = pathlib.Path(directory) / "off"
            self.compile_host(unit, exe)
            symbols = subprocess.run(["nm", "-C", str(exe)], cwd=ROOT, check=True, capture_output=True, text=True).stdout
            self.assertNotIn("ItemAtan2", symbols)
            subprocess.run([str(exe)], check=True)

    def test_item_manager_hooks_and_canonical_path(self) -> None:
        body = function_body(self.item_manager, "void ItemManager::OnUpdate()")
        self.assertIn('#include "item_atan2_audit.hpp"', self.item_manager)
        # Canonical call survives on every non-product path, and the product path
        # falls back to the same canonical call inside the helper.
        self.assertIn("#else\n                    angle = g_Player.AngleToPoint(&item->currentPosition);\n#endif", body)
        helper = function_body(self.item_manager, "inline f32 ItemAutocollectAngleFastpath(Item *item)")
        self.assertIn("return g_Player.AngleToPoint(&item->currentPosition);", helper)
        self.assertIn("g_Player.position.x - item->currentPosition.x", helper)
        self.assertIn("g_Player.position.y - item->currentPosition.y", helper)
        self.assertIn("ItemAtan2FastpathTry(yDelta, xDelta, &angle, &reason)", helper)
        # Audit hooks: begin before the canonical call, compare after it, velocity timed after.
        self.assertLess(body.index("ItemAtan2AuditBeginCall("), body.index("g_Player.AngleToPoint(&item->currentPosition);"))
        self.assertLess(body.index("g_Player.AngleToPoint(&item->currentPosition);"), body.index("ItemAtan2AuditAfterCanonical("))
        self.assertLess(body.index("ItemAtan2AuditAfterCanonical("), body.index("ItemAtan2AuditEndVelocity("))
        macros = ("TH08_PSP_ITEM_ATAN2_FASTPATH_AUDIT_ENABLED", "TH08_PSP_ITEM_ATAN2_FASTPATH_PRODUCT_ENABLED")
        for needle in ("ItemAtan2Audit", "itemAtan2", "ItemAutocollectAngleFastpath", "ItemAtan2Fastpath", "ItemAtan2Product"):
            self.assertEqual(unguarded_lines(self.item_manager, needle, macros), [], needle)
        # The audit observer recomputes the same deltas (player minus item) as Player::AngleToPoint.
        observer = function_body(self.probe, "void ItemAtan2AuditAfterCanonical(")
        self.assertIn("const float xDelta = playerX - itemX;", observer)
        self.assertIn("const float yDelta = playerY - itemY;", observer)
        self.assertIn("ItemAtan2FastpathTry(yDelta, xDelta, &fast, &reason)", observer)
        for forbidden in ("g_Rng", "ReplayManager", "frameEventFlags", "PlaySound", "startPositionOrVelocity"):
            self.assertNotIn(forbidden, self.probe)
        # The 33 KiB per-slot history and the audit entry points exist only in audit builds.
        audit_only = ("TH08_PSP_ITEM_ATAN2_FASTPATH_AUDIT_ENABLED",)
        for needle in ("gSlots", "ItemAtan2AuditAfterCanonical(", "ItemAtan2AuditBeginCall(", "ItemAtan2AuditEndVelocity("):
            self.assertEqual(unguarded_lines(self.probe, needle, audit_only), [], needle)
            self.assertEqual(unguarded_lines(self.header, needle, audit_only), [], needle)

    def test_product_mode_host_compiles_without_audit_state(self) -> None:
        unit = textwrap.dedent(
            """
            #define PSP 1
            #define TH08_PSP_ITEM_ATAN2_FASTPATH 1
            #include "psp/item_atan2_audit.hpp"
            #if !TH08_PSP_ITEM_ATAN2_FASTPATH_PRODUCT_ENABLED || TH08_PSP_ITEM_ATAN2_FASTPATH_AUDIT_ENABLED
            #error product switch not selected
            #endif
            int main()
            {
                float out = 0.0f;
                th08::psp::ItemAtan2FastpathReason r = th08::psp::ItemAtan2FastpathReason::Count;
                const bool ok = th08::psp::ItemAtan2FastpathTry(1.0f, 2.0f, &out, &r);
                th08::psp::ItemAtan2ProductNote(r);
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
                 "-DPSP=1", "-DTH08_PSP_ITEM_ATAN2_FASTPATH=1", "-I", str(d), "-I", str(ROOT / "psp"), "-I", str(ROOT),
                 str(d / "main.cpp"), str(d / "stub.cpp"), str(ROOT / "psp" / "item_atan2_audit.cpp"), "-o", str(d / "product")],
                cwd=ROOT, check=True, capture_output=True, text=True,
            )
            symbols = subprocess.run(["nm", "-C", str(d / "product")], cwd=ROOT, check=True, capture_output=True, text=True).stdout
            self.assertNotIn("gSlots", symbols)
            self.assertNotIn("ItemAtan2AuditAfterCanonical", symbols)
            self.assertIn("ItemAtan2ProductNote", symbols)
            subprocess.run([str(d / "product")], check=True)

    def test_parent_window_hooks_and_record(self) -> None:
        self.assertIn('#include "item_atan2_audit.hpp"', self.perf)
        for hook in ("ItemAtan2StatsResetWindow(gWindowActive);", "ItemAtan2StatsEmitWindow(gStage, gBaselineStageFrame, stageFrame);", "ItemAtan2StatsCancelWindow();"):
            self.assertIn(hook, self.perf)
        self.assertEqual(unguarded_lines(self.perf, "ItemAtan2Stats", ("TH08_PSP_ITEM_ATAN2_STATS_ENABLED",)), [])
        self.assertEqual(self.probe.count("BootLog("), 1)
        self.assertNotIn("FlushBootLog", self.probe)
        fmt = re.search(r'BootLog\(\s*((?:"[^"]*"\s*)+),', self.probe).group(1)
        fmt_text = "".join(re.findall(r'"([^"]*)"', fmt))
        specifiers = re.findall(r"%(?:0\d)?(?:ll|l)?[dusx]", fmt_text)
        self.assertEqual(len(specifiers), 44)
        call = function_body(self.probe, "void ItemAtan2StatsEmitWindow(")
        self.assertEqual(call.count("static_cast<"), 43)  # 44 args minus the literal mode string
        self.assertIn("mode=%s", fmt_text)

    def test_psp_codegen_has_no_fused_multiply_add(self) -> None:
        if shutil.which("psp-g++") is None:
            self.skipTest("psp-g++ not available")
        unit = textwrap.dedent(
            """
            #include "psp/item_atan2_fastpath_math.hpp"
            float probe(float y, float x, unsigned char *reason)
            {
                float out = 0.0f;
                th08::psp::ItemAtan2FastpathReason r;
                const bool ok = th08::psp::ItemAtan2FastpathTry(y, x, &out, &r);
                *reason = static_cast<unsigned char>(r);
                return ok ? out : -1.0f;
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
            self.assertRegex(text, r"\bdiv\.s\b")
            self.assertRegex(text, r"\bmul\.s\b")


if __name__ == "__main__":
    unittest.main()
