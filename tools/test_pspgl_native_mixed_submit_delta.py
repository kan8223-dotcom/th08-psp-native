#!/usr/bin/env python3
"""Static and ordering-model gates for the post-v3 PSPGL mixed-submit delta."""

from __future__ import annotations

import hashlib
import random
import re
import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DELTA_PATH = (
    ROOT / "deps" / "pspgl-ge4" / "pspgl-th08-native-mixed-submit-v1.patch"
)
V3_PATH = ROOT / "deps" / "pspgl-ge4" / "pspgl-th08-native-submit-v3.patch"
README_PATH = ROOT / "deps" / "pspgl-ge4" / "README.md"
MAKEFILE_PATH = ROOT / "Makefile.psp"
V3_ARCHIVE_PATH = ROOT / "deps" / "pspgl-ge4" / "libGL_th08_ge4.a"
MIXED_ARCHIVE_PATH = (
    ROOT / "deps" / "pspgl-ge4" / "libGL_th08_ge4_mixed_v1.a"
)
DELTA = DELTA_PATH.read_text(encoding="utf-8")
V3 = V3_PATH.read_text(encoding="utf-8")
README = README_PATH.read_text(encoding="utf-8")
MAKEFILE = MAKEFILE_PATH.read_text(encoding="utf-8")

UINT_MAX = (1 << 32) - 1
VERTEX_STRIDE = 24


def added_source(patch: str) -> str:
    """Recover only source lines added by a unified diff."""
    return "\n".join(
        line[1:]
        for line in patch.splitlines()
        if line.startswith("+") and not line.startswith("+++")
    )


def body(source: str, signature: str) -> str:
    start = source.rindex(signature)
    opening = source.index("{", start)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    raise AssertionError(f"unterminated function: {signature}")


def range_valid(address: int | None, byte_count: int, alignment: int) -> bool:
    if address is None or byte_count == 0 or address % alignment != 0:
        return False
    return address + byte_count <= UINT_MAX


def mixed_submit_model(
    *,
    pair_address: int | None,
    pair_bytes: int,
    quad_address: int | None,
    quad_bytes: int,
    quad_count: int,
    index_address: int | None,
    index_bytes: int,
    index_count: int,
    context_valid: bool = True,
    compiling_list: bool = False,
) -> tuple[bool, list[str]]:
    """Mirror the C preflight and record cache/PRIM side effects."""
    events: list[str] = []
    has_pairs = pair_bytes != 0
    has_quads = quad_count != 0

    if not context_valid or compiling_list:
        return False, events
    if (pair_address is None) != (pair_bytes == 0):
        return False, events
    if not has_pairs and not has_quads:
        return False, events

    pair_vertex_count = 0
    if has_pairs:
        if not range_valid(pair_address, pair_bytes, 4):
            return False, events
        if pair_bytes % VERTEX_STRIDE != 0:
            return False, events
        pair_vertex_count = pair_bytes // VERTEX_STRIDE
        if not 0 < pair_vertex_count <= 0xFFFF or pair_vertex_count % 2:
            return False, events

    if not has_quads:
        if any(
            (
                quad_address is not None,
                quad_bytes != 0,
                index_address is not None,
                index_bytes != 0,
                index_count != 0,
            )
        ):
            return False, events
    else:
        if (
            quad_count > UINT_MAX // 4
            or quad_count > UINT_MAX // (4 * VERTEX_STRIDE)
            or quad_count > UINT_MAX // 6
        ):
            return False, events
        quad_vertex_count = quad_count * 4
        expected_quad_bytes = quad_vertex_count * VERTEX_STRIDE
        expected_index_count = quad_count * 6
        if expected_index_count > UINT_MAX // 2:
            return False, events
        expected_index_bytes = expected_index_count * 2
        if not range_valid(quad_address, quad_bytes, 4):
            return False, events
        if not range_valid(index_address, index_bytes, 2):
            return False, events
        if (
            quad_bytes % VERTEX_STRIDE != 0
            or quad_bytes != expected_quad_bytes
            or not 0 < quad_vertex_count <= 0x10000
            or quad_vertex_count % 4 != 0
        ):
            return False, events
        if (
            not 0 < index_count <= 0xFFFF
            or index_count % 3 != 0
            or index_count % 6 != 0
            or index_count != expected_index_count
            or index_bytes != expected_index_bytes
        ):
            return False, events

    if has_pairs:
        events.append("WB_PAIR")
    if has_quads:
        events.extend(("WB_QUAD", "WB_INDEX"))
    if has_pairs:
        events.append("PRIM_SPRITES")
    if has_quads:
        events.append("PRIM_TRIANGLES")
    return True, events


