#!/usr/bin/env python3
"""Compile/run the allocation-free radial-trail activity counters."""

from __future__ import annotations

import shutil
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
EFFECT = (ROOT / "src/EffectManager.cpp").read_text(encoding="utf-8")
MEMORY = (ROOT / "psp/memory_telemetry.cpp").read_text(encoding="utf-8")
MAKEFILE = (ROOT / "Makefile.psp").read_text(encoding="utf-8")

HARNESS = r"""
#include "psp/radial_trail_telemetry.hpp"
#include <cstdio>

int main()
{
    using namespace th08::psp;
    RadialTrailTelemetryReset();
    RadialTrailTelemetryNoteDraw(44);
    RadialTrailTelemetryNoteRebuild(
        44, RadialTrailBranch::ZeroSecondary, false);
    RadialTrailTelemetryNoteDraw(54);
    RadialTrailTelemetryNoteRebuild(
        54, RadialTrailBranch::ZeroSecondary, true);
    RadialTrailTelemetryNoteDraw(32);
    RadialTrailTelemetryNoteRebuild(32, RadialTrailBranch::Wavy, false);
    RadialTrailTelemetryNoteRebuild(24, RadialTrailBranch::Ellipse, false);

    const auto value = RadialTrailTelemetryPeek();
    if (value.drawCalls != 3 || value.submittedVertices != 266 ||
        value.dirtyRebuilds != 4 || value.zeroSecondaryRebuilds != 2 ||
        value.ellipseRebuilds != 1 || value.wavyRebuilds != 1 ||
        value.angularSamples != 158 || value.rebuiltVertices != 316 ||
        value.reusableTrigPairs != 100 || value.reusedTrigPairs != 55 ||
        value.trigEvaluationsAvoided != 110 || value.peakSegments != 54 ||
        value.peakSubmittedVertices != 110)
        return 1;

    const auto taken = RadialTrailTelemetryTake();
    const auto cleared = RadialTrailTelemetryPeek();
    if (taken.trigEvaluationsAvoided != 110 || cleared.drawCalls != 0 ||
        cleared.dirtyRebuilds != 0 || cleared.peakSegments != 0)
        return 2;
    std::printf("RADIAL_TRAIL_TELEMETRY PASS bytes=%zu\n", sizeof(value));
    return 0;
}
"""


class RadialTrailTelemetryTests(unittest.TestCase):
    def test_runtime_integration_is_interval_owned(self) -> None:
        self.assertIn("RadialTrailTelemetryNoteDraw(effect->vertexSegmentCount)", EFFECT)
        self.assertEqual(EFFECT.count("RadialTrailTelemetryNoteRebuild("), 3)
        self.assertIn("RadialTrailBranch::ZeroSecondary", EFFECT)
        self.assertIn("RadialTrailBranch::Ellipse", EFFECT)
        self.assertIn("RadialTrailBranch::Wavy", EFFECT)
        self.assertIn("RADIAL_TRAIL_TELEMETRY kind=%s", MEMORY)
        self.assertIn("RadialTrailTelemetryTake()", MEMORY)
        self.assertIn("RadialTrailTelemetryPeek()", MEMORY)
        self.assertGreaterEqual(MEMORY.count("RadialTrailTelemetryReset();"), 2)
        self.assertIn("psp/radial_trail_telemetry.cpp", MAKEFILE)

    def test_host_and_psp_translation_units(self) -> None:
        host = shutil.which("c++")
        psp = shutil.which("psp-g++")
        if host is None:
            self.skipTest("host compiler unavailable")
        with tempfile.TemporaryDirectory(prefix="th08-radial-telemetry-") as temp:
            source = Path(temp) / "harness.cpp"
            binary = Path(temp) / "harness"
            source.write_text(textwrap.dedent(HARNESS), encoding="utf-8")
            subprocess.run(
                [host, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
                 "-I", str(ROOT), str(source),
                 str(ROOT / "psp/radial_trail_telemetry.cpp"), "-o", str(binary)],
                check=True,
            )
            result = subprocess.run(
                [str(binary)], check=True, text=True, capture_output=True
            )
            self.assertIn("RADIAL_TRAIL_TELEMETRY PASS", result.stdout)
            if psp is not None:
                obj = Path(temp) / "radial_trail_telemetry.o"
                subprocess.run(
                    [psp, "-std=gnu++17", "-O2", "-G0", "-march=allegrex",
                     "-mtune=allegrex", "-I", str(ROOT), "-c",
                     str(ROOT / "psp/radial_trail_telemetry.cpp"), "-o", str(obj)],
                    check=True,
                )


if __name__ == "__main__":
    unittest.main()
