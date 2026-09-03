#!/usr/bin/env python3
"""Static and independent-TU gates for canonical radial trig reuse."""

from __future__ import annotations

import re
import shutil
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
EFFECT = (ROOT / "src/EffectManager.cpp").read_text(encoding="utf-8")
HEADER = (ROOT / "psp/radial_trig_reuse.hpp").read_text(encoding="utf-8")
MAKEFILE = (ROOT / "Makefile.psp").read_text(encoding="utf-8")
MAIN = (ROOT / "psp/main.cpp").read_text(encoding="utf-8")
PLAYER_BOMB = (ROOT / "src/PlayerBomb.cpp").read_text(encoding="utf-8")
ZUN_MATH = (ROOT / "src/ZunMath.hpp").read_text(encoding="utf-8")
FEATURE = "TH08_PSP_RADIAL_TRAIL_TRIG_REUSE"


def zero_fill_storage_bytes(size_output: str) -> int:
    """Return every .bss/.sbss contribution, including named subsections."""
    return sum(
        int(match.group(1))
        for match in re.finditer(
            r"^\.(?:bss|sbss)(?:\.[^\s]+)?\s+(\d+)\b",
            size_output,
            re.MULTILINE,
        )
    )


def fixed_storage_symbols(nm_output: str) -> list[str]:
    """Find BSS/common/small-BSS symbols in a defined-only nm listing."""
    storage_types = frozenset("BbCcSs")
    matches: list[str] = []
    for line in nm_output.splitlines():
        match = re.match(
            r"^\s*[0-9a-fA-F]+\s+(?:[0-9a-fA-F]+\s+)?([A-Za-z?])\s+",
            line,
        )
        if match and match.group(1) in storage_types:
            matches.append(line)
    return matches


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening : index + 1]
    raise AssertionError(f"unterminated function: {signature}")