def valid_args(pair_quads: int = 3, indexed_quads: int = 5) -> dict[str, object]:
    return {
        "pair_address": 0x08800000 if pair_quads else None,
        "pair_bytes": pair_quads * 2 * VERTEX_STRIDE,
        "quad_address": 0x09000000 if indexed_quads else None,
        "quad_bytes": indexed_quads * 4 * VERTEX_STRIDE,
        "quad_count": indexed_quads,
        "index_address": 0x09800000 if indexed_quads else None,
        "index_bytes": indexed_quads * 6 * 2,
        "index_count": indexed_quads * 6,
    }


class MixedSubmitDeltaStaticTests(unittest.TestCase):
    def test_v3_is_unchanged_and_delta_is_independent_and_fingerprinted(self) -> None:
        self.assertEqual(
            hashlib.sha256(V3_PATH.read_bytes()).hexdigest(),
            "ae70bc1a492212989a87fa1656ff002c6b6f5d8f2ade2b2efa50983dc77aaf47",
        )
        self.assertEqual(
            hashlib.sha256(DELTA_PATH.read_bytes()).hexdigest(),
            "26e76be666d31e9adea5660edcf67290cf747e8104260484345c596cca4e828a",
        )
        self.assertEqual(DELTA.count("diff --git "), 1)
        self.assertIn(
            "diff --git a/glDrawElements.c b/glDrawElements.c", DELTA
        )
        self.assertIn("index a0695fe..28ebed5 100644", DELTA)
        self.assertNotIn("__pspgl_th08_native_submit_marker", DELTA)
        for token in (
            "applied after v3",
            "not present in the frozen archive",
            "separately gated and default-off",
            "BUILT AND ABI/NM",
            "SELECTED BY TH08 ONLY WHEN",
            "STAGE 5 PPSSPP REPLAY/SURFACE",
            "UNTESTED ON HARDWARE",
            "26e76be666d31e9adea5660edcf67290cf747e8104260484345c596cca4e828a",
        ):
            self.assertIn(token, README)

        selection = re.search(
            r"PSPGL_GE4_SELECTED_ARCHIVE := \$\(PSPGL_GE4_ARCHIVE\).*?"
            r"ifneq \(\$\(filter 1,\$\(TH08_PSP_BULLET_MIXED_QUADS_FASTPATH\) "
            r"\$\(TH08_PSP_ITEM_MIXED_QUADS_FASTPATH\)\),\).*?"
            r"PSPGL_GE4_SELECTED_ARCHIVE := \$\(PSPGL_GE4_MIXED_ARCHIVE\)",
            MAKEFILE,
            re.DOTALL,
        )
        self.assertIsNotNone(selection)

    def test_reproducible_archive_hash_symbols_markers_and_abi_are_frozen(self) -> None:
        self.assertEqual(V3_ARCHIVE_PATH.stat().st_size, 1_679_664)
        self.assertEqual(
            hashlib.sha256(V3_ARCHIVE_PATH.read_bytes()).hexdigest(),
            "3711ea969b85c839e6d1b36e7faf3ea5922e339a5f1b800ac50f25a45aa226ce",
        )
        self.assertEqual(MIXED_ARCHIVE_PATH.stat().st_size, 1_683_644)
        self.assertEqual(
            hashlib.sha256(MIXED_ARCHIVE_PATH.read_bytes()).hexdigest(),
            "b401e0f924ffdaffca62ac62e16f58000dd7b0b7d862675124f5289252ead530",
        )

        def exports(path: Path) -> set[str]:
            output = subprocess.run(
                ["psp-nm", "-P", "-g", "--defined-only", str(path)],
                check=True,
                capture_output=True,
                text=True,
            ).stdout
            return {
                line.split()[0]
                for line in output.splitlines()
                if line and not line.endswith(":")
            }

        old_exports = exports(V3_ARCHIVE_PATH)
        mixed_exports = exports(MIXED_ARCHIVE_PATH)
        self.assertEqual(len(old_exports), 304)
        self.assertEqual(len(mixed_exports), 305)
        self.assertEqual(
            mixed_exports - old_exports,
            {"__pspgl_th08_draw_native_mixed_quads"},
        )
        self.assertFalse(old_exports - mixed_exports)

        sized_nm = subprocess.run(
            ["psp-nm", "-S", "-g", "--defined-only", str(MIXED_ARCHIVE_PATH)],
            check=True,
            capture_output=True,
            text=True,
        ).stdout
        self.assertRegex(
            sized_nm,
            r"(?m)^00000198 000002d8 T __pspgl_th08_draw_native_mixed_quads$",
        )
        for symbol in (
            "__pspgl_th08_draw_native_sprite_pairs_copy",
            "__pspgl_th08_draw_native_indexed_triangles",
            "__pspgl_th08_native_submit_marker",
            "__pspgl_th08_ge4_fork_marker",
        ):
            self.assertIn(symbol, sized_nm)

        archive_bytes = MIXED_ARCHIVE_PATH.read_bytes()
        for marker in (
            b"pspGL TH08 GE4 fork v1 upstream "
            b"de4260adf56d06516ec46018d404ca77e0b61748",
            b"pspGL TH08 native submit v3 indexed-no-copy sprite-copy upstream "
            b"de4260adf56d06516ec46018d404ca77e0b61748 base ge4-v1",
        ):
            self.assertIn(marker, archive_bytes)

        elf_headers = subprocess.run(
            ["psp-readelf", "-h", str(MIXED_ARCHIVE_PATH)],
            check=True,
            capture_output=True,
            text=True,
        ).stdout
        self.assertEqual(
            set(re.findall(r"(?m)^\s*Class:\s+(.+)$", elf_headers)),
            {"ELF32"},
        )
        self.assertEqual(
            set(re.findall(r"(?m)^\s*Data:\s+(.+)$", elf_headers)),
            {"2's complement, little endian"},
        )
        self.assertEqual(
            set(re.findall(r"(?m)^\s*Machine:\s+(.+)$", elf_headers)),
            {"MIPS R3000"},
        )
        self.assertEqual(
            set(re.findall(r"(?m)^\s*Flags:\s+(.+)$", elf_headers)),
            {"0x10a23001, noreorder, allegrex, eabi32, mips2"},
        )

        for token in (
            "a0695fe85617bdb060d8251a45fe2dba109d6f28",
            "28ebed5834af2f1a435459a37654ec6723b91711",
            "b401e0f924ffdaffca62ac62e16f58000dd7b0b7d862675124f5289252ead530",
            "SOURCE_DATE_EPOCH=1648032239",
            "byte-identical",
            "304 symbols plus one",
            "TH08_PSP_BULLET_MIXED_QUADS_FASTPATH=1",
        ):
            self.assertIn(token, README)

    def test_unified_diff_hunk_counts_are_internally_valid(self) -> None:
        lines = DELTA.splitlines()
        hunk_re = re.compile(
            r"^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@"
        )
        found = 0
        index = 0
        while index < len(lines):
            match = hunk_re.match(lines[index])
            if match is None:
                index += 1
                continue
            found += 1
            declared_old = int(match.group(2) or "1")
            declared_new = int(match.group(4) or "1")
            actual_old = 0
            actual_new = 0
            index += 1
            while index < len(lines) and not lines[index].startswith(("@@", "diff --git")):
                prefix = lines[index][:1]
                if prefix == " ":
                    actual_old += 1
                    actual_new += 1
                elif prefix == "-":
                    actual_old += 1
                elif prefix == "+":
                    actual_new += 1
                elif lines[index] != r"\ No newline at end of file":
                    self.fail(f"invalid unified-diff line: {lines[index]!r}")
                index += 1
            self.assertEqual(actual_old, declared_old)
            self.assertEqual(actual_new, declared_new)
        self.assertEqual(found, 2)

    def test_complete_preflight_precedes_every_writeback_and_prim(self) -> None:
        native = body(
            added_source(DELTA),
            "int __pspgl_th08_draw_native_mixed_quads(",
        )
        first_writeback = native.index("sceKernelDcacheWritebackRange")
        first_prim = native.index("__pspgl_context_render_prim")
        self.assertLess(native.rindex("return 0;"), first_writeback)
        self.assertLess(native.rindex("sceKernelDcacheWritebackRange"), first_prim)
        for token in (
            "pspgl_curctx == NULL",
            "__pspgl_actuallist != NULL",
            "(pair_vertices == NULL) != (pair_vertex_bytes == 0)",
            "!has_pairs && !has_quads",
            "th08_native_range_valid(pair_vertices",
            "pair_vertex_bytes % vertex_stride",
            "pair_vertex_count > 0xffffu",
            "pair_vertex_count % 2u",
            "quad_count > uint_max / 4u",
            "quad_count > uint_max / (4u * vertex_stride)",
            "quad_vertex_bytes % vertex_stride",
            "quad_vertex_count > 0x10000u",
            "quad_vertex_count % 4u",
            "quad_index_count > 0xffffu",
            "quad_index_count % 3u",
            "quad_index_count % 6u",
            "quad_index_bytes != expected_quad_index_bytes",
        ):
            self.assertLess(native.index(token), first_writeback, token)

        helper_source = added_source(V3)
        for token in (
            "uintptr_t begin = (uintptr_t)address",
            "(begin & (alignment - 1)) != 0",
            "end = begin + (uintptr_t)bytes",
            "return end >= begin",
        ):
            self.assertIn(token, helper_source)

    def test_emit_order_formats_and_current_list_owner_are_exact(self) -> None:
        native = body(
            added_source(DELTA),
            "int __pspgl_th08_draw_native_mixed_quads(",
        )
        self.assertEqual(native.count("__pspgl_context_render_prim"), 2)
        self.assertLess(native.index("GE_SPRITES"), native.index("GE_TRIANGLES"))
        sprite_format = native[
            native.index("const unsigned sprite_vertex_format") :
            native.index("const unsigned indexed_vertex_format")
        ]
        indexed_format = native[
            native.index("const unsigned indexed_vertex_format") :
            native.index("const unsigned uint_max")
        ]
        for token in (
            "GE_TEXTURE_32BITF",
            "GE_COLOR_8888",
            "GE_VERTEX_32BITF",
            "GE_TRANSFORM_3D",
        ):
            self.assertIn(token, sprite_format)
            self.assertIn(token, indexed_format)
        self.assertNotIn("GE_VINDEX_16BIT", sprite_format)
        self.assertIn("GE_VINDEX_16BIT", indexed_format)
        for forbidden in (
            "sceGuStart",
            "sceGuFinish",
            "sceGuDrawArray",
            "__pspgl_buffer_new",
            "sceGuGetMemory",
        ):
            self.assertNotIn(forbidden, native)
        self.assertIsNone(
            re.search(
                r"\b(?:malloc|calloc|realloc|free|memcpy|memmove)\s*\(",
                native,
            )
        )

    def test_readme_pins_index_authority_lifetime_and_no_reordering(self) -> None:
        for token in (
            "immutable index authority",
            "0,1,2,1,2,3",
            "quad_count * 6",
            "immutable and",
            "live until the present fence",
            "Reordering quads",
            "state-key change",
        ):
            self.assertIn(token, README)


