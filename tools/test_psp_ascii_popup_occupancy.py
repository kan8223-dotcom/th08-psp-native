#!/usr/bin/env python3
"""Differential/static checks for TH08_PSP_ASCII_POPUP_OCCUPANCY.

The production path is PSP-only and depends on the complete game runtime, so
this host test models the authoritative fixed-array scans and the derived
occupancy scans side by side.  Static checks bind the model to every production
writer, expiry, memset/reset, ordered-draw, and feature-gate boundary.
"""

from __future__ import annotations

from dataclasses import dataclass, field
import copy
import random
import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ASCII_CPP = (ROOT / "src" / "AsciiManager.cpp").read_text(encoding="utf-8")
ASCII_HPP = (ROOT / "src" / "AsciiManager.hpp").read_text(encoding="utf-8")
MAKEFILE_PSP = (ROOT / "Makefile.psp").read_text(encoding="utf-8")
PSP_MAIN = (ROOT / "psp" / "main.cpp").read_text(encoding="utf-8")

FEATURE = "TH08_PSP_ASCII_POPUP_OCCUPANCY"
SCORE_POPUPS = 720
PLAYER_POPUPS = 3
SCORE_SLOTS = SCORE_POPUPS + PLAYER_POPUPS
TIME_SLOTS = 128


def stored_number(number: int) -> list[int]:
    if number < 0:
        return [10]
    if number == 0:
        return [0]
    result: list[int] = []
    while number:
        result.append(number % 10)
        number //= 10
    return result


def stored_time(primary: int, secondary: int) -> list[int]:
    result: list[int] = []
    if secondary > 0:
        result.append(15)
        result.extend(stored_number(secondary))
        result.append(14)
    if primary > 0:
        result.extend(stored_number(primary))
    else:
        result.append(0)
    result.append(13)
    if len(result) > 12:
        raise ValueError("test input exceeds the retail popup text storage")
    return result


@dataclass
class Popup:
    text: list[int] = field(default_factory=lambda: [0] * 12)
    character_count: int = 0
    in_use: bool = False
    timer: int = 0
    y: float = 0.0
    scale_x: float = 0.0
    scale_y: float = 0.0
    color: int = 0
    serial: int = -1

    def install(
        self,
        glyphs: list[int],
        *,
        y: float,
        color: int,
        serial: int,
        timer: int = 0,
        scale: tuple[float, float] | None = None,
    ) -> None:
        # Production overwrites only the used prefix; bytes beyond
        # characterCount deliberately retain their prior values.
        for index, glyph in enumerate(glyphs):
            self.text[index] = glyph
        self.character_count = len(glyphs)
        self.in_use = True
        self.timer = timer
        self.y = y
        self.color = color
        self.serial = serial
        if scale is not None:
            self.scale_x, self.scale_y = scale


@dataclass
class Manager:
    score: list[Popup] = field(
        default_factory=lambda: [Popup() for _ in range(SCORE_SLOTS)]
    )
    time: list[Popup] = field(
        default_factory=lambda: [Popup() for _ in range(TIME_SLOTS)]
    )
    next_score: int = 0
    next_player: int = 0
    next_time: int = 0
    scale_x: float = 1.0
    scale_y: float = 1.0

    def reset(self) -> None:
        # Mirrors AsciiManager::Reset: nextTimePopupIndex is intentionally
        # preserved, while both authoritative arrays are memset to zero.
        next_time = self.next_time
        self.score = [Popup() for _ in range(SCORE_SLOTS)]
        self.time = [Popup() for _ in range(TIME_SLOTS)]
        self.next_score = 0
        self.next_player = 0
        self.next_time = next_time
        self.scale_x = 1.0
        self.scale_y = 1.0

    def added_callback_memset(self) -> None:
        self.score = [Popup() for _ in range(SCORE_SLOTS)]
        self.time = [Popup() for _ in range(TIME_SLOTS)]
        self.next_score = 0
        self.next_player = 0
        self.next_time = 0
        self.scale_x = 0.0
        self.scale_y = 0.0

    def snapshot(self) -> object:
        def popup_snapshot(popup: Popup) -> tuple[object, ...]:
            return (
                tuple(popup.text),
                popup.character_count,
                popup.in_use,
                popup.timer,
                popup.y,
                popup.scale_x,
                popup.scale_y,
                popup.color,
                popup.serial,
            )

        return (
            self.next_score,
            self.next_player,
            self.next_time,
            self.scale_x,
            self.scale_y,
            tuple(popup_snapshot(popup) for popup in self.score),
            tuple(popup_snapshot(popup) for popup in self.time),
        )


