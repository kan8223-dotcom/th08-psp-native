#!/usr/bin/env python3
"""Host and static tests for TH08_PSP_ITEM_AUTOCOLLECT_CACHE_AUDIT.

The PSP audit is deliberately observational: the canonical atan2/cos/sin
result is stored first, then an exact-bit per-slot cache measures how often a
future implementation could reuse that already-proven result.  The host model
exercises the key, lifecycle, slot boundaries, and mismatch detector.  Static
checks bind those properties to the production ItemManager call site.
"""

from __future__ import annotations

import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


REPO_ROOT = Path(__file__).resolve().parents[1]
ITEM_CPP = REPO_ROOT / "src" / "ItemManager.cpp"
ITEM_HPP = REPO_ROOT / "src" / "ItemManager.hpp"
MAKEFILE_PSP = REPO_ROOT / "Makefile.psp"
PSP_MAIN = REPO_ROOT / "psp" / "main.cpp"
MEMORY_TELEMETRY = REPO_ROOT / "psp" / "memory_telemetry.cpp"


HARNESS_SOURCE = r"""
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>

namespace {

constexpr int kItemCount = 2096;

struct Velocity {
    float x = 0.0f;
    float y = 0.0f;
};

struct Entry {
    std::uint32_t angleBits = 0;
    std::uint32_t speedBits = 0;
    std::uint32_t velocityXBits = 0;
    std::uint32_t velocityYBits = 0;
};

struct Stats {
    std::uint32_t canonicalCalls = 0;
    std::uint32_t exactInputRepeats = 0;
    std::uint32_t exactOutputMatches = 0;
    std::uint32_t exactOutputMismatches = 0;
    std::uint32_t invalidSlotObservations = 0;
};

struct Audit {
    std::array<Entry, kItemCount> entries{};
    std::array<std::uint32_t, (kItemCount + 31) / 32> validBits{};
    Stats stats{};

    void Reset() {
        entries.fill(Entry{});
        validBits.fill(0U);
        stats = Stats{};
    }
};

[[noreturn]] void Fail(const std::string& message) {
    std::cerr << "item-autocollect-cache-audit: FAIL " << message << '\n';
    std::exit(1);
}

void Require(bool condition, const std::string& message) {
    if (!condition) {
        Fail(message);
    }
}

std::uint32_t FloatBits(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

float FloatFromBits(std::uint32_t bits) {
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

Velocity CanonicalVelocity(float angle, float speed) {
    Velocity result;
    result.x = static_cast<float>(
        std::cos(static_cast<double>(angle)) * static_cast<double>(speed));
    result.y = static_cast<float>(
        std::sin(static_cast<double>(angle)) * static_cast<double>(speed));
    return result;
}

void Observe(Audit& audit, int slot, float angle, float speed,
             const Velocity& velocity) {
    ++audit.stats.canonicalCalls;
    if (slot < 0 || slot >= kItemCount) {
        ++audit.stats.invalidSlotObservations;
        return;
    }

    const std::uint32_t angleBits = FloatBits(angle);
    const std::uint32_t speedBits = FloatBits(speed);
    const std::uint32_t velocityXBits = FloatBits(velocity.x);
    const std::uint32_t velocityYBits = FloatBits(velocity.y);
    const std::uint32_t validMask = 1U << (slot & 31);
    Entry& entry = audit.entries[slot];

    if ((audit.validBits[slot >> 5] & validMask) != 0U &&
        entry.angleBits == angleBits && entry.speedBits == speedBits) {
        ++audit.stats.exactInputRepeats;
        if (entry.velocityXBits == velocityXBits &&
            entry.velocityYBits == velocityYBits) {
            ++audit.stats.exactOutputMatches;
        } else {
            ++audit.stats.exactOutputMismatches;
        }
    }

    entry.angleBits = angleBits;
    entry.speedBits = speedBits;
    entry.velocityXBits = velocityXBits;
    entry.velocityYBits = velocityYBits;
    audit.validBits[slot >> 5] |= validMask;
}

void ObserveCanonical(Audit& audit, int slot, float angle, float speed) {
    const Velocity velocity = CanonicalVelocity(angle, speed);
    const std::uint32_t beforeX = FloatBits(velocity.x);
    const std::uint32_t beforeY = FloatBits(velocity.y);
    Observe(audit, slot, angle, speed, velocity);
    Require(FloatBits(velocity.x) == beforeX && FloatBits(velocity.y) == beforeY,
            "observer mutated canonical result");
}

void ScenarioRepeatedInput() {
    Audit audit;
    for (int count = 0; count < 5; ++count) {
        ObserveCanonical(audit, 7, 0.375f, 6.0f);
    }
    Require(audit.stats.canonicalCalls == 5, "repeated call count");
    Require(audit.stats.exactInputRepeats == 4, "repeated hit count");
    Require(audit.stats.exactOutputMatches == 4, "repeated match count");
    Require(audit.stats.exactOutputMismatches == 0,
            "repeated mismatch count");
}

void ScenarioExactBitKey() {
    Audit audit;
    const float angle = 0.5f;
    const float adjacentAngle =
        std::nextafter(angle, std::numeric_limits<float>::infinity());
    const float speed = 4.0f;
    const float adjacentSpeed =
        std::nextafter(speed, std::numeric_limits<float>::infinity());

    ObserveCanonical(audit, 12, angle, speed);          // cold
    ObserveCanonical(audit, 12, adjacentAngle, speed);  // angle miss
    ObserveCanonical(audit, 12, adjacentAngle, speed);  // exact hit
    ObserveCanonical(audit, 12, adjacentAngle, adjacentSpeed); // speed miss
    ObserveCanonical(audit, 13, 0.0f, speed);           // cold
    ObserveCanonical(audit, 13, -0.0f, speed);          // signed-zero miss

    Require(audit.stats.canonicalCalls == 6, "exact-bit call count");
    Require(audit.stats.exactInputRepeats == 1, "exact-bit hit count");
    Require(audit.stats.exactOutputMatches == 1, "exact-bit match count");
}

void ScenarioSlotBoundaries() {
    Audit audit;
    const int slots[] = {0, 31, 32, 127, 128, 2047, 2095};
    for (int slot : slots) {
        ObserveCanonical(audit, slot, 1.25f, 5.5f);
    }
    for (int slot : slots) {
        ObserveCanonical(audit, slot, 1.25f, 5.5f);
    }
    Require(audit.stats.canonicalCalls == 14, "boundary call count");
    Require(audit.stats.exactInputRepeats == 7, "boundary hit count");
    Require(audit.stats.exactOutputMatches == 7, "boundary match count");
    Require(audit.stats.invalidSlotObservations == 0,
            "boundary invalid count");
}

void ScenarioInvalidSlots() {
    Audit audit;
    const Velocity velocity = CanonicalVelocity(0.25f, 3.0f);
    Observe(audit, -1, 0.25f, 3.0f, velocity);
    Observe(audit, kItemCount, 0.25f, 3.0f, velocity);
    Require(audit.stats.canonicalCalls == 2, "invalid call count");
    Require(audit.stats.invalidSlotObservations == 2,
            "invalid observation count");
    Require(audit.stats.exactInputRepeats == 0, "invalid false hit");
}

void ScenarioMismatchDetector() {
    Audit audit;
    const float angle = 0.75f;
    const float speed = 7.0f;
    Velocity canonical = CanonicalVelocity(angle, speed);
    Observe(audit, 91, angle, speed, canonical);

    std::uint32_t changedBits = FloatBits(canonical.x) + 1U;
    Velocity changed = canonical;
    changed.x = FloatFromBits(changedBits);
    Observe(audit, 91, angle, speed, changed);

    Require(audit.stats.exactInputRepeats == 1, "mismatch repeat count");
    Require(audit.stats.exactOutputMatches == 0, "mismatch false match");
    Require(audit.stats.exactOutputMismatches == 1,
            "mismatch was not detected");
}

void ScenarioResetLifecycle() {
    Audit audit;
    ObserveCanonical(audit, 2095, -2.0f, 8.0f);
    ObserveCanonical(audit, 2095, -2.0f, 8.0f);
    Require(audit.stats.exactInputRepeats == 1, "pre-reset hit");
    audit.Reset();
    ObserveCanonical(audit, 2095, -2.0f, 8.0f);
    Require(audit.stats.canonicalCalls == 1, "post-reset call count");
    Require(audit.stats.exactInputRepeats == 0, "post-reset stale hit");
}

std::uint32_t gRandomState = 0x8142a53dU;

std::uint32_t NextRandom() {
    gRandomState = gRandomState * 1664525U + 1013904223U;
    return gRandomState;
}

void ScenarioRandomizedAccounting() {
    Audit audit;
    std::array<bool, kItemCount> valid{};
    std::array<std::uint32_t, kItemCount> previousAngle{};
    std::array<std::uint32_t, kItemCount> previousSpeed{};
    std::uint32_t expectedHits = 0;

    for (int step = 0; step < 20000; ++step) {
        const int slot = static_cast<int>(NextRandom() % kItemCount);
        float angle;
        float speed;
        if (valid[slot] && (NextRandom() & 3U) != 0U) {
            angle = FloatFromBits(previousAngle[slot]);
            speed = FloatFromBits(previousSpeed[slot]);
            ++expectedHits;
        } else {
            angle = static_cast<float>(static_cast<int>(NextRandom() % 4097U) -
                                       2048) / 257.0f;
            speed = static_cast<float>((NextRandom() % 1023U) + 1U) / 64.0f;
        }

        ObserveCanonical(audit, slot, angle, speed);
        valid[slot] = true;
        previousAngle[slot] = FloatBits(angle);
        previousSpeed[slot] = FloatBits(speed);
    }

    Require(audit.stats.canonicalCalls == 20000,
            "randomized call count");
    Require(audit.stats.exactInputRepeats == expectedHits,
            "randomized hit accounting");
    Require(audit.stats.exactOutputMatches == expectedHits,
            "randomized output accounting");
    Require(audit.stats.exactOutputMismatches == 0,
            "randomized deterministic mapping mismatch");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        Fail("expected exactly one scenario name");
    }
    const std::string scenario = argv[1];
    if (scenario == "repeated-input") {
        ScenarioRepeatedInput();
    } else if (scenario == "exact-bit-key") {
        ScenarioExactBitKey();
    } else if (scenario == "slot-boundaries") {
        ScenarioSlotBoundaries();
    } else if (scenario == "invalid-slots") {
        ScenarioInvalidSlots();
    } else if (scenario == "mismatch-detector") {
        ScenarioMismatchDetector();
    } else if (scenario == "reset-lifecycle") {
        ScenarioResetLifecycle();
    } else if (scenario == "randomized-accounting") {
        ScenarioRandomizedAccounting();
    } else {
        Fail("unknown scenario: " + scenario);
    }
    std::cout << "item-autocollect-cache-audit: PASS scenario="
              << scenario << '\n';
    return 0;
}
"""


