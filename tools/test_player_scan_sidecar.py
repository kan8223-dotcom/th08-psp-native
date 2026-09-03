#!/usr/bin/env python3
"""Differential and static checks for TH08_PSP_PLAYER_SCAN_SIDECAR.

The production implementation is PSP-only and cannot be linked into a normal
host test without the complete game runtime.  This test therefore compiles a
small C++ model of the authoritative full scans and the sidecar scans, then
drives both with identical operations.  Static checks bind that model to every
production writer/repair/lifecycle boundary on which the optimization relies.
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
PLAYER_CPP = REPO_ROOT / "src" / "Player.cpp"
PLAYER_BOMB_CPP = REPO_ROOT / "src" / "PlayerBomb.cpp"


HARNESS_SOURCE = r"""
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr int kRegionCount = 192;
constexpr int kShotCount = 128;

struct Region {
    int active = 0;
    int collisionValue = -1;
    int hitAccumulator = 0;
    int hitCap = 0;
    int damage = 0;
    int overlaps = 0;
    int serial = 0;
};

struct Shot {
    int state = 0;
    int serial = 0;
};

struct Player {
    std::array<Region, kRegionCount> damageRegions{};
    std::array<Region, kRegionCount> cancelRegions{};
    std::array<Shot, kShotCount> shots{};
    int bulletCancelItemType = 6;
};

struct Sidecar {
    Player* owner = nullptr;
    std::array<std::uint32_t, 4> activeShotBits{};
    std::array<std::uint32_t, 6> activeDamageRegionBits{};
    std::array<std::uint32_t, 6> activeCancelRegionBits{};
};

[[noreturn]] void Fail(const std::string& message) {
    std::cerr << "player-scan-sidecar: FAIL " << message << '\n';
    std::exit(1);
}

void Require(bool condition, const std::string& message) {
    if (!condition) {
        Fail(message);
    }
}

void SetBit(std::uint32_t* bits, int index) {
    bits[index >> 5] |= 1U << (index & 31);
}

void ClearBit(std::uint32_t* bits, int index) {
    bits[index >> 5] &= ~(1U << (index & 31));
}

bool TestBit(const std::uint32_t* bits, int index) {
    return (bits[index >> 5] & (1U << (index & 31))) != 0;
}

int NextBit(const std::uint32_t* bits, int wordCount, int previousIndex) {
    int index = previousIndex + 1;
    int wordIndex = index >> 5;
    if (wordIndex >= wordCount) {
        return -1;
    }

    std::uint32_t word = bits[wordIndex] & (~0U << (index & 31));
    while (true) {
        if (word != 0U) {
            return (wordIndex << 5) + __builtin_ctz(word);
        }
        if (++wordIndex >= wordCount) {
            return -1;
        }
        word = bits[wordIndex];
    }
}

std::vector<int> CollectBits(const std::uint32_t* bits, int wordCount) {
    std::vector<int> result;
    for (int index = NextBit(bits, wordCount, -1);
         index >= 0;
         index = NextBit(bits, wordCount, index)) {
        result.push_back(index);
    }
    return result;
}

void ResetSidecar(Sidecar& sidecar, Player& player) {
    sidecar = Sidecar{};
    sidecar.owner = &player;
}

void AuditShots(Sidecar& sidecar, Player& player) {
    sidecar.activeShotBits.fill(0);
    for (int index = 0; index < kShotCount; ++index) {
        if (player.shots[index].state != 0) {
            SetBit(sidecar.activeShotBits.data(), index);
        }
    }
}

void AuditRegions(Sidecar& sidecar, Player& player) {
    sidecar.activeDamageRegionBits.fill(0);
    sidecar.activeCancelRegionBits.fill(0);
    for (int index = 0; index < kRegionCount; ++index) {
        if (player.damageRegions[index].active != 0) {
            SetBit(sidecar.activeDamageRegionBits.data(), index);
        }
        if (player.cancelRegions[index].active != 0) {
            SetBit(sidecar.activeCancelRegionBits.data(), index);
        }
    }
}

void EnsureSidecar(Sidecar& sidecar, Player& player) {
    if (sidecar.owner == &player) {
        return;
    }
    ResetSidecar(sidecar, player);
    AuditShots(sidecar, player);
    AuditRegions(sidecar, player);
}

