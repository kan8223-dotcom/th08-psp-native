#!/usr/bin/env python3
"""Differential/static checks for TH08_PSP_CANCEL_EMPTY_FASTPATH."""

from __future__ import annotations

from dataclasses import dataclass
import random
import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PLAYER_CPP = (ROOT / "src" / "Player.cpp").read_text(encoding="utf-8")
PLAYER_BOMB_CPP = (ROOT / "src" / "PlayerBomb.cpp").read_text(encoding="utf-8")
MAKEFILE = (ROOT / "Makefile.psp").read_text(encoding="utf-8")
MAIN_CPP = (ROOT / "psp" / "main.cpp").read_text(encoding="utf-8")
FEATURE = "TH08_PSP_CANCEL_EMPTY_FASTPATH"
REGION_COUNT = 192
WORD_COUNT = REGION_COUNT // 32


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    # Reconstructed game functions contain mutually exclusive preprocessor
    # branches whose braces confuse a raw lexical brace counter.  Their next
    # address marker is the stable translation-unit boundary used by the
    # existing Player sidecar test as well.
    if "Player::" in signature:
        end = source.find("\n// FUNCTION:", start + len(signature))
        return source[start : end if end >= 0 else len(source)]
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


@dataclass
class Region:
    active: bool = False
    overlaps: bool = False
    collision_value: int = -1
    hit_accumulator: int = 0
    serial: int = 0


class PlayerModel:
    def __init__(self) -> None:
        self.cancel_regions = [Region() for _ in range(REGION_COUNT)]
        self.bullet_cancel_item_type = 6

    def snapshot(self) -> tuple[object, ...]:
        return (
            self.bullet_cancel_item_type,
            tuple(
                (
                    slot.active,
                    slot.overlaps,
                    slot.collision_value,
                    slot.hit_accumulator,
                    slot.serial,
                )
                for slot in self.cancel_regions
            ),
        )


class CancelSidecarModel:
    """Model of the production one-way-safe derived metadata."""

    def __init__(self) -> None:
        self.owner: PlayerModel | None = None
        self.bits = [0] * WORD_COUNT
        self.cancel_any = False
        self.last_active_word = -1

    def clear_all(self) -> None:
        self.bits = [0] * WORD_COUNT
        self.cancel_any = False
        self.last_active_word = -1

    def reset(self, player: PlayerModel) -> None:
        # Mirrors memset(sidecar, 0) followed by the explicit -1 sentinel.
        self.owner = player
        self.clear_all()

    def set_bit(self, index: int) -> None:
        word = index >> 5
        self.bits[word] |= 1 << (index & 31)
        self.cancel_any = True
        self.last_active_word = max(self.last_active_word, word)

    def clear_bit(self, index: int) -> None:
        word = index >> 5
        self.bits[word] &= ~(1 << (index & 31))
        if (
            not self.cancel_any
            or word != self.last_active_word
            or self.bits[word] != 0
        ):
            return
        word -= 1
        while word >= 0 and self.bits[word] == 0:
            word -= 1
        self.last_active_word = word
        if word < 0:
            self.cancel_any = False

    def audit(self, player: PlayerModel) -> None:
        self.clear_all()
        for index, slot in enumerate(player.cancel_regions):
            if slot.active:
                self.set_bit(index)

    def ensure(self, player: PlayerModel) -> None:
        if self.owner is player:
            return
        self.reset(player)
        self.audit(player)

    def indices(self):
        if not self.cancel_any:
            return
        for word_index in range(self.last_active_word + 1):
            word = self.bits[word_index]
            while word:
                bit = (word & -word).bit_length() - 1
                yield (word_index << 5) + bit
                word &= word - 1

    def assert_safe(self, player: PlayerModel) -> None:
        nonzero_words = [index for index, word in enumerate(self.bits) if word]
        self_any = bool(nonzero_words)
        if self.cancel_any != self_any:
            raise AssertionError("cancelAny disagrees with bitset")
        expected_last = nonzero_words[-1] if nonzero_words else -1
        if self.last_active_word != expected_last:
            raise AssertionError("last active word disagrees with bitset")
        for index, slot in enumerate(player.cancel_regions):
            cached = bool(self.bits[index >> 5] & (1 << (index & 31)))
            if slot.active and not cached:
                raise AssertionError(f"unsafe false negative at slot {index}")