HARNESS = r"""
#include "psp/radial_trig_reuse.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

struct VertexPair
{
    float outer[3];
    float inner[3];
};

__attribute__((noinline)) float CanonicalCosMul(float angle, float magnitude)
{
    return static_cast<float>(
        std::cos(static_cast<double>(angle)) * static_cast<double>(magnitude));
}

__attribute__((noinline)) float CanonicalSinMul(float angle, float magnitude)
{
    return static_cast<float>(
        std::sin(static_cast<double>(angle)) * static_cast<double>(magnitude));
}

void FinishVertex(float *vertex, float centerX, float centerY,
                  float arcadeX, float arcadeY)
{
    vertex[0] += centerX;
    vertex[1] += centerY;
    vertex[2] += 0.0f;
    vertex[0] += arcadeX;
    vertex[1] += arcadeY;
}

VertexPair Canonical(float angle, float outerRadius, float innerRadius,
                     float centerX, float centerY, float arcadeX, float arcadeY)
{
    VertexPair result{};
    result.outer[2] = 0.0f;
    result.outer[0] = CanonicalCosMul(angle, outerRadius);
    result.outer[1] = CanonicalSinMul(angle, outerRadius);
    FinishVertex(result.outer, centerX, centerY, arcadeX, arcadeY);
    result.inner[2] = 0.0f;
    result.inner[0] = CanonicalCosMul(angle, innerRadius);
    result.inner[1] = CanonicalSinMul(angle, innerRadius);
    FinishVertex(result.inner, centerX, centerY, arcadeX, arcadeY);
    return result;
}

VertexPair Reused(float angle, float outerRadius, float innerRadius,
                  float centerX, float centerY, float arcadeX, float arcadeY)
{
    VertexPair result{};
    result.outer[2] = 0.0f;
    th08::psp::CanonicalRadialSinCos trig;
    trig.cosine = th08::psp::EvaluateCanonicalRadialCos(angle);
    result.outer[0] = th08::psp::CanonicalRadialCosMul(trig, outerRadius);
    trig.sine = th08::psp::EvaluateCanonicalRadialSin(angle);
    result.outer[1] = th08::psp::CanonicalRadialSinMul(trig, outerRadius);
    FinishVertex(result.outer, centerX, centerY, arcadeX, arcadeY);
    result.inner[2] = 0.0f;
    result.inner[0] = th08::psp::CanonicalRadialCosMul(trig, innerRadius);
    result.inner[1] = th08::psp::CanonicalRadialSinMul(trig, innerRadius);
    FinishVertex(result.inner, centerX, centerY, arcadeX, arcadeY);
    return result;
}

VertexPair NaiveFloatTrig(float angle, float outerRadius, float innerRadius,
                          float centerX, float centerY,
                          float arcadeX, float arcadeY)
{
    // Deliberately wrong candidate used only to prove that this byte audit
    // rejects rounding sin/cos to binary32 before the radius multiply.
    const float cosine = static_cast<float>(
        std::cos(static_cast<double>(angle)));
    const float sine = static_cast<float>(
        std::sin(static_cast<double>(angle)));
    VertexPair result{};
    result.outer[2] = 0.0f;
    result.outer[0] = cosine * outerRadius;
    result.outer[1] = sine * outerRadius;
    FinishVertex(result.outer, centerX, centerY, arcadeX, arcadeY);
    result.inner[2] = 0.0f;
    result.inner[0] = cosine * innerRadius;
    result.inner[1] = sine * innerRadius;
    FinishVertex(result.inner, centerX, centerY, arcadeX, arcadeY);
    return result;
}

std::uint32_t Next(std::uint32_t *state)
{
    std::uint32_t value = *state;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    *state = value;
    return value;
}

float Range(std::uint32_t *state, float magnitude)
{
    const std::int32_t signedValue = static_cast<std::int32_t>(Next(state));
    return static_cast<float>(signedValue) * (magnitude / 2147483648.0f);
}

bool Check(float angle, float outerRadius, float innerRadius,
           float centerX, float centerY, float arcadeX, float arcadeY,
           std::uint32_t index)
{
    const VertexPair canonical = Canonical(
        angle, outerRadius, innerRadius, centerX, centerY, arcadeX, arcadeY);
    const VertexPair reused = Reused(
        angle, outerRadius, innerRadius, centerX, centerY, arcadeX, arcadeY);
    if (std::memcmp(&canonical, &reused, sizeof(canonical)) == 0)
        return true;
    const auto *left = reinterpret_cast<const unsigned char *>(&canonical);
    const auto *right = reinterpret_cast<const unsigned char *>(&reused);
    for (std::size_t byte = 0; byte < sizeof(canonical); ++byte)
    {
        if (left[byte] != right[byte])
        {
            std::fprintf(stderr,
                         "mismatch sample=%u byte=%zu canonical=%02x reused=%02x\n",
                         index, byte, left[byte], right[byte]);
            break;
        }
    }
    return false;
}

int main()
{
    constexpr float pi = 3.14159265358979323846f;
    const float fixedAngles[] = {
        -pi, -pi / 2.0f, -pi / 8.0f, -0.0f, 0.0f,
        pi / 8.0f, pi / 2.0f, pi,
    };
    std::uint32_t sample = 0;
    bool naiveFloatTrigMismatchWitnessed = false;
    for (float angle : fixedAngles)
    {
        if (!Check(angle, 384.0f, 192.0f, -64.5f, 224.25f,
                   32.0f, 16.0f, sample++))
            return 1;
        if (!Check(angle, -0.0f, 0.0f, 0.0f, -0.0f,
                   0.0f, 0.0f, sample++))
            return 1;
        const VertexPair canonical = Canonical(
            angle, 123.456f, -78.25f, 0.0f, 0.0f, 0.0f, 0.0f);
        const VertexPair naive = NaiveFloatTrig(
            angle, 123.456f, -78.25f, 0.0f, 0.0f, 0.0f, 0.0f);
        naiveFloatTrigMismatchWitnessed |=
            std::memcmp(&canonical, &naive, sizeof(canonical)) != 0;
    }

    std::uint32_t rng = 0x52414438U;
    for (std::uint32_t i = 0; i < 500000U; ++i)
    {
        const float angle = Range(&rng, pi);
        const float outer = Range(&rng, 2048.0f);
        const float inner = Range(&rng, 2048.0f);
        const float centerX = Range(&rng, 1024.0f);
        const float centerY = Range(&rng, 1024.0f);
        const float arcadeX = Range(&rng, 64.0f);
        const float arcadeY = Range(&rng, 64.0f);
        if (!Check(angle, outer, inner, centerX, centerY,
                   arcadeX, arcadeY, sample++))
            return 1;
        const VertexPair canonical = Canonical(
            angle, outer, inner, centerX, centerY, arcadeX, arcadeY);
        const VertexPair naive = NaiveFloatTrig(
            angle, outer, inner, centerX, centerY, arcadeX, arcadeY);
        naiveFloatTrigMismatchWitnessed |=
            std::memcmp(&canonical, &naive, sizeof(canonical)) != 0;
    }
    if (!naiveFloatTrigMismatchWitnessed)
    {
        std::fprintf(stderr, "audit sensitivity failure: no f32 trig mismatch\n");
        return 2;
    }
    std::printf(
        "RADIAL_TRIG_REUSE_BYTE_AUDIT PASS samples=%u bytes_per_sample=%zu "
        "naive_f32_witness=1\n",
        sample, sizeof(VertexPair));
    return 0;
}
"""