bool SameRegion(const Region& left, const Region& right) {
    return left.active == right.active &&
           left.collisionValue == right.collisionValue &&
           left.hitAccumulator == right.hitAccumulator &&
           left.hitCap == right.hitCap &&
           left.damage == right.damage &&
           left.overlaps == right.overlaps &&
           left.serial == right.serial;
}

bool SamePlayer(const Player& left, const Player& right) {
    if (left.bulletCancelItemType != right.bulletCancelItemType) {
        return false;
    }
    for (int index = 0; index < kRegionCount; ++index) {
        if (!SameRegion(left.damageRegions[index], right.damageRegions[index]) ||
            !SameRegion(left.cancelRegions[index], right.cancelRegions[index])) {
            return false;
        }
    }
    for (int index = 0; index < kShotCount; ++index) {
        if (left.shots[index].state != right.shots[index].state ||
            left.shots[index].serial != right.shots[index].serial) {
            return false;
        }
    }
    return true;
}

int CanonicalCancel(Player& player, std::vector<int>* trace = nullptr) {
    for (int index = 0; index < kRegionCount; ++index) {
        Region& slot = player.cancelRegions[index];
        if (slot.active == 0) {
            continue;
        }
        if (trace != nullptr) {
            trace->push_back(index);
        }
        if (slot.overlaps == 0) {
            continue;
        }
        player.bulletCancelItemType = slot.collisionValue;
        ++slot.hitAccumulator;
        return 2;
    }
    return 0;
}

int SidecarCancel(Player& player, Sidecar& sidecar,
                  std::vector<int>* trace = nullptr) {
    EnsureSidecar(sidecar, player);
    for (int index = NextBit(sidecar.activeCancelRegionBits.data(), 6, -1);
         index >= 0;
         index = NextBit(sidecar.activeCancelRegionBits.data(), 6, index)) {
        Region& slot = player.cancelRegions[index];
        if (slot.active == 0) {
            ClearBit(sidecar.activeCancelRegionBits.data(), index);
            continue;
        }
        if (trace != nullptr) {
            trace->push_back(index);
        }
        if (slot.overlaps == 0) {
            continue;
        }
        player.bulletCancelItemType = slot.collisionValue;
        ++slot.hitAccumulator;
        return 2;
    }
    return 0;
}

int CanonicalDamage(Player& player, std::vector<int>* trace = nullptr) {
    int total = 0;
    for (int index = 0; index < kRegionCount; ++index) {
        Region& slot = player.damageRegions[index];
        if (slot.active == 0) {
            continue;
        }
        if (trace != nullptr) {
            trace->push_back(index);
        }
        if (slot.overlaps == 0) {
            continue;
        }
        total += slot.damage;
        slot.hitAccumulator += slot.damage;
        if (slot.hitCap > 0 && slot.hitCap <= slot.hitAccumulator) {
            slot.damage = 0;
            total -= slot.hitAccumulator - slot.hitCap;
        }
    }
    return total;
}

int SidecarDamage(Player& player, Sidecar& sidecar,
                  std::vector<int>* trace = nullptr) {
    int total = 0;
    EnsureSidecar(sidecar, player);
    for (int index = NextBit(sidecar.activeDamageRegionBits.data(), 6, -1);
         index >= 0;
         index = NextBit(sidecar.activeDamageRegionBits.data(), 6, index)) {
        Region& slot = player.damageRegions[index];
        if (slot.active == 0) {
            ClearBit(sidecar.activeDamageRegionBits.data(), index);
            continue;
        }
        if (trace != nullptr) {
            trace->push_back(index);
        }
        if (slot.overlaps == 0) {
            continue;
        }
        total += slot.damage;
        slot.hitAccumulator += slot.damage;
        if (slot.hitCap > 0 && slot.hitCap <= slot.hitAccumulator) {
            slot.damage = 0;
            total -= slot.hitAccumulator - slot.hitCap;
        }
    }
    return total;
}

int CanonicalCreate(Player& player, bool cancel, const Region& value) {
    std::array<Region, kRegionCount>& pool =
        cancel ? player.cancelRegions : player.damageRegions;
    int index;
    for (index = 0; index < 191; ++index) {
        if (pool[index].active == 0) {
            break;
        }
    }
    pool[index] = value;
    pool[index].active = 1;
    return index;
}

