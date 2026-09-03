#!/usr/bin/env python3
"""Static and differential gates for callback2's PSP work-item bound."""

from __future__ import annotations

import copy
import random
import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PLAYER_BOMB = (ROOT / "src/PlayerBomb.cpp").read_text(encoding="utf-8")
MAKEFILE = (ROOT / "Makefile.psp").read_text(encoding="utf-8")
MAIN = (ROOT / "psp/main.cpp").read_text(encoding="utf-8")
FEATURE = "TH08_PSP_FANTASY_SEAL_WORK_BOUNDS"
LIMIT_MACRO = "TH08_FANTASY_SEAL_CALLBACK2_SCAN_LIMIT"
SLOT_COUNT = 128
BOUNDED_COUNT = 18


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


def update_secondary(
    states: list[int], vm_ticks: list[int], frame: int, limit: int
) -> list[tuple[str, int]]:
    events: list[tuple[str, int]] = []
    for slot in range(16, limit):
        if states[slot] == 0:
            continue
        events.append(("centers", slot))
        vm_ticks[slot] += 1
        events.append(("vm", slot))
        # Deterministic mock for ExecuteScript completion. It uses no random
        # state, just as changing the production scan bound must consume none.
        if ((frame * 17 + slot * 29) & 63) == 7:
            states[slot] = 0
            events.append(("inactive", slot))
    return events


def draw_active(states: list[int], limit: int) -> list[int]:
    return [slot for slot in range(limit) if states[slot] != 0]


class FantasySealWorkBoundsStaticTests(unittest.TestCase):
    def test_default_off_stamp_rebuild_and_boot_identity(self) -> None:
        self.assertIn(f"{FEATURE} ?= 0", MAKEFILE)
        self.assertIn("fantasy-seal-work-bounds-0.stamp", MAKEFILE)
        self.assertIn("fantasy-seal-work-bounds-1.stamp", MAKEFILE)
        self.assertIn(f"-D{FEATURE}=1", MAKEFILE)
        self.assertIn(f"$(error {FEATURE} must be 0 or 1)", MAKEFILE)
        self.assertIn("$(FANTASY_SEAL_WORK_BOUNDS_CONFIG_STAMPS)", MAKEFILE)
        self.assertIn(
            "src/PlayerBomb.o psp/main.o: "
            "$(FANTASY_SEAL_WORK_BOUNDS_CONFIG_STAMP)",
            MAKEFILE,
        )
        self.assertIn("TH08_PSP_FEATURE_FANTASY_SEAL_WORK_BOUNDS", MAIN)
        self.assertIn("FANTASY_SEAL_WORK_BOUNDS=%d", MAIN)

    def test_gate_is_psp_only_and_limit_is_exact(self) -> None:
        gate = (
            f"#if defined(PSP) && defined({FEATURE}) && \\\n"
            f"    {FEATURE}"
        )
        self.assertIn(gate, PLAYER_BOMB)
        self.assertRegex(
            PLAYER_BOMB,
            re.compile(rf"#define {LIMIT_MACRO}\(items\) 18U\b"),
        )
        self.assertIn(
            f"#define {LIMIT_MACRO}(items) ARRAY_SIZE(items)", PLAYER_BOMB
        )
        self.assertEqual(PLAYER_BOMB.count(f"{LIMIT_MACRO}("), 4)
        self.assertEqual(PLAYER_BOMB.count(f"#undef {LIMIT_MACRO}"), 1)

    def test_only_callback2_update_and_draw_scans_are_bounded(self) -> None:
        begin = function_body(PLAYER_BOMB, "void __fastcall BeginBombSpell(")
        update = function_body(
            PLAYER_BOMB,
            "void __fastcall UpdateFantasySealBlinkDeathbomb(Player *player)",
        )
        draw = function_body(
            PLAYER_BOMB,
            "void __fastcall DrawFantasySealBlinkDeathbomb(Player *player)",
        )

        self.assertNotIn(LIMIT_MACRO, begin)
        self.assertRegex(begin, r"for \(; i < 128; i\+\+, workItem\+\+\)")
        self.assertIn(
            "workItem->state = PLAYER_BOMB_WORK_ITEM_INACTIVE;", begin
        )

        self.assertEqual(update.count(LIMIT_MACRO), 1)
        self.assertRegex(
            update,
            re.compile(
                rf"workItem = &bomb->workItems\[16\];\s*"
                rf"for \(i = 16;\s*i < {LIMIT_MACRO}"
            ),
        )
        self.assertIn("bomb->secondaryWorkCursor = 0;", update)
        self.assertIn(
            "workItem = &bomb->workItems[bomb->secondaryWorkCursor + 16];",
            update,
        )
        self.assertIn("bomb->secondaryWorkCursor = 1;", update)
        self.assertEqual(update.count("GetRandomF32InRange"), 2)

        self.assertEqual(draw.count(LIMIT_MACRO), 1)
        self.assertRegex(draw, re.compile(rf"for \(;\s*i < {LIMIT_MACRO}"))
        self.assertEqual(draw.count("DrawNoRotation(vm)"), 1)