STORAGE_TU = r"""
#include "psp/radial_trig_reuse.hpp"

#include <cmath>

extern "C" void BuildRadialPair(float angle, float outer, float inner,
                                 float *output)
{
#if defined(TH08_PSP_RADIAL_TRIG_REUSE) && TH08_PSP_RADIAL_TRIG_REUSE
    th08::psp::CanonicalRadialSinCos trig;
    trig.cosine = th08::psp::EvaluateCanonicalRadialCos(angle);
    output[0] = th08::psp::CanonicalRadialCosMul(trig, outer);
    trig.sine = th08::psp::EvaluateCanonicalRadialSin(angle);
    output[1] = th08::psp::CanonicalRadialSinMul(trig, outer);
    output[2] = th08::psp::CanonicalRadialCosMul(trig, inner);
    output[3] = th08::psp::CanonicalRadialSinMul(trig, inner);
#else
    output[0] = static_cast<float>(
        std::cos(static_cast<double>(angle)) * static_cast<double>(outer));
    output[1] = static_cast<float>(
        std::sin(static_cast<double>(angle)) * static_cast<double>(outer));
    output[2] = static_cast<float>(
        std::cos(static_cast<double>(angle)) * static_cast<double>(inner));
    output[3] = static_cast<float>(
        std::sin(static_cast<double>(angle)) * static_cast<double>(inner));
#endif
}
"""