int SidecarCreate(Player& player, Sidecar& sidecar, bool cancel,
                  const Region& value) {
    EnsureSidecar(sidecar, player);
    std::array<Region, kRegionCount>& pool =
        cancel ? player.cancelRegions : player.damageRegions;
    std::uint32_t* bits = cancel ? sidecar.activeCancelRegionBits.data()
                                 : sidecar.activeDamageRegionBits.data();
    int index;
    for (index = 0; index < 191; ++index) {
        if (pool[index].active != 0) {
            SetBit(bits, index);
            continue;
        }
        ClearBit(bits, index);
        break;
    }
    pool[index] = value;
    pool[index].active = 1;
    SetBit(bits, index);
    return index;
}

Region MakeRegion(int serial, bool overlaps, int collisionValue,
                  int damage, int hitAccumulator, int hitCap) {
    Region result;
    result.active = 1;
    result.serial = serial;
    result.overlaps = overlaps ? 1 : 0;
    result.collisionValue = collisionValue;
    result.damage = damage;
    result.hitAccumulator = hitAccumulator;
    result.hitCap = hitCap;
    return result;
}

void RequireSame(const Player& canonical, const Player& sidecar,
                 const std::string& where) {
    Require(SamePlayer(canonical, sidecar), where + ": authoritative state differs");
}

void ScenarioBitBoundaries() {
    Player canonical;
    const std::array<int, 4> regionBoundaries{{31, 32, 127, 191}};
    const std::array<int, 3> shotBoundaries{{31, 32, 127}};
    for (int index : regionBoundaries) {
        canonical.cancelRegions[index] = MakeRegion(index, false, index, 0, 0, 0);
        canonical.damageRegions[index] = MakeRegion(index, false, 0, 1, 0, 0);
    }
    for (int index : shotBoundaries) {
        canonical.shots[index].state = 1;
        canonical.shots[index].serial = index;
    }
    Player optimized = canonical;
    Sidecar sidecar;
    ResetSidecar(sidecar, optimized);
    AuditShots(sidecar, optimized);
    AuditRegions(sidecar, optimized);

    Require(CollectBits(sidecar.activeCancelRegionBits.data(), 6) ==
                std::vector<int>(regionBoundaries.begin(), regionBoundaries.end()),
            "cancel bit boundaries");
    Require(CollectBits(sidecar.activeDamageRegionBits.data(), 6) ==
                std::vector<int>(regionBoundaries.begin(), regionBoundaries.end()),
            "damage bit boundaries");
    Require(CollectBits(sidecar.activeShotBits.data(), 4) ==
                std::vector<int>(shotBoundaries.begin(), shotBoundaries.end()),
            "shot bit boundaries");

    std::vector<int> canonicalTrace;
    std::vector<int> optimizedTrace;
    Require(CanonicalCancel(canonical, &canonicalTrace) ==
                SidecarCancel(optimized, sidecar, &optimizedTrace),
            "boundary cancel result");
    Require(canonicalTrace == optimizedTrace, "boundary traversal order");
    RequireSame(canonical, optimized, "boundary scan");
}

void ScenarioDuplicateCancel() {
    Player canonical;
    canonical.cancelRegions[31] = MakeRegion(31, false, 31, 0, 0, 0);
    canonical.cancelRegions[32] = MakeRegion(32, true, 7, 0, 0, 0);
    canonical.cancelRegions[191] = MakeRegion(191, true, 9, 0, 0, 0);
    Player optimized = canonical;
    Sidecar sidecar;
    ResetSidecar(sidecar, optimized);
    AuditRegions(sidecar, optimized);

    for (int call = 0; call < 2; ++call) {
        canonical.bulletCancelItemType = 6;
        optimized.bulletCancelItemType = 6;
        std::vector<int> canonicalTrace;
        std::vector<int> optimizedTrace;
        Require(CanonicalCancel(canonical, &canonicalTrace) ==
                    SidecarCancel(optimized, sidecar, &optimizedTrace),
                "duplicate cancel result");
        Require(canonicalTrace == optimizedTrace, "duplicate cancel order");
        RequireSame(canonical, optimized, "duplicate cancel call");
    }
    Require(canonical.cancelRegions[32].hitAccumulator == 2,
            "duplicate cancel hitAccumulator");
    Require(canonical.cancelRegions[191].hitAccumulator == 0,
            "duplicate cancel stopped after first hit");
}

