#!/usr/bin/env python3
"""Host and source-contract tests for the PSP Item update sub-profiler."""

from __future__ import annotations

import pathlib
import re
import subprocess
import tempfile
import textwrap
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
FEATURE = "TH08_PSP_ITEM_UPDATE_SUBPROFILE"
ENABLED = FEATURE + "_ENABLED"


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


def unguarded_probe_lines(source: str, needle: str) -> list[int]:
    """Lines mentioning ``needle`` outside any #if ENABLED region."""
    stack: list[bool] = []
    offenders: list[int] = []
    for number, line in enumerate(source.splitlines(), start=1):
        stripped = line.strip()
        if stripped.startswith("#if"):
            stack.append(ENABLED in stripped)
        elif stripped.startswith("#endif"):
            if stack:
                stack.pop()
        elif stripped.startswith("#else") or stripped.startswith("#elif"):
            if stack and stack[-1]:
                # The #else branch of our own guard is the canonical path.
                stack[-1] = False
        if needle in line and not stripped.startswith("#") and not any(stack):
            offenders.append(number)
    return offenders


class PspItemUpdateSubprofileTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.makefile = read("Makefile.psp")
        cls.header = read("psp/item_update_subprofile.hpp")
        cls.math = read("psp/item_update_subprofile_math.hpp")
        cls.probe = read("psp/item_update_subprofile.cpp")
        cls.item_manager = read("src/ItemManager.cpp")
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
        self.assertIn(f"{FEATURE} ?= 0", self.makefile)
        self.assertIn(
            f"$(error {FEATURE}=1 requires TH08_PSP_PERF_ATTRIBUTION=1)",
            self.makefile,
        )
        self.assertIn(
            "PSP_ITEM_UPDATE_SUBPROFILE_SRCS := psp/item_update_subprofile.cpp",
            self.makefile,
        )
        self.assertIn("$(PSP_ITEM_UPDATE_SUBPROFILE_SRCS)", self.makefile)
        self.assertIn("item-update-subprofile-0.stamp", self.makefile)
        self.assertIn("item-update-subprofile-1.stamp", self.makefile)
        self.assertIn("$(ITEM_UPDATE_SUBPROFILE_CONFIG_STAMPS)", self.makefile)
        self.assertIn(f"CXXFLAGS += -D{FEATURE}=1", self.makefile)
        self.assertNotIn(f"-D{FEATURE}=$({FEATURE})", self.makefile)
        self.assertIn(
            "src/ItemManager.o psp/item_update_subprofile.o "
            "psp/perf_attribution.o \\\n\tpsp/main.o: "
            "$(ITEM_UPDATE_SUBPROFILE_CONFIG_STAMP)",
            self.makefile,
        )

        self.assertIn(
            "#define TH08_PSP_FEATURE_ITEM_UPDATE_SUBPROFILE 1", self.psp_main
        )
        self.assertIn(
            "#define TH08_PSP_FEATURE_ITEM_UPDATE_SUBPROFILE 0", self.psp_main
        )
        self.assertIn("ITEM_UPDATE_SUBPROFILE=%d", self.psp_main)
        self.assertIn("TH08_PSP_FEATURE_ITEM_UPDATE_SUBPROFILE,", self.psp_main)
        # The new field follows its sibling in both the format and the args.
        self.assertIn(
            "DRAW_PRIORITY_SUBPROFILE=%d ITEM_UPDATE_SUBPROFILE=%d ",
            self.psp_main,
        )
        self.assertIn(
            "TH08_PSP_FEATURE_DRAW_PRIORITY_SUBPROFILE,\n"
            "        TH08_PSP_FEATURE_ITEM_UPDATE_SUBPROFILE,",
            self.psp_main,
        )

        off_unit = textwrap.dedent(
            """
            #define PSP 1
            #include "psp/item_update_subprofile.hpp"
            #if TH08_PSP_ITEM_UPDATE_SUBPROFILE_ENABLED
            #error item-update probe unexpectedly enabled
            #endif
            int main() { return TH08_PSP_ITEM_UPDATE_SUBPROFILE_ENABLED; }
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
            self.assertNotIn("ItemUpdateSubprofile", symbols)
            subprocess.run([str(exe)], check=True)

    def test_math_contracts_on_host(self) -> None:
        harness = textwrap.dedent(
            """
            #include "psp/item_update_subprofile_math.hpp"
            #include <cstdint>
            #include <cstdio>
            #include <limits>
            using namespace th08::psp;
            int main()
            {
                // Exactly 1 of 32 list positions is sampled on every tick.
                std::uint32_t rotation = 0U;
                for (unsigned tick = 0U; tick < 64U; ++tick)
                {
                    rotation = ItemUpdateNextRotation(rotation);
                    unsigned hits = 0U;
                    for (std::uint32_t ordinal = 0U; ordinal < 1024U; ++ordinal)
                        hits += ItemUpdateShouldSampleOrdinal(ordinal, rotation) ? 1U : 0U;
                    if (hits != 32U) { std::printf("hits=%u\\n", hits); return 1; }
                }
                // A fixed position is sampled exactly once per 32 ticks.
                rotation = 5U;
                unsigned fixedHits = 0U;
                for (unsigned tick = 0U; tick < 32U; ++tick)
                {
                    rotation = ItemUpdateNextRotation(rotation);
                    fixedHits += ItemUpdateShouldSampleOrdinal(100U, rotation) ? 1U : 0U;
                }
                if (fixedHits != 1U) return 2;
                if (ItemUpdateNextRotation(31U) != 0U) return 3;
                static_assert(ItemUpdateStateCounter(0) == kItemUpdateCounterStateDefault, "s0");
                static_assert(ItemUpdateStateCounter(1) == kItemUpdateCounterStateAutocollect, "s1");
                static_assert(ItemUpdateStateCounter(2) == kItemUpdateCounterStateSpread, "s2");
                static_assert(ItemUpdateStateCounter(3) == kItemUpdateCounterStateRising, "s3");
                static_assert(ItemUpdateStateCounter(4) == kItemUpdateCounterStateOther, "s4");
                static_assert(ItemUpdateStateCounter(5) == kItemUpdateCounterStateApex, "s5");
                static_assert(ItemUpdateStateCounter(-1) == kItemUpdateCounterStateOther, "s-1");
                static_assert(kItemUpdateCounterCount == 12U, "count");
                ItemUpdateDurationStat stat{};
                if (ItemUpdateAccumulate(stat, 5U) || stat.totalUs != 5U ||
                    stat.maxUs != 5U || stat.calls != 1U) return 4;
                if (ItemUpdateAccumulate(stat, 3U) || stat.totalUs != 8U ||
                    stat.maxUs != 5U || stat.calls != 2U) return 5;
                stat.totalUs = std::numeric_limits<std::uint64_t>::max() - 1U;
                if (!ItemUpdateAccumulate(stat, 2U) ||
                    stat.totalUs != std::numeric_limits<std::uint64_t>::max()) return 6;
                std::uint64_t total = std::numeric_limits<std::uint64_t>::max() - 1U;
                if (!ItemUpdateAddCounter(total, 5U) ||
                    total != std::numeric_limits<std::uint64_t>::max()) return 7;
                total = 1U;
                if (ItemUpdateAddCounter(total, 2U) || total != 3U) return 8;
                constexpr ItemUpdateDifference ok = ItemUpdateSubtract(10U, 4U);
                static_assert(ok.value == 6U && !ok.underflow, "ok");
                constexpr ItemUpdateDifference under = ItemUpdateSubtract(4U, 10U);
                static_assert(under.value == 0U && under.underflow, "under");
                return 0;
            }
            """
        )
        with tempfile.TemporaryDirectory() as directory:
            exe = pathlib.Path(directory) / "math"
            self.compile_host(harness, exe)
            subprocess.run([str(exe)], check=True)

    def test_item_manager_hooks_are_guarded_and_canonical_path_survives(self) -> None:
        body = function_body(self.item_manager, "void ItemManager::OnUpdate()")
        self.assertIn('#include "item_update_subprofile.hpp"', self.item_manager)
        self.assertIn(
            "ItemUpdateSubprofileBeginTick(itemUpdateRotation)", body
        )
        self.assertIn(
            "ItemUpdateSubprofileEndTick(this->itemCount,", body
        )
        self.assertIn(
            "ItemUpdateShouldSampleOrdinal(this->itemCount - 1U,", body
        )
        # The canonical collision condition is retained verbatim on the OFF path.
        self.assertIn(
            "#else\n        if (item->state != ITEM_STATE_TIME_RISING &&\n"
            "            g_Player.CalcItemBoxCollision(&item->currentPosition, &itemBox))\n"
            "#endif",
            body,
        )
        probe = function_body(
            self.item_manager,
            "inline bool ItemUpdateSubprofileCollisionProbe(",
        )
        self.assertIn("if (item->state == ITEM_STATE_TIME_RISING)\n        return false;", probe)
        self.assertIn("g_Player.CalcItemBoxCollision(&item->currentPosition, itemBox)", probe)
        # Whole is closed on all three exits: offscreen delete, pickup delete, normal end.
        self.assertEqual(body.count("ItemUpdateSection::Whole"), 3)
        self.assertEqual(body.count("ItemUpdateSection::Autocollect"), 1)
        self.assertEqual(body.count("ItemUpdateSection::Script"), 1)
        # Every probe reference is inside a #if TH08_PSP_ITEM_UPDATE_SUBPROFILE_ENABLED region.
        for needle in ("ItemUpdateSubprofile", "itemUpdate", "th08::psp::kItemUpdate"):
            self.assertEqual(
                unguarded_probe_lines(self.item_manager, needle), [], needle
            )
        # No RNG, replay, or game-state writes in the probe.
        for forbidden in ("g_Rng", "ReplayManager", "frameEventFlags", "PlaySound"):
            self.assertNotIn(forbidden, self.probe)

    def test_parent_window_hooks(self) -> None:
        self.assertIn('#include "item_update_subprofile.hpp"', self.perf)
        for hook in (
            "ItemUpdateSubprofileResetWindow(gWindowActive);",
            "ItemUpdateSubprofileEmitWindow(gStage, gBaselineStageFrame, stageFrame);",
            "ItemUpdateSubprofileCancelWindow();",
        ):
            self.assertIn(hook, self.perf)
        self.assertEqual(unguarded_probe_lines(self.perf, "ItemUpdateSubprofile"), [])
        # One record per parent window, on the same boundary as DRAW_PRIO.
        self.assertLess(
            self.perf.index("DrawPrioritySubprofileEmitWindow("),
            self.perf.index("ItemUpdateSubprofileEmitWindow("),
        )
        self.assertIn('"ITEM_UPD V1 st=%ld sf=%lu-%lu ticks=%lu rot=32 "', self.probe)
        self.assertEqual(self.probe.count("BootLog("), 1)
        self.assertNotIn("FlushBootLog", self.probe)
        self.assertIn("sceKernelGetSystemTimeWide", self.probe)
        # Field/argument parity of the single record.
        fmt = re.search(r'BootLog\(\s*((?:"[^"]*"\s*)+),', self.probe).group(1)
        fmt_text = "".join(re.findall(r'"([^"]*)"', fmt))
        specifiers = re.findall(r"%(?:0\d)?(?:ll|l)?[dux]", fmt_text)
        # st, sf x2, ticks, items x3, sampled, timer_reads, cr, ov, uf,
        # 4 sections x3, resid, 6 branch counters, 6 state counters = 37.
        self.assertEqual(len(specifiers), 37)
        self.assertEqual(fmt_text.count("%"), 37)
        # Every specifier has exactly one static_cast argument after the format.
        call = function_body(self.probe, "void ItemUpdateSubprofileEmitWindow(")
        self.assertEqual(call.count("static_cast<"), 37)


if __name__ == "__main__":
    unittest.main()