def clone_player(source: PlayerModel) -> PlayerModel:
    result = PlayerModel()
    result.bullet_cancel_item_type = source.bullet_cancel_item_type
    result.cancel_regions = [Region(**vars(slot)) for slot in source.cancel_regions]
    return result


def canonical_check(player: PlayerModel, trace: list[int] | None = None) -> int:
    for index, slot in enumerate(player.cancel_regions):
        if not slot.active:
            continue
        if trace is not None:
            trace.append(index)
        if not slot.overlaps:
            continue
        player.bullet_cancel_item_type = slot.collision_value
        slot.hit_accumulator += 1
        return 2
    return 0


def optimized_check(
    player: PlayerModel,
    sidecar: CancelSidecarModel,
    trace: list[int] | None = None,
) -> int:
    sidecar.ensure(player)
    if not sidecar.cancel_any:
        return 0
    # Materialize the original upper bound: production similarly captures
    # cancelLastActiveWord before repairing any stale positive during the scan.
    indices = list(sidecar.indices())
    for index in indices:
        slot = player.cancel_regions[index]
        if not slot.active:
            sidecar.clear_bit(index)
            continue
        if trace is not None:
            trace.append(index)
        if not slot.overlaps:
            continue
        player.bullet_cancel_item_type = slot.collision_value
        slot.hit_accumulator += 1
        return 2
    return 0


def canonical_create(player: PlayerModel, value: Region) -> int:
    index = 191
    for candidate in range(191):
        if not player.cancel_regions[candidate].active:
            index = candidate
            break
    player.cancel_regions[index] = Region(**vars(value))
    player.cancel_regions[index].active = True
    return index


def optimized_create(
    player: PlayerModel, sidecar: CancelSidecarModel, value: Region
) -> int:
    sidecar.ensure(player)
    index = 191
    for candidate in range(191):
        if player.cancel_regions[candidate].active:
            sidecar.set_bit(candidate)
            continue
        sidecar.clear_bit(candidate)
        index = candidate
        break
    player.cancel_regions[index] = Region(**vars(value))
    player.cancel_regions[index].active = True
    sidecar.set_bit(index)
    return index