void ScenarioFirstHitOrder() {
    Player canonical;
    for (int index : {191, 127, 32}) {
        canonical.cancelRegions[index] =
            MakeRegion(index, true, 1000 + index, 0, 0, 0);
    }
    Player optimized = canonical;
    Sidecar sidecar;
    ResetSidecar(sidecar, optimized);
    AuditRegions(sidecar, optimized);

    Require(CanonicalCancel(canonical) == SidecarCancel(optimized, sidecar),
            "first-hit result");
    RequireSame(canonical, optimized, "first-hit order");
    Require(canonical.bulletCancelItemType == 1032,
            "lowest active hit slot selected");
    Require(canonical.cancelRegions[32].hitAccumulator == 1 &&
                canonical.cancelRegions[127].hitAccumulator == 0 &&
                canonical.cancelRegions[191].hitAccumulator == 0,
            "first-hit mutation order");
}

void ScenarioFullPoolOverwrite() {
    Player canonical;
    for (int index = 0; index < kRegionCount; ++index) {
        canonical.cancelRegions[index] =
            MakeRegion(index, false, index, 0, index, 0);
        canonical.damageRegions[index] =
            MakeRegion(1000 + index, false, 0, index + 1, index, index + 10);
    }
    Player optimized = canonical;
    Sidecar sidecar;
    ResetSidecar(sidecar, optimized);
    AuditRegions(sidecar, optimized);

    const Region cancelValue = MakeRegion(9001, true, 77, 0, 0, 0);
    const Region damageValue = MakeRegion(9002, true, 0, 23, 4, 19);
    Require(CanonicalCreate(canonical, true, cancelValue) == 191,
            "canonical cancel full-pool slot");
    Require(SidecarCreate(optimized, sidecar, true, cancelValue) == 191,
            "sidecar cancel full-pool slot");
    Require(CanonicalCreate(canonical, false, damageValue) == 191,
            "canonical damage full-pool slot");
    Require(SidecarCreate(optimized, sidecar, false, damageValue) == 191,
            "sidecar damage full-pool slot");
    RequireSame(canonical, optimized, "slot191 overwrite");
    Require(TestBit(sidecar.activeCancelRegionBits.data(), 191),
            "slot191 cancel bit retained");
    Require(TestBit(sidecar.activeDamageRegionBits.data(), 191),
            "slot191 damage bit retained");
    Require(canonical.cancelRegions[191].serial == 9001 &&
                canonical.damageRegions[191].serial == 9002,
            "slot191 payload overwritten");
}

void ScenarioStalePositiveRepair() {
    Player canonical;
    canonical.cancelRegions[31] = MakeRegion(31, false, 31, 0, 0, 0);
    canonical.cancelRegions[127] = MakeRegion(127, true, 127, 0, 0, 0);
    canonical.damageRegions[32] = MakeRegion(32, true, 0, 11, 0, 0);
    canonical.damageRegions[191] = MakeRegion(191, true, 0, 5, 0, 0);
    Player optimized = canonical;
    Sidecar sidecar;
    ResetSidecar(sidecar, optimized);
    AuditRegions(sidecar, optimized);

    // PlayerBomb has direct deactivation writers.  They may leave a one-way
    // stale positive, which each consumer must validate and remove.
    canonical.cancelRegions[31].active = 0;
    optimized.cancelRegions[31].active = 0;
    canonical.damageRegions[32].active = 0;
    optimized.damageRegions[32].active = 0;
    Require(TestBit(sidecar.activeCancelRegionBits.data(), 31),
            "cancel stale positive setup");
    Require(TestBit(sidecar.activeDamageRegionBits.data(), 32),
            "damage stale positive setup");

    Require(CanonicalCancel(canonical) == SidecarCancel(optimized, sidecar),
            "stale cancel result");
    Require(CanonicalDamage(canonical) == SidecarDamage(optimized, sidecar),
            "stale damage result");
    RequireSame(canonical, optimized, "stale-positive repair");
    Require(!TestBit(sidecar.activeCancelRegionBits.data(), 31),
            "cancel stale positive cleared");
    Require(!TestBit(sidecar.activeDamageRegionBits.data(), 32),
            "damage stale positive cleared");
}