class MixedSubmitModelTests(unittest.TestCase):
    def test_pair_only_suffix_only_and_mixed_emit_in_contract_order(self) -> None:
        ok, events = mixed_submit_model(**valid_args(3, 5))
        self.assertTrue(ok)
        self.assertEqual(
            events,
            [
                "WB_PAIR",
                "WB_QUAD",
                "WB_INDEX",
                "PRIM_SPRITES",
                "PRIM_TRIANGLES",
            ],
        )
        ok, events = mixed_submit_model(**valid_args(3, 0))
        self.assertTrue(ok)
        self.assertEqual(events, ["WB_PAIR", "PRIM_SPRITES"])
        ok, events = mixed_submit_model(**valid_args(0, 5))
        self.assertTrue(ok)
        self.assertEqual(events, ["WB_QUAD", "WB_INDEX", "PRIM_TRIANGLES"])

    def test_invalid_mutations_reject_before_first_prim(self) -> None:
        base = valid_args(3, 5)
        mutations: list[dict[str, object]] = [
            {"context_valid": False},
            {"compiling_list": True},
            {"pair_address": None},
            {"pair_address": 0x08800002},
            {"pair_bytes": 5 * VERTEX_STRIDE},
            {"pair_bytes": 6 * VERTEX_STRIDE + 1},
            {"pair_address": UINT_MAX - 7, "pair_bytes": 8},
            {"quad_address": None},
            {"quad_address": 0x09000002},
            {"quad_bytes": 5 * 4 * VERTEX_STRIDE - VERTEX_STRIDE},
            {"quad_count": UINT_MAX // 4 + 1},
            {"index_address": None},
            {"index_address": 0x09800001},
            {"index_count": 29},
            {"index_count": 30, "index_bytes": 58},
            {"index_address": UINT_MAX - 5, "index_bytes": 60},
        ]
        for mutation in mutations:
            args = dict(base)
            args.update(mutation)
            with self.subTest(mutation=mutation):
                ok, events = mixed_submit_model(**args)
                self.assertFalse(ok)
                self.assertFalse(any(event.startswith("PRIM_") for event in events))
                self.assertEqual(events, [])

        ok, events = mixed_submit_model(**valid_args(0, 0))
        self.assertFalse(ok)
        self.assertEqual(events, [])

    def test_random_prefix_suffix_geometry_preserves_original_quad_order(self) -> None:
        rng = random.Random(0x08_2A_4A_6A)
        canonical = (0, 1, 2, 1, 2, 3)
        for _ in range(20_000):
            total = rng.randrange(1, 65)
            prefix_count = rng.randrange(total + 1)
            labels = [rng.getrandbits(64) for _ in range(total)]
            prefix = labels[:prefix_count]
            suffix = labels[prefix_count:]

            pair_vertices = [
                (label, corner)
                for label in prefix
                for corner in (0, 3)
            ]
            quad_vertices = [
                (label, corner)
                for label in suffix
                for corner in range(4)
            ]
            quad_indices = [
                quad * 4 + corner
                for quad in range(len(suffix))
                for corner in canonical
            ]

            rendered: list[int] = []
            for vertex in range(0, len(pair_vertices), 2):
                first, second = pair_vertices[vertex : vertex + 2]
                self.assertEqual(first[0], second[0])
                rendered.append(first[0])
            for index in range(0, len(quad_indices), 6):
                six = quad_indices[index : index + 6]
                quad_labels = {quad_vertices[position][0] for position in six}
                self.assertEqual(len(quad_labels), 1)
                rendered.append(next(iter(quad_labels)))

            self.assertEqual(rendered, labels)
            ok, events = mixed_submit_model(
                **valid_args(len(prefix), len(suffix))
            )
            self.assertTrue(ok)
            expected_prims = []
            if prefix:
                expected_prims.append("PRIM_SPRITES")
            if suffix:
                expected_prims.append("PRIM_TRIANGLES")
            self.assertEqual(
                [event for event in events if event.startswith("PRIM_")],
                expected_prims,
            )


if __name__ == "__main__":
    unittest.main(verbosity=2)