class CancelEmptyDifferentialTests(unittest.TestCase):
    def assert_same(self, canonical: PlayerModel, optimized: PlayerModel) -> None:
        self.assertEqual(canonical.snapshot(), optimized.snapshot())

    def test_empty_fast_return_preserves_callers_item_type(self) -> None:
        canonical = PlayerModel()
        optimized = clone_player(canonical)
        sidecar = CancelSidecarModel()
        sidecar.reset(optimized)

        for caller_value in (6, 91):
            canonical.bullet_cancel_item_type = caller_value
            optimized.bullet_cancel_item_type = caller_value
            self.assertEqual(canonical_check(canonical), 0)
            self.assertEqual(optimized_check(optimized, sidecar), 0)
            self.assertEqual(optimized.bullet_cancel_item_type, caller_value)
        self.assertFalse(sidecar.cancel_any)
        self.assertEqual(sidecar.last_active_word, -1)
        sidecar.assert_safe(optimized)

    def test_slots_31_32_191_keep_first_hit_order(self) -> None:
        canonical = PlayerModel()
        for index in (31, 32, 191):
            canonical.cancel_regions[index] = Region(
                active=True,
                overlaps=index != 31,
                collision_value=1000 + index,
                serial=index,
            )
        optimized = clone_player(canonical)
        sidecar = CancelSidecarModel()
        sidecar.ensure(optimized)
        self.assertEqual(sidecar.last_active_word, 5)
        canonical_trace: list[int] = []
        optimized_trace: list[int] = []
        self.assertEqual(
            canonical_check(canonical, canonical_trace),
            optimized_check(optimized, sidecar, optimized_trace),
        )
        self.assertEqual(canonical_trace, [31, 32])
        self.assertEqual(optimized_trace, canonical_trace)
        self.assertEqual(canonical.bullet_cancel_item_type, 1032)
        self.assert_same(canonical, optimized)
        sidecar.assert_safe(optimized)

    def test_duplicate_hit_mutates_same_slot_twice(self) -> None:
        canonical = PlayerModel()
        canonical.cancel_regions[32] = Region(True, True, 7, 0, 32)
        canonical.cancel_regions[191] = Region(True, True, 9, 0, 191)
        optimized = clone_player(canonical)
        sidecar = CancelSidecarModel()
        sidecar.ensure(optimized)
        for _ in range(2):
            canonical.bullet_cancel_item_type = 6
            optimized.bullet_cancel_item_type = 6
            self.assertEqual(
                canonical_check(canonical), optimized_check(optimized, sidecar)
            )
            self.assert_same(canonical, optimized)
        self.assertEqual(canonical.cancel_regions[32].hit_accumulator, 2)
        self.assertEqual(canonical.cancel_regions[191].hit_accumulator, 0)

    def test_stale_positive_is_repaired_without_false_negative(self) -> None:
        canonical = PlayerModel()
        canonical.cancel_regions[191] = Region(True, False, 9, 0, 191)
        optimized = clone_player(canonical)
        sidecar = CancelSidecarModel()
        sidecar.ensure(optimized)

        # Direct deactivation writers may leave this safe one-way staleness.
        canonical.cancel_regions[191].active = False
        optimized.cancel_regions[191].active = False
        self.assertTrue(sidecar.cancel_any)
        self.assertEqual(canonical_check(canonical), optimized_check(optimized, sidecar))
        self.assertFalse(sidecar.cancel_any)
        self.assertEqual(sidecar.last_active_word, -1)
        self.assert_same(canonical, optimized)
        sidecar.assert_safe(optimized)

    def test_factory_slot191_rebuild_owner_and_memset_lifecycle(self) -> None:
        canonical = PlayerModel()
        for index in range(REGION_COUNT):
            canonical.cancel_regions[index] = Region(
                True, False, index, index, index
            )
        optimized = clone_player(canonical)
        sidecar = CancelSidecarModel()
        sidecar.ensure(optimized)
        replacement = Region(True, True, 77, 0, 9001)
        self.assertEqual(canonical_create(canonical, replacement), 191)
        self.assertEqual(optimized_create(optimized, sidecar, replacement), 191)
        self.assert_same(canonical, optimized)
        sidecar.assert_safe(optimized)

        # memset(Player) plus the registered reset leaves a provably empty
        # same-owner sidecar, including the nonzero -1 sentinel.
        canonical = PlayerModel()
        optimized = PlayerModel()
        sidecar.reset(optimized)
        self.assertEqual(canonical_check(canonical), optimized_check(optimized, sidecar))
        self.assertFalse(sidecar.cancel_any)
        self.assertEqual(sidecar.last_active_word, -1)

        # A different owner is rebuilt from authoritative slots before use.
        canonical_two = PlayerModel()
        canonical_two.cancel_regions[191] = Region(True, True, 191, 0, 191)
        optimized_two = clone_player(canonical_two)
        self.assertEqual(
            canonical_check(canonical_two), optimized_check(optimized_two, sidecar)
        )
        self.assert_same(canonical_two, optimized_two)
        sidecar.assert_safe(optimized_two)

    def test_25000_operation_deterministic_differential(self) -> None:
        rng = random.Random(0x54483038)
        canonical = PlayerModel()
        optimized = PlayerModel()
        sidecar = CancelSidecarModel()
        sidecar.reset(optimized)

        for step in range(25_000):
            operation = rng.randrange(8)
            index = rng.randrange(REGION_COUNT)
            if operation in (0, 1):
                value = Region(
                    active=True,
                    overlaps=rng.randrange(4) == 0,
                    collision_value=rng.randrange(-1, 10),
                    hit_accumulator=rng.randrange(12),
                    serial=step + 1,
                )
                self.assertEqual(
                    canonical_create(canonical, value),
                    optimized_create(optimized, sidecar, value),
                )
            elif operation == 2:
                # Known direct writers only deactivate: stale-positive-safe.
                canonical.cancel_regions[index].active = False
                optimized.cancel_regions[index].active = False
            elif operation == 3:
                sidecar.audit(optimized)
            elif operation == 4:
                canonical.bullet_cancel_item_type = 6
                optimized.bullet_cancel_item_type = 6
                self.assertEqual(
                    canonical_check(canonical),
                    optimized_check(optimized, sidecar),
                )
            elif operation == 5:
                overlaps = bool(rng.getrandbits(1))
                canonical.cancel_regions[index].overlaps = overlaps
                optimized.cancel_regions[index].overlaps = overlaps
            elif operation == 6:
                canonical = PlayerModel()
                optimized = PlayerModel()
                sidecar.reset(optimized)
            else:
                # Exercise last-word descent without creating a false negative.
                if optimized.cancel_regions[index].active:
                    canonical.cancel_regions[index].active = False
                    optimized.cancel_regions[index].active = False
                    sidecar.clear_bit(index)

            self.assert_same(canonical, optimized)
            sidecar.assert_safe(optimized)