void ScenarioDamageMutation() {
    Player canonical;
    canonical.damageRegions[31] = MakeRegion(31, true, 0, 4, 0, 0);
    canonical.damageRegions[32] = MakeRegion(32, true, 0, 10, 7, 12);
    canonical.damageRegions[127] = MakeRegion(127, true, 0, 6, 4, 10);
    canonical.damageRegions[191] = MakeRegion(191, true, 0, 3, 3, 5);
    Player optimized = canonical;
    Sidecar sidecar;
    ResetSidecar(sidecar, optimized);
    AuditRegions(sidecar, optimized);

    std::vector<int> canonicalTrace;
    std::vector<int> optimizedTrace;
    const int canonicalDamage = CanonicalDamage(canonical, &canonicalTrace);
    const int optimizedDamage = SidecarDamage(optimized, sidecar, &optimizedTrace);
    Require(canonicalDamage == 17 && optimizedDamage == canonicalDamage,
            "damage/hitCap total");
    Require(canonicalTrace == optimizedTrace,
            "damage region traversal order");
    RequireSame(canonical, optimized, "damage mutation");
    Require(canonical.damageRegions[31].hitAccumulator == 4 &&
                canonical.damageRegions[31].damage == 4,
            "uncapped damage mutation");
    Require(canonical.damageRegions[32].hitAccumulator == 17 &&
                canonical.damageRegions[32].damage == 0,
            "over-cap damage mutation");
    Require(canonical.damageRegions[127].hitAccumulator == 10 &&
                canonical.damageRegions[127].damage == 0,
            "exact-cap damage mutation");
    Require(canonical.damageRegions[191].hitAccumulator == 6 &&
                canonical.damageRegions[191].damage == 0,
            "late-slot hitCap mutation");
}

void ScenarioResetRebuildLifecycle() {
    Player canonicalOne;
    canonicalOne.cancelRegions[31] = MakeRegion(31, true, 31, 0, 0, 0);
    Player optimizedOne = canonicalOne;
    Sidecar sidecar;

    // owner mismatch must rebuild from authoritative arrays.
    Require(CanonicalCancel(canonicalOne) ==
                SidecarCancel(optimizedOne, sidecar),
            "initial owner rebuild");
    RequireSame(canonicalOne, optimizedOne, "initial owner lifecycle");

    // RegisterChain/AddedCallback zero the authoritative pools and reset the
    // external cache to the same owner with empty bits.
    canonicalOne = Player{};
    optimizedOne = Player{};
    ResetSidecar(sidecar, optimizedOne);
    Require(CanonicalCancel(canonicalOne) ==
                SidecarCancel(optimizedOne, sidecar),
            "pool reset lifecycle");
    Require(CollectBits(sidecar.activeCancelRegionBits.data(), 6).empty(),
            "pool reset empties bits");

    // A new Player owner triggers Ensure's reset + complete audit.
    Player canonicalTwo;
    canonicalTwo.cancelRegions[191] = MakeRegion(191, true, 191, 0, 0, 0);
    Player optimizedTwo = canonicalTwo;
    Require(CanonicalCancel(canonicalTwo) ==
                SidecarCancel(optimizedTwo, sidecar),
            "owner switch rebuild");
    RequireSame(canonicalTwo, optimizedTwo, "owner switch lifecycle");

    // UpdateCollisionRegions is the once-per-frame false-negative audit.
    canonicalTwo.cancelRegions[127] = MakeRegion(127, true, 127, 0, 0, 0);
    optimizedTwo.cancelRegions[127] = canonicalTwo.cancelRegions[127];
    canonicalTwo.cancelRegions[191].overlaps = 0;
    optimizedTwo.cancelRegions[191].overlaps = 0;
    Require(!TestBit(sidecar.activeCancelRegionBits.data(), 127),
            "untracked writer begins as false negative");
    AuditRegions(sidecar, optimizedTwo);
    Require(TestBit(sidecar.activeCancelRegionBits.data(), 127),
            "frame rebuild recovers active writer");
    Require(CanonicalCancel(canonicalTwo) ==
                SidecarCancel(optimizedTwo, sidecar),
            "post-rebuild collision");
    RequireSame(canonicalTwo, optimizedTwo, "post-rebuild lifecycle");
}

std::uint32_t gRandom = 0x54483038U;

std::uint32_t NextRandom() {
    gRandom = gRandom * 1664525U + 1013904223U;
    return gRandom;
}