class Occupancy:
    def __init__(self) -> None:
        self.owner: Manager | None = None
        self.score_words = [0] * ((SCORE_SLOTS + 31) // 32)
        self.time_words = [0] * ((TIME_SLOTS + 31) // 32)
        self.score_last = -1
        self.time_last = -1

    @staticmethod
    def _set(words: list[int], last: int, slot: int) -> int:
        word = slot // 32
        words[word] |= 1 << (slot % 32)
        return max(last, word)

    @staticmethod
    def _clear(words: list[int], last: int, slot: int) -> int:
        word = slot // 32
        words[word] &= ~(1 << (slot % 32))
        if word == last and words[word] == 0:
            while last >= 0 and words[last] == 0:
                last -= 1
        return last

    def reset(self, owner: Manager | None) -> None:
        self.owner = owner
        self.score_words = [0] * len(self.score_words)
        self.time_words = [0] * len(self.time_words)
        self.score_last = -1
        self.time_last = -1

    def rebuild(self, owner: Manager) -> None:
        self.reset(owner)
        for slot, popup in enumerate(owner.score):
            if popup.in_use:
                self.mark_score(slot)
        for slot, popup in enumerate(owner.time):
            if popup.in_use:
                self.mark_time(slot)

    def ensure(self, owner: Manager) -> None:
        if self.owner is not owner:
            self.rebuild(owner)

    def mark_score(self, slot: int) -> None:
        self.score_last = self._set(self.score_words, self.score_last, slot)

    def mark_time(self, slot: int) -> None:
        self.time_last = self._set(self.time_words, self.time_last, slot)

    def clear_score(self, slot: int) -> None:
        self.score_last = self._clear(
            self.score_words, self.score_last, slot
        )

    def clear_time(self, slot: int) -> None:
        self.time_last = self._clear(self.time_words, self.time_last, slot)

    @staticmethod
    def indices(words: list[int], last: int, slots: int):
        for word_index in range(last + 1):
            pending = words[word_index]
            while pending:
                low = pending & -pending
                bit = low.bit_length() - 1
                slot = word_index * 32 + bit
                if slot < slots:
                    yield slot
                pending &= pending - 1

    def score_indices(self):
        return self.indices(self.score_words, self.score_last, SCORE_SLOTS)

    def time_indices(self):
        return self.indices(self.time_words, self.time_last, TIME_SLOTS)

    def assert_no_false_negative(self, manager: Manager) -> None:
        for slot, popup in enumerate(manager.score):
            cached = bool(
                self.score_words[slot // 32] & (1 << (slot % 32))
            )
            if popup.in_use and not cached:
                raise AssertionError(f"score false negative at {slot}")
        for slot, popup in enumerate(manager.time):
            cached = bool(
                self.time_words[slot // 32] & (1 << (slot % 32))
            )
            if popup.in_use and not cached:
                raise AssertionError(f"time false negative at {slot}")
        score_nonzero = [
            index for index, word in enumerate(self.score_words) if word
        ]
        time_nonzero = [
            index for index, word in enumerate(self.time_words) if word
        ]
        if self.score_last != (score_nonzero[-1] if score_nonzero else -1):
            raise AssertionError("score last-active word mismatch")
        if self.time_last != (time_nonzero[-1] if time_nonzero else -1):
            raise AssertionError("time last-active word mismatch")


def create_score(
    manager: Manager, occupancy: Occupancy | None, number: int, serial: int
) -> int:
    if manager.next_score >= SCORE_POPUPS:
        manager.next_score = 0
    slot = manager.next_score
    manager.score[slot].install(
        stored_number(number), y=float(serial % 480), color=serial, serial=serial
    )
    if occupancy is not None:
        occupancy.ensure(manager)
        occupancy.mark_score(slot)
    manager.next_score += 1
    return slot


def create_player(
    manager: Manager, occupancy: Occupancy | None, number: int, serial: int
) -> int:
    if manager.next_player >= PLAYER_POPUPS:
        manager.next_player = 0
    slot = SCORE_POPUPS + manager.next_player
    manager.score[slot].install(
        stored_number(number), y=float(serial % 480), color=serial, serial=serial
    )
    if occupancy is not None:
        occupancy.ensure(manager)
        occupancy.mark_score(slot)
    manager.next_player += 1
    return slot


def create_time(
    manager: Manager,
    occupancy: Occupancy | None,
    primary: int,
    secondary: int,
    serial: int,
    *,
    familiar: bool = False,
) -> int:
    if manager.next_time >= TIME_SLOTS:
        manager.next_time = 0
    slot = manager.next_time
    manager.time[slot].install(
        stored_time(primary, secondary),
        y=float(serial % 480),
        color=serial,
        serial=serial,
        timer=88 if familiar else 0,
        scale=(manager.scale_x, manager.scale_y),
    )
    if occupancy is not None:
        occupancy.ensure(manager)
        occupancy.mark_time(slot)
    manager.next_time += 1
    return slot


def canonical_update(
    manager: Manager, framerate_multiplier: float, enabled: bool = True
) -> list[tuple[str, int]]:
    trace: list[tuple[str, int]] = []
    if not enabled:
        return trace
    for slot, popup in enumerate(manager.score):
        if not popup.in_use:
            continue
        trace.append(("score", slot))
        popup.y -= 0.5 * framerate_multiplier
        popup.timer += 1
        if popup.timer > 60:
            popup.in_use = False
    for slot, popup in enumerate(manager.time):
        if not popup.in_use:
            continue
        trace.append(("time", slot))
        popup.timer += 1
        if popup.timer > 90:
            popup.in_use = False
    return trace


def optimized_update(
    manager: Manager,
    occupancy: Occupancy,
    framerate_multiplier: float,
    enabled: bool = True,
) -> list[tuple[str, int]]:
    trace: list[tuple[str, int]] = []
    if not enabled:
        return trace
    occupancy.ensure(manager)
    for slot in list(occupancy.score_indices()):
        popup = manager.score[slot]
        if not popup.in_use:
            occupancy.clear_score(slot)
            continue
        trace.append(("score", slot))
        popup.y -= 0.5 * framerate_multiplier
        popup.timer += 1
        if popup.timer > 60:
            popup.in_use = False
            occupancy.clear_score(slot)
    for slot in list(occupancy.time_indices()):
        popup = manager.time[slot]
        if not popup.in_use:
            occupancy.clear_time(slot)
            continue
        trace.append(("time", slot))
        popup.timer += 1
        if popup.timer > 90:
            popup.in_use = False
            occupancy.clear_time(slot)
    return trace


def popup_draw_record(pool: str, slot: int, popup: Popup, manager: Manager):
    glyphs = list(reversed(popup.text[: popup.character_count]))
    if pool == "score":
        bank = 0 if popup.timer < 52 else 11 if popup.timer < 56 else 21
        sprites = tuple(glyph + bank for glyph in glyphs)
        scale = (manager.scale_x, manager.scale_y)
    else:
        sprites = tuple(glyph + 136 for glyph in glyphs)
        scale = (popup.scale_x, popup.scale_y)
    return pool, slot, popup.serial, sprites, scale, popup.color, popup.y


def canonical_draw(manager: Manager) -> list[object]:
    result: list[object] = []
    for slot, popup in enumerate(manager.score):
        if popup.in_use:
            result.append(popup_draw_record("score", slot, popup, manager))
    for slot, popup in enumerate(manager.time):
        if popup.in_use:
            result.append(popup_draw_record("time", slot, popup, manager))
    return result


def optimized_draw(manager: Manager, occupancy: Occupancy) -> list[object]:
    result: list[object] = []
    occupancy.ensure(manager)
    for slot in list(occupancy.score_indices()):
        popup = manager.score[slot]
        if not popup.in_use:
            occupancy.clear_score(slot)
            continue
        result.append(popup_draw_record("score", slot, popup, manager))
    for slot in list(occupancy.time_indices()):
        popup = manager.time[slot]
        if not popup.in_use:
            occupancy.clear_time(slot)
            continue
        result.append(popup_draw_record("time", slot, popup, manager))
    return result


class PopupOccupancyDifferentialTests(unittest.TestCase):
    def make_pair(self) -> tuple[Manager, Manager, Occupancy]:
        canonical = Manager()
        optimized = copy.deepcopy(canonical)
        occupancy = Occupancy()
        occupancy.reset(optimized)
        return canonical, optimized, occupancy

    def assert_same(
        self, canonical: Manager, optimized: Manager, occupancy: Occupancy
    ) -> None:
        self.assertEqual(canonical.snapshot(), optimized.snapshot())
        self.assertEqual(canonical_draw(canonical), optimized_draw(optimized, occupancy))
        occupancy.assert_no_false_negative(optimized)

    def test_boundaries_and_strict_slot_order(self) -> None:
        canonical, optimized, occupancy = self.make_pair()
        for slot in (31, 32, 719, 720, 722):
            serial = 1000 + slot
            canonical.score[slot].install(
                stored_number(serial), y=slot, color=serial, serial=serial
            )
            optimized.score[slot] = copy.deepcopy(canonical.score[slot])
            occupancy.mark_score(slot)
        for slot in (31, 32, 127):
            serial = 2000 + slot
            canonical.time[slot].install(
                stored_time(13, 0),
                y=slot,
                color=serial,
                serial=serial,
                scale=(2.0, 2.0),
            )
            optimized.time[slot] = copy.deepcopy(canonical.time[slot])
            occupancy.mark_time(slot)

        expected = [
            *(('score', slot) for slot in (31, 32, 719, 720, 722)),
            *(('time', slot) for slot in (31, 32, 127)),
        ]
        actual = [(entry[0], entry[1]) for entry in optimized_draw(optimized, occupancy)]
        self.assertEqual(actual, expected)
        self.assert_same(canonical, optimized, occupancy)

    def test_wrap_overwrite_and_glyphs(self) -> None:
        canonical, optimized, occupancy = self.make_pair()
        for serial in range(725):
            create_score(canonical, None, serial % 137, serial)
            create_score(optimized, occupancy, serial % 137, serial)
        for serial in range(7):
            create_player(canonical, None, -1 if serial == 0 else serial, 9000 + serial)
            create_player(optimized, occupancy, -1 if serial == 0 else serial, 9000 + serial)
        for serial in range(135):
            create_time(canonical, None, 13, 0, 10000 + serial)
            create_time(optimized, occupancy, 13, 0, 10000 + serial)

        self.assertEqual(optimized.score[0].serial, 720)
        self.assertEqual(optimized.score[4].serial, 724)
        self.assertEqual(optimized.time[0].serial, 10128)
        self.assertEqual(optimized.time[6].serial, 10134)
        self.assertEqual(occupancy.score_last, 22)
        self.assertEqual(occupancy.time_last, 3)
        self.assert_same(canonical, optimized, occupancy)

    def test_exact_expiry_pause_and_familiar_lifetime(self) -> None:
        canonical, optimized, occupancy = self.make_pair()
        score_slot = create_score(canonical, None, 13, 1)
        create_score(optimized, occupancy, 13, 1)
        time_slot = create_time(canonical, None, 1, 0, 2)
        create_time(optimized, occupancy, 1, 0, 2)
        familiar_slot = create_time(canonical, None, 13, 4, 3, familiar=True)
        create_time(optimized, occupancy, 13, 4, 3, familiar=True)
        canonical.score[score_slot].timer = optimized.score[score_slot].timer = 60
        canonical.time[time_slot].timer = optimized.time[time_slot].timer = 90

        self.assertEqual(canonical_update(canonical, 1.0, False), [])
        self.assertEqual(optimized_update(optimized, occupancy, 1.0, False), [])
        self.assert_same(canonical, optimized, occupancy)

        self.assertEqual(
            canonical_update(canonical, 1.0),
            optimized_update(optimized, occupancy, 1.0),
        )
        self.assertFalse(optimized.score[score_slot].in_use)
        self.assertFalse(optimized.time[time_slot].in_use)
        self.assertEqual(optimized.time[familiar_slot].timer, 89)
        self.assert_same(canonical, optimized, occupancy)

        for _ in range(2):
            self.assertEqual(
                canonical_update(canonical, 1.0),
                optimized_update(optimized, occupancy, 1.0),
            )
        self.assertFalse(optimized.time[familiar_slot].in_use)
        self.assert_same(canonical, optimized, occupancy)

    def test_stale_positive_repair_never_changes_output(self) -> None:
        canonical, optimized, occupancy = self.make_pair()
        score_slot = create_score(canonical, None, 7, 71)
        create_score(optimized, occupancy, 7, 71)
        time_slot = create_time(canonical, None, 13, 0, 72)
        create_time(optimized, occupancy, 13, 0, 72)
        canonical.score[score_slot].in_use = False
        optimized.score[score_slot].in_use = False
        canonical.time[time_slot].in_use = False
        optimized.time[time_slot].in_use = False

        self.assertEqual(canonical_draw(canonical), optimized_draw(optimized, occupancy))
        self.assertEqual(occupancy.score_last, -1)
        self.assertEqual(occupancy.time_last, -1)
        self.assert_same(canonical, optimized, occupancy)

    def test_reset_memset_owner_rebuild_lifecycle(self) -> None:
        canonical, optimized, occupancy = self.make_pair()
        for serial in range(40):
            create_score(canonical, None, serial, serial)
            create_score(optimized, occupancy, serial, serial)
            create_time(canonical, None, serial, serial // 2, 100 + serial)
            create_time(optimized, occupancy, serial, serial // 2, 100 + serial)
        saved_next_time = optimized.next_time
        canonical.reset()
        optimized.reset()
        occupancy.reset(optimized)
        self.assertEqual(optimized.next_time, saved_next_time)
        self.assert_same(canonical, optimized, occupancy)

        other = Manager()
        other.score[32].install(stored_number(32), y=1.0, color=1, serial=32)
        other.time[127].install(
            stored_time(13, 0), y=2.0, color=2, serial=127, scale=(1.0, 1.0)
        )
        occupancy.ensure(other)
        self.assertEqual(list(occupancy.score_indices()), [32])
        self.assertEqual(list(occupancy.time_indices()), [127])

        # DeletedCallback invalidates the owner.  If a callback were to arrive
        # unexpectedly before AddedCallback, Ensure must rebuild rather than
        # produce a false negative from an empty sidecar.
        occupancy.reset(None)
        self.assertEqual(
            canonical_draw(other), optimized_draw(other, occupancy)
        )
        occupancy.assert_no_false_negative(other)

        other.added_callback_memset()
        occupancy.reset(other)
        self.assertEqual(list(occupancy.score_indices()), [])
        self.assertEqual(list(occupancy.time_indices()), [])
        occupancy.assert_no_false_negative(other)

    def test_25000_randomized_operations(self) -> None:
        rng = random.Random(0xA5C11)
        canonical, optimized, occupancy = self.make_pair()
        serial = 0
        for operation_index in range(25_000):
            choice = rng.randrange(100)
            if choice < 28:
                number = rng.randrange(-1, 1_000_000)
                create_score(canonical, None, number, serial)
                create_score(optimized, occupancy, number, serial)
                serial += 1
            elif choice < 34:
                number = rng.randrange(-1, 10_000)
                create_player(canonical, None, number, serial)
                create_player(optimized, occupancy, number, serial)
                serial += 1
            elif choice < 49:
                primary = rng.randrange(0, 500)
                secondary = rng.randrange(0, 500)
                create_time(canonical, None, primary, secondary, serial)
                create_time(optimized, occupancy, primary, secondary, serial)
                serial += 1
            elif choice < 53:
                primary = rng.randrange(0, 500)
                secondary = rng.randrange(0, 500)
                create_time(
                    canonical, None, primary, secondary, serial, familiar=True
                )
                create_time(
                    optimized,
                    occupancy,
                    primary,
                    secondary,
                    serial,
                    familiar=True,
                )
                serial += 1
            elif choice < 78:
                enabled = rng.randrange(7) != 0
                multiplier = (0.5, 1.0, 1.25)[rng.randrange(3)]
                self.assertEqual(
                    canonical_update(canonical, multiplier, enabled),
                    optimized_update(optimized, occupancy, multiplier, enabled),
                )
            elif choice < 84:
                canonical.scale_x = optimized.scale_x = rng.choice((0.5, 1.0, 2.0))
                canonical.scale_y = optimized.scale_y = rng.choice((0.5, 1.0, 2.0))
            elif choice < 91:
                if rng.randrange(2) == 0:
                    slot = rng.randrange(SCORE_SLOTS)
                    canonical.score[slot].in_use = False
                    optimized.score[slot].in_use = False
                else:
                    slot = rng.randrange(TIME_SLOTS)
                    canonical.time[slot].in_use = False
                    optimized.time[slot].in_use = False
            elif choice < 94:
                canonical.reset()
                optimized.reset()
                occupancy.reset(optimized)
            elif choice < 95:
                canonical.added_callback_memset()
                optimized.added_callback_memset()
                occupancy.reset(optimized)
            else:
                self.assertEqual(
                    canonical_draw(canonical), optimized_draw(optimized, occupancy)
                )

            self.assertEqual(canonical.snapshot(), optimized.snapshot())
            occupancy.assert_no_false_negative(optimized)
            if operation_index % 97 == 0:
                self.assertEqual(
                    canonical_draw(canonical), optimized_draw(optimized, occupancy)
                )


class PopupOccupancyProductionBindingTests(unittest.TestCase):
    def test_feature_is_psp_only_default_off_and_layout_external(self) -> None:
        gate = (
            f"#if defined(PSP) && defined({FEATURE}) && \\\n    {FEATURE}"
        )
        self.assertIn(gate, ASCII_CPP)
        self.assertIn("struct PspAsciiPopupOccupancy", ASCII_CPP)
        self.assertNotIn("PspAsciiPopupOccupancy", ASCII_HPP)
        self.assertIn("C_ASSERT(sizeof(AsciiManager) == 0x171b0);", ASCII_HPP)
        self.assertIn(f"{FEATURE} ?= 0", MAKEFILE_PSP)
        self.assertIn(f"-D{FEATURE}=1", MAKEFILE_PSP)
        self.assertIn("ASCII_POPUP_OCCUPANCY_CONFIG_STAMP", MAKEFILE_PSP)
        self.assertIn(
            "src/AsciiManager.o psp/main.o: "
            "$(ASCII_POPUP_OCCUPANCY_CONFIG_STAMP)",
            MAKEFILE_PSP,
        )
        self.assertIn("ASCII_POPUP_OCCUPANCY=%d", PSP_MAIN)
        self.assertIn("TH08_PSP_FEATURE_ASCII_POPUP_OCCUPANCY", PSP_MAIN)

    def test_all_four_activation_writers_mark_the_exact_pool(self) -> None:
        self.assertEqual(len(re.findall(r"popup->inUse = true;", ASCII_CPP)), 4)
        self.assertIn(
            "PspMarkScorePopupActive(this, nextScorePopupIndex);", ASCII_CPP
        )
        self.assertIn(
            "ASCII_MAX_SCORE_POPUPS + nextPlayerPointPopupIndex", ASCII_CPP
        )
        self.assertEqual(
            ASCII_CPP.count("PspMarkTimePopupActive(this, nextTimePopupIndex);"),
            2,
        )

    def test_expiry_reset_memset_and_deleted_lifecycle_are_closed(self) -> None:
        self.assertIn("PspMarkScorePopupInactive(i);", ASCII_CPP)
        self.assertIn("PspMarkTimePopupInactive(i);", ASCII_CPP)
        self.assertGreaterEqual(
            ASCII_CPP.count("PspResetAsciiPopupOccupancy("), 4
        )
        self.assertIn("memset(ascii, 0, sizeof(AsciiManager));", ASCII_CPP)
        self.assertIn("PspResetAsciiPopupOccupancy(ascii);", ASCII_CPP)
        self.assertIn("PspResetAsciiPopupOccupancy(NULL);", ASCII_CPP)

    def test_update_and_draw_both_use_ctz_ordered_word_iteration(self) -> None:
        self.assertIn("__builtin_ctz(pending)", ASCII_CPP)
        for iterator in (
            "scoreIterator.Next(&i)",
            "timeIterator.Next(&i)",
            "scoreDrawIterator.Next(&j)",
            "timeDrawIterator.Next(&j)",
        ):
            self.assertIn(iterator, ASCII_CPP)
        # Default-off branches retain the exact authored full-array bounds.
        self.assertIn(
            "for (i = 0; i < ASCII_MAX_SCORE_POPUPS + "
            "ASCII_MAX_PLAYER_POPUPS; i++, popup++)",
            ASCII_CPP,
        )
        self.assertIn(
            "for (j = 0; j < ASCII_MAX_SCORE_POPUPS + "
            "ASCII_MAX_PLAYER_POPUPS; j++, popup++)",
            ASCII_CPP,
        )
        self.assertGreaterEqual(
            ASCII_CPP.count("i < ASCII_MAX_TIME_POPUPS"), 1
        )
        self.assertGreaterEqual(
            ASCII_CPP.count("j < ASCII_MAX_TIME_POPUPS"), 1
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