class FantasySealWorkBoundsDifferentialTests(unittest.TestCase):
    def test_legal_200_frame_lifetimes_match_canonical_order(self) -> None:
        scenario_rng = random.Random(0x46534238)
        for _trial in range(256):
            # BeginBombSpell is authoritative: stale state in every one of the
            # 128 large slots is cleared before callback2 initializes 0..15.
            stale_states = [scenario_rng.randrange(3) for _ in range(SLOT_COUNT)]
            canonical_states = copy.copy(stale_states)
            bounded_states = copy.copy(stale_states)
            for slot in range(SLOT_COUNT):
                canonical_states[slot] = 0
                bounded_states[slot] = 0
            for slot in range(16):
                canonical_states[slot] = bounded_states[slot] = 1

            canonical_ticks = [scenario_rng.randrange(1000) for _ in range(SLOT_COUNT)]
            bounded_ticks = copy.copy(canonical_ticks)
            secondary_cursor = 0

            for frame in range(200):
                # Exact source schedule: slot 16 at frame 40, then slot 17 for
                # every later 20-frame spawn through frame 180.
                if frame >= 40 and frame % 20 == 0:
                    slot = secondary_cursor + 16
                    secondary_cursor = 1
                    canonical_states[slot] = bounded_states[slot] = 1

                # Model primary ANM completion and all other legal writes as
                # identical external changes confined to slots 0..17.
                for slot in range(18):
                    if (
                        canonical_states[slot] != 0
                        and scenario_rng.randrange(257) == 0
                    ):
                        canonical_states[slot] = bounded_states[slot] = 0

                canonical_update = update_secondary(
                    canonical_states, canonical_ticks, frame, SLOT_COUNT
                )
                bounded_update = update_secondary(
                    bounded_states, bounded_ticks, frame, BOUNDED_COUNT
                )
                self.assertEqual(canonical_update, bounded_update)
                self.assertEqual(canonical_states, bounded_states)
                self.assertEqual(canonical_ticks, bounded_ticks)
                self.assertEqual(
                    draw_active(canonical_states, SLOT_COUNT),
                    draw_active(bounded_states, BOUNDED_COUNT),
                )

    def test_illegal_slot_18_writer_is_detected_by_model(self) -> None:
        states = [0] * SLOT_COUNT
        states[18] = 1
        self.assertNotEqual(
            draw_active(states, SLOT_COUNT), draw_active(states, BOUNDED_COUNT)
        )
        self.assertNotEqual(
            update_secondary(copy.copy(states), [0] * SLOT_COUNT, 0, SLOT_COUNT),
            update_secondary(copy.copy(states), [0] * SLOT_COUNT, 0, BOUNDED_COUNT),
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
