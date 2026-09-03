#!/usr/bin/env python3
"""Host-model and source-contract tests for the exact Item velocity cache."""

from __future__ import annotations

import math
from pathlib import Path
import random
import struct
import unittest


ROOT = Path(__file__).resolve().parents[1]
ITEM_CPP = ROOT / "src" / "ItemManager.cpp"
ITEM_HPP = ROOT / "src" / "ItemManager.hpp"
MAKEFILE = ROOT / "Makefile.psp"
PSP_MAIN = ROOT / "psp" / "main.cpp"
TELEMETRY = ROOT / "psp" / "memory_telemetry.cpp"


def f32(value: float) -> float:
    return struct.unpack("<f", struct.pack("<f", value))[0]


def bits(value: float) -> int:
    return struct.unpack("<I", struct.pack("<f", f32(value)))[0]


def from_bits(value: int) -> float:
    return struct.unpack("<f", struct.pack("<I", value))[0]


def canonical(angle: float, speed: float) -> tuple[int, int]:
    # The model checks cache control and exact bit transport.  Runtime replay
    # audit remains authoritative for PSP's X87-compatible libm result.
    return bits(math.cos(f32(angle)) * f32(speed)), bits(
        math.sin(f32(angle)) * f32(speed)
    )


class SlotCache:
    CAPACITY = 1024
    INVALID_TAG = 0xFF

    def __init__(self) -> None:
        self.entries = [(0, 0, 0, 0)] * self.CAPACITY
        self.tags = [self.INVALID_TAG] * self.CAPACITY
        self.lookups = 0
        self.hits = 0
        self.misses = 0
        self.conflicts = 0

    def apply(self, slot: int, angle: float, speed: float, z_bits: int) -> tuple[int, int, int]:
        self.lookups += 1
        angle_bits = bits(angle)
        speed_bits = bits(speed)
        if slot < 0 or slot >= 2096:
            self.misses += 1
            x_bits, y_bits = canonical(angle, speed)
            return x_bits, y_bits, z_bits

        index = slot & (self.CAPACITY - 1)
        tag = slot >> 10
        old_tag = self.tags[index]
        old_angle, old_speed, old_x, old_y = self.entries[index]
        if old_tag == tag and old_angle == angle_bits and old_speed == speed_bits:
            self.hits += 1
            return old_x, old_y, z_bits

        self.misses += 1
        if old_tag != self.INVALID_TAG and old_tag != tag:
            self.conflicts += 1
        x_bits, y_bits = canonical(angle, speed)
        self.entries[index] = (angle_bits, speed_bits, x_bits, y_bits)
        self.tags[index] = tag
        return x_bits, y_bits, z_bits


class ItemAutocollectCacheModelTest(unittest.TestCase):
    def test_cold_then_exact_hit(self) -> None:
        cache = SlotCache()
        z = bits(-7.25)
        first = cache.apply(17, 0.375, 6.0, z)
        second = cache.apply(17, 0.375, 6.0, z)
        self.assertEqual(first, second)
        self.assertEqual((cache.lookups, cache.hits, cache.misses), (2, 1, 1))

    def test_z_is_never_overwritten(self) -> None:
        cache = SlotCache()
        cache.apply(4, 1.0, 5.0, bits(2.0))
        result = cache.apply(4, 1.0, 5.0, bits(-0.0))
        self.assertEqual(result[2], bits(-0.0))

    def test_raw_key_distinguishes_adjacent_and_signed_zero(self) -> None:
        cache = SlotCache()
        angle = f32(0.5)
        adjacent = from_bits(bits(angle) + 1)
        cache.apply(23, angle, 4.0, 0)
        cache.apply(23, adjacent, 4.0, 0)
        cache.apply(23, adjacent, from_bits(bits(4.0) + 1), 0)
        cache.apply(24, 0.0, 4.0, 0)
        cache.apply(24, -0.0, 4.0, 0)
        self.assertEqual(cache.hits, 0)

    def test_alias_conflict_falls_back_canonical(self) -> None:
        cache = SlotCache()
        a = cache.apply(0, 0.25, 3.0, 0)
        b = cache.apply(1024, -0.5, 7.0, 0)
        c = cache.apply(0, 0.25, 3.0, 0)
        self.assertEqual(a, c)
        self.assertNotEqual(a[:2], b[:2])
        self.assertEqual((cache.hits, cache.misses, cache.conflicts), (0, 3, 2))

    def test_third_tag_at_pool_tail_is_distinct(self) -> None:
        cache = SlotCache()
        for slot in (47, 1071, 2095):
            cache.apply(slot, 0.75, 8.0, 0)
        self.assertEqual(cache.conflicts, 2)
        self.assertEqual(cache.hits, 0)

    def test_randomized_results_equal_canonical(self) -> None:
        cache = SlotCache()
        rng = random.Random(0x8142A53D)
        last: dict[int, tuple[float, float]] = {}
        for _ in range(25000):
            slot = rng.randrange(2096)
            if slot in last and rng.randrange(4) != 0:
                angle, speed = last[slot]
            else:
                angle = f32(rng.uniform(-math.pi, math.pi))
                speed = f32(rng.uniform(0.0, 12.0))
                last[slot] = (angle, speed)
            result = cache.apply(slot, angle, speed, bits(9.0))
            self.assertEqual(result[:2], canonical(angle, speed))
        self.assertEqual(cache.lookups, cache.hits + cache.misses)
        self.assertGreater(cache.hits, 0)


class ItemAutocollectCacheSourceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = ITEM_CPP.read_text(encoding="utf-8")
        cls.header = ITEM_HPP.read_text(encoding="utf-8")
        cls.makefile = MAKEFILE.read_text(encoding="utf-8")
        cls.main = PSP_MAIN.read_text(encoding="utf-8")
        cls.telemetry = TELEMETRY.read_text(encoding="utf-8")

    def test_cache_is_psp_only_and_default_off(self) -> None:
        feature = "TH08_PSP_ITEM_AUTOCOLLECT_CACHE"
        self.assertIn(f"{feature} ?= 0", self.makefile)
        self.assertIn(f"-D{feature}=1", self.makefile)
        self.assertIn("ITEM_AUTOCOLLECT_CACHE_CONFIG_STAMP", self.makefile)
        self.assertIn("ITEM_AUTOCOLLECT_CACHE=%d", self.main)
        self.assertIn("defined(PSP)", self.source)

    def test_audit_and_product_cache_are_mutually_exclusive(self) -> None:
        self.assertIn(
            "TH08_PSP_ITEM_AUTOCOLLECT_CACHE and "
            "TH08_PSP_ITEM_AUTOCOLLECT_CACHE_AUDIT are mutually exclusive",
            self.makefile,
        )

    def test_geometry_is_fixed_and_under_eighteen_kib(self) -> None:
        self.assertIn("kItemAutocollectCacheCapacity = 1024U", self.source)
        self.assertIn(
            "g_ItemAutocollectCacheEntries[kItemAutocollectCacheCapacity]",
            self.source,
        )
        self.assertIn(
            "g_ItemAutocollectCacheSlotTags[kItemAutocollectCacheCapacity]",
            self.source,
        )
        self.assertIn("total_bytes=17408", self.telemetry)

    def test_hit_requires_slot_tag_and_both_raw_inputs(self) -> None:
        self.assertIn(
            "g_ItemAutocollectCacheSlotTags[cacheIndex] == slotTag",
            self.source,
        )
        self.assertIn("entry.angleBits == angleBits", self.source)
        self.assertIn("entry.speedBits == speedBits", self.source)

    def test_hit_restores_xy_bits_and_miss_is_canonical(self) -> None:
        self.assertIn(
            "memcpy(&item->startPositionOrVelocity.x, &entry.velocityXBits",
            self.source,
        )
        self.assertIn(
            "memcpy(&item->startPositionOrVelocity.y, &entry.velocityYBits",
            self.source,
        )
        self.assertGreaterEqual(
            self.source.count(
                "item->startPositionOrVelocity.FromAngleMagnitude(angle, speed);"
            ),
            2,
        )

    def test_replay_audit_exposes_hit_and_conflict_counts(self) -> None:
        self.assertIn("struct ItemAutocollectCacheStats", self.header)
        self.assertIn("ITEM_AUTOCOLLECT_CACHE kind=%s", self.telemetry)
        for field in ("lookups=%lu", "hits=%lu", "misses=%lu", "conflicts=%lu"):
            self.assertIn(field, self.telemetry)


if __name__ == "__main__":
    unittest.main(verbosity=2)