class CancelEmptyStaticTests(unittest.TestCase):
    def test_default_off_stamped_dependency_and_fingerprint(self) -> None:
        self.assertIn(f"{FEATURE} ?= 0", MAKEFILE)
        self.assertIn(f"-D{FEATURE}=1", MAKEFILE)
        self.assertIn("cancel-empty-fastpath-0.stamp", MAKEFILE)
        self.assertIn("cancel-empty-fastpath-1.stamp", MAKEFILE)
        self.assertIn("$(CANCEL_EMPTY_FASTPATH_CONFIG_STAMP)", MAKEFILE)
        self.assertIn(
            f"{FEATURE}=1 requires TH08_PSP_PLAYER_SCAN_SIDECAR=1", MAKEFILE
        )
        self.assertIn("TH08_PSP_FEATURE_CANCEL_EMPTY_FASTPATH", MAIN_CPP)
        self.assertIn("CANCEL_EMPTY_FASTPATH=%d", MAIN_CPP)

    def test_empty_reject_and_last_word_bound_are_in_collision_consumer(self) -> None:
        body = function_body(PLAYER_CPP, "Player::CheckBulletCancelCollision(")
        ensure = body.index("PspEnsurePlayerScanSidecar(this);")
        reject = body.index("if (!g_PspPlayerScanSidecar.cancelAny)")
        loop = body.index("for (i = PspNextScanBit")
        self.assertLess(ensure, reject)
        self.assertLess(reject, loop)
        self.assertIn("cancelLastActiveWord + 1", body)
        self.assertIn("PspClearCancelScanBit(i);", body)
        self.assertIn("this->bulletCancelItemType = slot->collisionValue;", body)
        self.assertIn("slot->hitAccumulator++;", body)

        caller = function_body(PLAYER_CPP, "Player::CheckBulletCollision(")
        reset = caller.index("this->bulletCancelItemType = 6;")
        call = caller.index("this->CheckBulletCancelCollision(position, size)")
        self.assertLess(reset, call)

    def test_all_activation_writers_publish_metadata(self) -> None:
        for signature in (
            "Player::CreateRectCancelRegion(",
            "Player::CreateCircleCancelRegion(",
        ):
            body = function_body(PLAYER_CPP, signature)
            self.assertEqual(body.count("slot->active = true;"), 1)
            self.assertIn("PspSetCancelScanBit(index);", body)
            self.assertIn("PspClearCancelScanBit(index);", body)
        # The other two active writes are damage-region factories, not hidden
        # cancel-region producers.
        self.assertEqual(PLAYER_CPP.count("slot->active = true;"), 4)

        direct_writes = re.findall(
            r"(?:cancelRegion|damageRegion)->active\s*=\s*([^;]+);",
            PLAYER_BOMB_CPP,
        )
        self.assertGreater(len(direct_writes), 0)
        self.assertTrue(
            all(value.strip() in {"0", "false"} for value in direct_writes),
            msg=f"unsafe direct activation writer: {direct_writes}",
        )

    def test_rebuild_and_memset_lifecycle_maintain_both_fields(self) -> None:
        audit = function_body(PLAYER_CPP, "static void PspAuditRegionScanBits(")
        update = function_body(PLAYER_CPP, "void Player::UpdateCollisionRegions(")
        reset = function_body(PLAYER_CPP, "static void PspResetPlayerScanSidecar(")
        clear_all = function_body(PLAYER_CPP, "static inline void PspClearAllCancelScanBits(")
        for body in (audit, update):
            self.assertIn("PspClearAllCancelScanBits();", body)
            self.assertIn("PspSetCancelScanBit(", body)
        self.assertIn("memset(&g_PspPlayerScanSidecar, 0", reset)
        self.assertIn("cancelLastActiveWord = -1;", reset)
        self.assertIn("cancelAny = false;", clear_all)
        self.assertIn("cancelLastActiveWord = -1;", clear_all)

        register = function_body(PLAYER_CPP, "Player::RegisterChain(")
        added = function_body(PLAYER_CPP, "Player::AddedCallback(")
        self.assertIn("PspResetPlayerScanSidecar(player);", register)
        self.assertIn("PspResetPlayerScanSidecar(player);", added)


if __name__ == "__main__":
    unittest.main(verbosity=2)