void ScenarioRandomizedDifferential() {
    Player canonical;
    Player optimized;
    Sidecar sidecar;
    ResetSidecar(sidecar, optimized);
    AuditShots(sidecar, optimized);
    AuditRegions(sidecar, optimized);

    for (int step = 0; step < 20000; ++step) {
        const int operation = static_cast<int>(NextRandom() % 9U);
        const int index = static_cast<int>(NextRandom() % kRegionCount);
        if (operation == 0 || operation == 1) {
            Region value = MakeRegion(
                step + 1,
                (NextRandom() & 3U) == 0U,
                static_cast<int>(NextRandom() % 10U) - 1,
                static_cast<int>(NextRandom() % 24U),
                static_cast<int>(NextRandom() % 12U),
                (NextRandom() & 1U) != 0U
                    ? static_cast<int>(NextRandom() % 32U) + 1
                    : 0);
            const bool cancel = operation == 0;
            Require(CanonicalCreate(canonical, cancel, value) ==
                        SidecarCreate(optimized, sidecar, cancel, value),
                    "random create slot");
        } else if (operation == 2) {
            canonical.cancelRegions[index].active = 0;
            optimized.cancelRegions[index].active = 0;
        } else if (operation == 3) {
            canonical.damageRegions[index].active = 0;
            optimized.damageRegions[index].active = 0;
        } else if (operation == 4) {
            AuditRegions(sidecar, optimized);
        } else if (operation == 5) {
            canonical.bulletCancelItemType = 6;
            optimized.bulletCancelItemType = 6;
            Require(CanonicalCancel(canonical) ==
                        SidecarCancel(optimized, sidecar),
                    "random cancel result");
        } else if (operation == 6) {
            Require(CanonicalDamage(canonical) ==
                        SidecarDamage(optimized, sidecar),
                    "random damage result");
        } else if (operation == 7) {
            const int overlap = static_cast<int>(NextRandom() & 1U);
            canonical.cancelRegions[index].overlaps = overlap;
            optimized.cancelRegions[index].overlaps = overlap;
            canonical.damageRegions[index].overlaps = overlap;
            optimized.damageRegions[index].overlaps = overlap;
        } else {
            canonical = Player{};
            optimized = Player{};
            ResetSidecar(sidecar, optimized);
        }
        RequireSame(canonical, optimized,
                    "random step " + std::to_string(step));
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        Fail("expected exactly one scenario name");
    }
    const std::string scenario = argv[1];
    if (scenario == "bit-boundaries") {
        ScenarioBitBoundaries();
    } else if (scenario == "duplicate-cancel") {
        ScenarioDuplicateCancel();
    } else if (scenario == "first-hit-order") {
        ScenarioFirstHitOrder();
    } else if (scenario == "full-pool-overwrite") {
        ScenarioFullPoolOverwrite();
    } else if (scenario == "stale-positive-repair") {
        ScenarioStalePositiveRepair();
    } else if (scenario == "damage-mutation") {
        ScenarioDamageMutation();
    } else if (scenario == "reset-rebuild-lifecycle") {
        ScenarioResetRebuildLifecycle();
    } else if (scenario == "randomized-differential") {
        ScenarioRandomizedDifferential();
    } else {
        Fail("unknown scenario: " + scenario);
    }
    std::cout << "player-scan-sidecar: PASS scenario=" << scenario << '\n';
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
    end = source.find("\n// FUNCTION:", start + len(signature))
    if end < 0:
        end = len(source)
    return source[start:end]


class PlayerScanSidecarHostTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.temporary = tempfile.TemporaryDirectory(prefix="th08-player-sidecar-")
        output = Path(cls.temporary.name)
        source = output / "player_scan_sidecar_harness.cpp"
        cls.executable = output / "player_scan_sidecar_harness"
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
                "host harness compilation failed:\n"
                + result.stdout
                + result.stderr
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

    def test_bit_boundaries_31_32_127_191(self) -> None:
        self.run_scenario("bit-boundaries")

    def test_duplicate_cancel_query_matches_twice(self) -> None:
        self.run_scenario("duplicate-cancel")

    def test_first_hit_slot_order(self) -> None:
        self.run_scenario("first-hit-order")

    def test_slot191_full_pool_overwrite(self) -> None:
        self.run_scenario("full-pool-overwrite")

    def test_stale_positive_repair(self) -> None:
        self.run_scenario("stale-positive-repair")

    def test_damage_hit_accumulator_and_cap_mutation(self) -> None:
        self.run_scenario("damage-mutation")

    def test_reset_and_rebuild_lifecycle(self) -> None:
        self.run_scenario("reset-rebuild-lifecycle")

    def test_randomized_differential(self) -> None:
        self.run_scenario("randomized-differential")


class PlayerScanSidecarStaticTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.player = PLAYER_CPP.read_text(encoding="utf-8")
        cls.player_bomb = PLAYER_BOMB_CPP.read_text(encoding="utf-8")

    def test_all_activation_factories_publish_bits(self) -> None:
        factories = (
            ("Player::CreateRectCancelRegion(", "activeCancelRegionBits"),
            ("Player::CreateCircleCancelRegion(", "activeCancelRegionBits"),
            ("Player::CreateRectDamageRegion(", "activeDamageRegionBits"),
            ("Player::CreateCircleDamageRegion(", "activeDamageRegionBits"),
        )
        for signature, bitset in factories:
            body = function_body(self.player, signature)
            self.assertIn("for (index = 0; index < 191;", body)
            self.assertEqual(body.count("slot->active = true;"), 1)
            if bitset == "activeCancelRegionBits":
                self.assertIn("PspSetCancelScanBit(index);", body)
            else:
                self.assertIn(
                    f"PspSetScanBit(g_PspPlayerScanSidecar.{bitset}, index);",
                    body,
                )

        # There must be no fifth Player collision-region activation path that
        # silently bypasses the four instrumented factories.
        self.assertEqual(self.player.count("slot->active = true;"), 4)

    def test_active_writer_coverage_and_one_way_staleness(self) -> None:
        # Direct PlayerBomb writers are deactivations only.  They can create a
        # stale positive but never the unsafe false negative.
        direct_region_writes = re.findall(
            r"(?:cancelRegion|damageRegion)->active\s*=\s*([^;]+);",
            self.player_bomb,
        )
        self.assertGreater(len(direct_region_writes), 0)
        self.assertTrue(
            all(value.strip() in {"0", "false"} for value in direct_region_writes),
            msg=f"unsafe direct collision-region activation: {direct_region_writes}",
        )

        deactivate = function_body(self.player, "PlayerCollisionRegion::Deactivate(")
        self.assertIn("this->active = false;", deactivate)

        cancel_consumer = function_body(
            self.player, "Player::CheckBulletCancelCollision("
        )
        damage_consumer = function_body(self.player, "Player::CalcDamageToEnemy(")
        self.assertRegex(
            cancel_consumer,
            r"(?s)if \(!slot->active\).*?PspClearCancelScanBit\(i\);",
        )
        self.assertRegex(
            damage_consumer,
            r"(?s)if \(!region->active\).*?PspClearScanBit\("
            r"g_PspPlayerScanSidecar\.activeDamageRegionBits, i\);",
        )

    def test_shot_writer_and_consumer_coverage(self) -> None:
        spawn = function_body(self.player, "Player::SpawnShots(")
        update = function_body(self.player, "Player::UpdateShots(")
        self.assertIn("PspEnsurePlayerScanSidecar(this);", spawn)
        self.assertIn("activeShotBits", spawn)
        self.assertIn("PspSetScanBit", spawn)
        self.assertIn("PspClearScanBit", spawn)
        self.assertIn("memset(g_PspPlayerScanSidecar.activeShotBits, 0", update)
        self.assertIn("PspSetScanBit", update)
        self.assertIn("PspClearScanBit", update)

        for signature in (
            "Player::DrawActiveShots(",
            "Player::DrawHitShots(",
            "Player::CalcDamageToEnemy(",
        ):
            consumer = function_body(self.player, signature)
            self.assertIn("activeShotBits", consumer)
            self.assertIn("PspClearScanBit", consumer)

    def test_frame_audit_and_reset_lifecycle_are_wired(self) -> None:
        update = function_body(self.player, "Player::UpdateCollisionRegions(")
        self.assertIn("for (index = 0; index < 384;", update)
        self.assertIn("memset(g_PspPlayerScanSidecar.activeDamageRegionBits, 0", update)
        self.assertIn("PspClearAllCancelScanBits();", update)
        self.assertIn("PspSetScanBit(g_PspPlayerScanSidecar.activeDamageRegionBits", update)
        self.assertIn("PspSetCancelScanBit(index - 192);", update)

        register = function_body(self.player, "Player::RegisterChain(")
        added = function_body(self.player, "Player::AddedCallback(")
        self.assertIn("PspResetPlayerScanSidecar(player);", register)
        self.assertIn("PspResetPlayerScanSidecar(player);", added)


if __name__ == "__main__":
    unittest.main(verbosity=2)