class RadialTrigReuseStaticTests(unittest.TestCase):
    def test_default_off_stamp_rebuild_and_boot_identity(self) -> None:
        self.assertIn(f"{FEATURE} ?= 0", MAKEFILE)
        self.assertIn("radial-trail-trig-reuse-0.stamp", MAKEFILE)
        self.assertIn("radial-trail-trig-reuse-1.stamp", MAKEFILE)
        self.assertIn(f"-D{FEATURE}=1", MAKEFILE)
        self.assertIn(f"$(error {FEATURE} must be 0 or 1)", MAKEFILE)
        self.assertIn(
            "src/EffectManager.o psp/main.o: $(RADIAL_TRAIL_TRIG_REUSE_CONFIG_STAMP)",
            MAKEFILE,
        )
        self.assertIn("TH08_PSP_FEATURE_RADIAL_TRAIL_TRIG_REUSE", MAIN)
        self.assertIn("RADIAL_TRAIL_TRIG_REUSE=%d", MAIN)

    def test_only_zero_secondary_draw_branch_changes(self) -> None:
        draw = function_body(
            EFFECT, "i32 __fastcall DrawRadialTrail(Effect *effect)\n{"
        )
        zero_start = draw.index("if (effect->secondaryRadius == 0.0f)")
        ellipse_start = draw.index("else if (effect->radialWaveCount == 0.0f)")
        zero_branch = draw[zero_start:ellipse_start]
        remaining_branches = draw[ellipse_start:]
        self.assertIn(FEATURE, zero_branch)
        self.assertNotIn(FEATURE, remaining_branches)
        self.assertEqual(zero_branch.count("EvaluateCanonicalRadialCos(angle)"), 1)
        self.assertEqual(zero_branch.count("EvaluateCanonicalRadialSin(angle)"), 1)
        self.assertEqual(zero_branch.count("CanonicalRadialCosMul(radialTrig"), 2)
        self.assertEqual(zero_branch.count("CanonicalRadialSinMul(radialTrig"), 2)
        self.assertEqual(zero_branch.count("FromAngleMagnitude(angle"), 2)
        first_z = zero_branch.index("vertex->pos.z = 0.0f;")
        capture_cos = zero_branch.index("EvaluateCanonicalRadialCos(angle)")
        outer_x = zero_branch.index("CanonicalRadialCosMul(radialTrig, radius)")
        capture_sin = zero_branch.index("EvaluateCanonicalRadialSin(angle)")
        outer_y = zero_branch.index("CanonicalRadialSinMul(radialTrig, radius)")
        self.assertLess(first_z, capture_cos)
        self.assertLess(capture_cos, outer_x)
        self.assertLess(outer_x, capture_sin)
        self.assertLess(capture_sin, outer_y)
        self.assertEqual(draw.count("g_AnmManager->DrawVertices("), 1)

    def test_gameplay_and_shared_math_are_not_feature_consumers(self) -> None:
        self.assertNotIn(FEATURE, PLAYER_BOMB)
        self.assertNotIn(FEATURE, ZUN_MATH)
        self.assertNotIn("RenderSinCos", HEADER)
        header_code = "\n".join(
            line for line in HEADER.splitlines()
            if not line.lstrip().startswith("//")
        )
        self.assertNotIn("vfpu", header_code.lower())
        for forbidden in ("Rng", "Replay", "Player", "Bullet", "Effect"):
            self.assertNotIn(forbidden, header_code)

    def test_helper_has_no_runtime_storage_or_allocation(self) -> None:
        header_code = "\n".join(
            line for line in HEADER.splitlines()
            if not line.lstrip().startswith("//")
        )
        for forbidden in (
            "static ", "thread_local", "malloc", "calloc", "realloc", "new ",
            "std::vector", "std::array", "volatile",
        ):
            self.assertNotIn(forbidden, header_code)
        self.assertIn("std::cos(static_cast<double>(angle))", HEADER)
        self.assertIn("std::sin(static_cast<double>(angle))", HEADER)
        self.assertIn("trig.cosine * static_cast<double>(magnitude)", HEADER)
        self.assertIn("trig.sine * static_cast<double>(magnitude)", HEADER)


class RadialTrigReuseIndependentTuTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.compiler = shutil.which("c++")
        cls.size_tool = shutil.which("size")
        cls.nm_tool = shutil.which("nm")
        cls.psp_compiler = shutil.which("psp-g++")
        cls.psp_size_tool = shutil.which("psp-size")
        cls.psp_nm_tool = shutil.which("psp-nm")
        if cls.compiler is None or cls.size_tool is None or cls.nm_tool is None:
            raise unittest.SkipTest("host C++ compiler, size, and nm are required")

    def test_full_vertex_pair_is_byte_identical(self) -> None:
        with tempfile.TemporaryDirectory(prefix="th08-radial-trig-") as temp:
            source = Path(temp) / "radial_byte_audit.cpp"
            executable = Path(temp) / "radial_byte_audit"
            source.write_text(textwrap.dedent(HARNESS), encoding="utf-8")
            subprocess.run(
                [
                    self.compiler,
                    "-std=c++17",
                    "-O2",
                    "-fno-fast-math",
                    "-fno-exceptions",
                    "-fno-rtti",
                    "-I",
                    str(ROOT),
                    str(source),
                    "-o",
                    str(executable),
                ],
                check=True,
                cwd=ROOT,
            )
            completed = subprocess.run(
                [str(executable)], check=True, text=True, capture_output=True
            )
            self.assertIn("RADIAL_TRIG_REUSE_BYTE_AUDIT PASS", completed.stdout)
            self.assertIn("samples=500016", completed.stdout)
            self.assertIn("bytes_per_sample=24", completed.stdout)
            self.assertIn("naive_f32_witness=1", completed.stdout)

    def test_off_on_objects_add_zero_bss(self) -> None:
        with tempfile.TemporaryDirectory(prefix="th08-radial-storage-") as temp:
            source = Path(temp) / "radial_storage.cpp"
            source.write_text(textwrap.dedent(STORAGE_TU), encoding="utf-8")
            bss_sizes: dict[int, int] = {}
            for enabled in (0, 1):
                obj = Path(temp) / f"radial-{enabled}.o"
                subprocess.run(
                    [
                        self.compiler,
                        "-std=c++17",
                        "-O2",
                        "-fno-fast-math",
                        "-I",
                        str(ROOT),
                        f"-D{FEATURE}={enabled}",
                        "-c",
                        str(source),
                        "-o",
                        str(obj),
                    ],
                    check=True,
                    cwd=ROOT,
                )
                sections = subprocess.run(
                    [self.size_tool, "-A", str(obj)],
                    check=True,
                    text=True,
                    capture_output=True,
                ).stdout
                bss_sizes[enabled] = zero_fill_storage_bytes(sections)
                symbols = subprocess.run(
                    [self.nm_tool, "-S", "--defined-only", str(obj)],
                    check=True,
                    text=True,
                    capture_output=True,
                ).stdout
                self.assertEqual(fixed_storage_symbols(symbols), [])
            self.assertEqual(bss_sizes, {0: 0, 1: 0})

    def test_psp_target_off_on_objects_compile_and_add_zero_bss(self) -> None:
        if (
            self.psp_compiler is None
            or self.psp_size_tool is None
            or self.psp_nm_tool is None
        ):
            self.skipTest("PSPSDK compiler, size, and nm are not available")
        with tempfile.TemporaryDirectory(prefix="th08-radial-psp-storage-") as temp:
            source = Path(temp) / "radial_storage.cpp"
            source.write_text(textwrap.dedent(STORAGE_TU), encoding="utf-8")
            bss_sizes: dict[int, int] = {}
            for enabled in (0, 1):
                obj = Path(temp) / f"radial-psp-{enabled}.o"
                subprocess.run(
                    [
                        self.psp_compiler,
                        "-std=gnu++17",
                        "-O2",
                        "-G0",
                        "-march=allegrex",
                        "-mtune=allegrex",
                        "-fno-fast-math",
                        "-fno-exceptions",
                        "-fno-rtti",
                        "-I",
                        str(ROOT),
                        f"-D{FEATURE}={enabled}",
                        "-c",
                        str(source),
                        "-o",
                        str(obj),
                    ],
                    check=True,
                    cwd=ROOT,
                )
                sections = subprocess.run(
                    [self.psp_size_tool, "-A", str(obj)],
                    check=True,
                    text=True,
                    capture_output=True,
                ).stdout
                bss_sizes[enabled] = zero_fill_storage_bytes(sections)
                symbols = subprocess.run(
                    [self.psp_nm_tool, "-S", "--defined-only", str(obj)],
                    check=True,
                    text=True,
                    capture_output=True,
                ).stdout
                self.assertEqual(fixed_storage_symbols(symbols), [])
            self.assertEqual(bss_sizes, {0: 0, 1: 0})


if __name__ == "__main__":
    unittest.main()