def find_compiler() -> str:
    for candidate in (os.environ.get("CXX"), "g++", "clang++"):
        if candidate:
            resolved = shutil.which(candidate)
            if resolved:
                return resolved
    raise unittest.SkipTest("no host C++17 compiler found")


def function_body(source: str, signature: str) -> str:
    start = source.find(signature)
    if start < 0:
        raise AssertionError(f"missing production function: {signature}")
    brace = source.find("{", start)
    if brace < 0:
        raise AssertionError(f"missing function body: {signature}")
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[brace : index + 1]
    raise AssertionError(f"unterminated function body: {signature}")


class ItemAutocollectCacheAuditHostTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.temporary = tempfile.TemporaryDirectory(prefix="th08-item-cache-audit-")
        output = Path(cls.temporary.name)
        source = output / "item_autocollect_cache_audit_harness.cpp"
        cls.executable = output / "item_autocollect_cache_audit_harness"
        source.write_text(HARNESS_SOURCE, encoding="utf-8")
        command = [
            find_compiler(),
            "-std=c++17",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-pedantic",
            str(source),
            "-o",
            str(cls.executable),
        ]
        result = subprocess.run(command, text=True, capture_output=True)
        if result.returncode != 0:
            raise AssertionError(
                "host harness compilation failed:\n" + result.stdout + result.stderr
            )

    @classmethod
    def tearDownClass(cls) -> None:
        cls.temporary.cleanup()

    def run_scenario(self, name: str) -> None:
        result = subprocess.run(
            [str(self.executable), name], text=True, capture_output=True
        )
        self.assertEqual(
            result.returncode,
            0,
            msg=f"scenario {name} failed:\n{result.stdout}{result.stderr}",
        )
        self.assertIn(f"PASS scenario={name}", result.stdout)

    def test_repeated_input(self) -> None:
        self.run_scenario("repeated-input")

    def test_exact_bit_key_including_signed_zero(self) -> None:
        self.run_scenario("exact-bit-key")

    def test_slot_and_bitset_boundaries(self) -> None:
        self.run_scenario("slot-boundaries")

    def test_invalid_slot_fail_closed(self) -> None:
        self.run_scenario("invalid-slots")

    def test_mismatch_detector(self) -> None:
        self.run_scenario("mismatch-detector")

    def test_reset_lifecycle(self) -> None:
        self.run_scenario("reset-lifecycle")

    def test_randomized_accounting(self) -> None:
        self.run_scenario("randomized-accounting")


class ItemAutocollectCacheAuditStaticTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = ITEM_CPP.read_text(encoding="utf-8")
        cls.header = ITEM_HPP.read_text(encoding="utf-8")
        cls.makefile = MAKEFILE_PSP.read_text(encoding="utf-8")
        cls.main = PSP_MAIN.read_text(encoding="utf-8")
        cls.telemetry = MEMORY_TELEMETRY.read_text(encoding="utf-8")

    def test_feature_gate_and_public_stats(self) -> None:
        gate = "TH08_PSP_ITEM_AUTOCOLLECT_CACHE_AUDIT"
        self.assertIn(gate, self.source)
        self.assertIn(gate, self.header)
        for field in (
            "canonicalCalls",
            "exactInputRepeats",
            "exactOutputMatches",
            "exactOutputMismatches",
            "invalidSlotObservations",
        ):
            self.assertIn(field, self.header)
        self.assertIn("GetItemAutocollectCacheAuditStats", self.header)

    def test_canonical_math_is_unconditionally_executed_first(self) -> None:
        update = function_body(self.source, "void ItemManager::OnUpdate()")
        canonical = update.find(
            "item->startPositionOrVelocity.FromAngleMagnitude("
        )
        observer = update.find("ObserveItemAutocollectCanonicalResult(")
        self.assertGreaterEqual(canonical, 0)
        self.assertGreater(observer, canonical)
        between = update[canonical:observer]
        self.assertNotIn("else", between)
        self.assertNotIn("return", between)
        self.assertEqual(
            update.count("ObserveItemAutocollectCanonicalResult("), 1
        )

    def test_observer_keys_all_inputs_and_both_outputs(self) -> None:
        observer = function_body(
            self.source, "void ObserveItemAutocollectCanonicalResult("
        )
        self.assertIn("entry.angleBits == angleBits", observer)
        self.assertIn("entry.speedBits == speedBits", observer)
        self.assertIn("entry.velocityXBits == velocityXBits", observer)
        self.assertIn("entry.velocityYBits == velocityYBits", observer)
        self.assertIn("itemIndex < 0 || itemIndex >= MAX_ITEMS", observer)
        # The observer may read the canonical velocity but must never alter it.
        self.assertNotRegex(
            observer,
            r"item->startPositionOrVelocity\.(?:x|y)\s*=",
        )

    def test_initialize_resets_audit_lifecycle(self) -> None:
        initialize = function_body(self.source, "void ItemManager::Initialize()")
        self.assertIn("ResetItemAutocollectCacheAudit();", initialize)
        self.assertLess(
            initialize.find("ResetItemAutocollectCacheAudit();"),
            initialize.find("this->itemListTail = &this->itemListHead;"),
        )

    def test_cache_geometry_covers_exact_logical_pool(self) -> None:
        self.assertIn(
            "g_ItemAutocollectCacheAuditEntries[MAX_ITEMS]", self.source
        )
        self.assertIn(
            "g_ItemAutocollectCacheAuditValidBits[(MAX_ITEMS + 31) / 32]",
            self.source,
        )

    def test_zero_value_cannot_enable_hidden_audit_state(self) -> None:
        value_guard = (
            "defined(TH08_PSP_ITEM_AUTOCOLLECT_CACHE_AUDIT) && \\\n"
            "    TH08_PSP_ITEM_AUTOCOLLECT_CACHE_AUDIT"
        )
        self.assertGreaterEqual(self.source.count(value_guard), 3)
        self.assertIn(value_guard, self.header)

    def test_build_fingerprint_stamp_and_telemetry_are_bound(self) -> None:
        feature = "TH08_PSP_ITEM_AUTOCOLLECT_CACHE_AUDIT"
        self.assertIn(f"{feature} ?= 0", self.makefile)
        self.assertIn(f"-D{feature}=1", self.makefile)
        self.assertIn("ITEM_AUTOCOLLECT_CACHE_AUDIT_CONFIG_STAMP", self.makefile)
        for consumer in ("src/ItemManager.o", "psp/memory_telemetry.o", "psp/main.o"):
            self.assertIn(consumer, self.makefile)
        self.assertIn("ITEM_AUTOCOLLECT_CACHE_AUDIT=%d", self.main)
        self.assertIn("ITEM_AUTOCOLLECT_AUDIT kind=%s", self.telemetry)
        for field in (
            "canonical_calls=%lu",
            "exact_input_repeats=%lu",
            "exact_output_matches=%lu",
            "exact_output_mismatches=%lu",
            "invalid_slot_observations=%lu",
        ):
            self.assertIn(field, self.telemetry)


if __name__ == "__main__":
    unittest.main(verbosity=2)
