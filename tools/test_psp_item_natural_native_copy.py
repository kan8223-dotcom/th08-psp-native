#!/usr/bin/env python3
"""Focused safety/provenance gates for Item natural owned indexed copy."""

from __future__ import annotations

import hashlib
import re
import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PATCH_PATH = (
    ROOT
    / "deps/pspgl-ge4/pspgl-th08-native-indexed-copy-v1.patch"
)
ARCHIVE_PATH = (
    ROOT / "deps/pspgl-ge4/libGL_th08_ge4_indexed_copy_v1.a"
)
V3_ARCHIVE_PATH = ROOT / "deps/pspgl-ge4/libGL_th08_ge4.a"
MAKEFILE = (ROOT / "Makefile.psp").read_text(encoding="utf-8")
D3D = (ROOT / "src/modern/linux/d3d8_compat.cpp").read_text(
    encoding="utf-8"
)
INTERNAL = (ROOT / "src/modern/linux/d3d8_internal.hpp").read_text(
    encoding="utf-8"
)
ANM = (ROOT / "src/AnmManager.cpp").read_text(encoding="utf-8")
MAIN = (ROOT / "psp/main.cpp").read_text(encoding="utf-8")
MEMORY = (ROOT / "psp/memory_telemetry.cpp").read_text(encoding="utf-8")
README = (ROOT / "deps/pspgl-ge4/README.md").read_text(encoding="utf-8")
PATCH = PATCH_PATH.read_text(encoding="utf-8")

FEATURE = "TH08_PSP_ITEM_NATURAL_NATIVE_COPY"
HOOK = "__pspgl_th08_draw_native_indexed_quads_copy"
MARKER_SYMBOL = "__pspgl_th08_native_indexed_copy_marker"
PATCH_SHA = "a045436e26fafc7212ce5ec2699d841d1a19afcd1ff4f0156de78f66404c518f"
ARCHIVE_SHA = "3d63366aa076ee44627bcd5121cdb305c6816015b192943eaa22a9a818179607"


def added_source(patch: str) -> str:
    return "\n".join(
        line[1:]
        for line in patch.splitlines()
        if line.startswith("+") and not line.startswith("+++")
    )


def body(source: str, signature: str) -> str:
    start = source.index(signature)
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


def symbol_disassembly(path: Path, symbol: str) -> str:
    output = subprocess.run(
        ["psp-objdump", "-dr", str(path)],
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    match = re.search(
        rf"(?ms)^[0-9a-f]+ <{re.escape(symbol)}>:\n(.*?)(?=^[0-9a-f]+ <|\Z)",
        output,
    )
    if match is None:
        raise AssertionError(f"missing disassembly for {symbol}")
    # Relocation annotations include archive-local addresses; instructions and
    # target symbol names are the behavior authority for the frozen old hook.
    return re.sub(r"(?m)^\s*[0-9a-f]+:\s+", "", match.group(1)).strip()


class NativeCopyPatchTests(unittest.TestCase):
    def test_patch_is_post_v3_single_file_and_fingerprinted(self) -> None:
        self.assertEqual(hashlib.sha256(PATCH_PATH.read_bytes()).hexdigest(), PATCH_SHA)
        self.assertEqual(PATCH.count("diff --git "), 1)
        self.assertIn("index a0695fe..23f2c5d 100644", PATCH)
        self.assertIn("diff --git a/glDrawElements.c b/glDrawElements.c", PATCH)
        self.assertNotIn("glDrawArrays.c", PATCH)
        for token in (
            PATCH_SHA,
            "23f2c5d63d23bbaedc64ae88bf9029e337a9940f",
            "applied independently after v3",
            "UNTESTED ON HARDWARE",
        ):
            self.assertIn(token, README)

    def test_complete_preflight_and_owned_copy_precede_the_only_prim(self) -> None:
        native = body(added_source(PATCH), f"int {HOOK}(")
        allocate = native.index("__pspgl_buffer_new")
        map_buffer = native.index("__pspgl_buffer_map")
        copy_vertices = native.index("memcpy(copy, vertices, vertex_bytes)")
        copy_indices = native.index("memcpy(copy + vertex_bytes, indices, index_bytes)")
        unmap_buffer = native.index("__pspgl_buffer_unmap")
        render = native.index("__pspgl_context_render_prim")
        pin = native.index("__pspgl_dlist_pin_buffer")
        final_free = native.rindex("__pspgl_buffer_free(buffer)")
        success = native.index("return 1;", final_free)

        self.assertLess(native.rindex("return 0;"), render)
        self.assertLess(allocate, map_buffer)
        self.assertLess(map_buffer, copy_vertices)
        self.assertLess(copy_vertices, copy_indices)
        self.assertLess(copy_indices, unmap_buffer)
        self.assertLess(unmap_buffer, render)
        self.assertLess(render, pin)
        self.assertLess(pin, final_free)
        self.assertLess(final_free, success)
        self.assertEqual(native.count("__pspgl_buffer_new"), 1)
        self.assertEqual(native.count("__pspgl_buffer_map"), 1)
        self.assertEqual(native.count("memcpy("), 2)
        self.assertEqual(native.count("__pspgl_context_render_prim"), 1)
        self.assertEqual(native.count("__pspgl_dlist_pin_buffer"), 1)
        self.assertNotIn("return 0;", native[render:])

    def test_preflight_is_exact_bounded_and_overflow_safe(self) -> None:
        native = body(added_source(PATCH), f"int {HOOK}(")
        allocate = native.index("__pspgl_buffer_new")
        for token in (
            "pspgl_curctx == NULL",
            "pspgl_curctx->draw == NULL",
            "*pspgl_curctx->draw->draw == NULL",
            "__pspgl_actuallist != NULL",
            "quad_count == 0",
            "quad_count > 0x600u",
            "quad_count > uint_max / 4u",
            "quad_count > uint_max / (4u * vertex_stride)",
            "quad_count > 0xffffu / 6u",
            "vertices == NULL",
            "vertex_bytes == 0",
            "vertex_begin & (sizeof(float) - 1u)",
            "indices == NULL",
            "index_bytes == 0",
            "index_begin & (sizeof(*indices) - 1u)",
            "vertex_end < vertex_begin",
            "index_end < index_begin",
            "vertex_bytes != expected_vertex_bytes",
            "index_bytes != expected_index_bytes",
            "vertex_bytes > uint_max - index_bytes",
        ):
            self.assertLess(native.index(token), allocate, token)
        self.assertIn("vertex_count = quad_count * 4u", native)
        self.assertIn("index_count = quad_count * 6u", native)

    def test_render_uses_only_owned_buffer_and_no_bullet_or_raw_gu_path(self) -> None:
        native = body(added_source(PATCH), f"int {HOOK}(")
        render = native[native.index("__pspgl_context_render_prim") :]
        self.assertIn("vertex_format, buffer->base", render)
        self.assertIn("(unsigned char *)buffer->base + vertex_bytes", render)
        self.assertNotIn("vertex_format, vertices", render)
        self.assertNotIn(", indices)", render)
        for forbidden in (
            "__pspgl_th08_draw_native_indexed_triangles",
            "RenderResourceArena",
            "pspBullet",
            "sceKernelDcacheWritebackRange",
            "sceGuStart",
            "sceGuFinish",
            "sceGuDrawArray",
            "eDRAM",
        ):
            self.assertNotIn(forbidden, native)
        for token in (
            "GE_TRIANGLES",
            "GE_TEXTURE_32BITF",
            "GE_COLOR_8888",
            "GE_VERTEX_32BITF",
            "GE_TRANSFORM_3D",
            "GE_VINDEX_16BIT",
            "GL_STREAM_DRAW_ARB",
            "GL_WRITE_ONLY_ARB",
            "BF_PINNED_RD",
        ):
            self.assertIn(token, native)


class NativeCopyArchiveTests(unittest.TestCase):
    def test_archive_hash_size_exports_markers_and_abi(self) -> None:
        self.assertEqual(ARCHIVE_PATH.stat().st_size, 1_685_392)
        self.assertEqual(hashlib.sha256(ARCHIVE_PATH.read_bytes()).hexdigest(), ARCHIVE_SHA)
        old = exports(V3_ARCHIVE_PATH)
        new = exports(ARCHIVE_PATH)
        self.assertEqual(len(old), 304)
        self.assertEqual(len(new), 306)
        self.assertEqual(new - old, {HOOK, MARKER_SYMBOL})
        self.assertFalse(old - new)

        nm = subprocess.run(
            ["psp-nm", "-S", "-g", "--defined-only", str(ARCHIVE_PATH)],
            check=True,
            capture_output=True,
            text=True,
        ).stdout
        self.assertRegex(nm, rf"(?m)^[0-9a-f]+ 000001b4 T {HOOK}$")
        self.assertRegex(nm, rf"(?m)^[0-9a-f]+ 0000006f R {MARKER_SYMBOL}$")
        for symbol in (
            "__pspgl_th08_draw_native_indexed_triangles",
            "__pspgl_th08_draw_native_sprite_pairs_copy",
            "__pspgl_th08_native_submit_marker",
            "__pspgl_th08_ge4_fork_marker",
        ):
            self.assertIn(symbol, nm)
        archive = ARCHIVE_PATH.read_bytes()
        self.assertIn(
            b"pspGL TH08 native indexed-quad copy v1 upstream "
            b"de4260adf56d06516ec46018d404ca77e0b61748 base native-submit-v3",
            archive,
        )

        headers = subprocess.run(
            ["psp-readelf", "-h", str(ARCHIVE_PATH)],
            check=True,
            capture_output=True,
            text=True,
        ).stdout
        self.assertEqual(set(re.findall(r"(?m)^\s*Class:\s+(.+)$", headers)), {"ELF32"})
        self.assertEqual(
            set(re.findall(r"(?m)^\s*Machine:\s+(.+)$", headers)),
            {"MIPS R3000"},
        )
        self.assertEqual(
            set(re.findall(r"(?m)^\s*Flags:\s+(.+)$", headers)),
            {"0x10a23001, noreorder, allegrex, eabi32, mips2"},
        )

    def test_existing_bullet_no_copy_hook_disassembly_is_unchanged(self) -> None:
        symbol = "__pspgl_th08_draw_native_indexed_triangles"
        self.assertEqual(
            symbol_disassembly(V3_ARCHIVE_PATH, symbol),
            symbol_disassembly(ARCHIVE_PATH, symbol),
        )


class NativeCopyIntegrationTests(unittest.TestCase):
    def test_default_off_requirements_archive_selection_and_provenance_gates(self) -> None:
        self.assertIn(f"{FEATURE} ?= 0", MAKEFILE)
        self.assertIn(f"-D{FEATURE}=1", MAKEFILE)
        self.assertIn(
            f"{FEATURE}=1 requires TH08_PSP_ITEM_NATURAL_QUADS=1",
            MAKEFILE,
        )
        self.assertIn(
            f"{FEATURE} is isolated from mixed PSPGL product archives",
            MAKEFILE,
        )
        self.assertIn("item-natural-native-copy-0.stamp", MAKEFILE)
        self.assertIn("item-natural-native-copy-1.stamp", MAKEFILE)
        self.assertIn("$(PSPGL_GE4_INDEXED_COPY_ARCHIVE)", MAKEFILE)
        self.assertIn(ARCHIVE_SHA, MAKEFILE)
        self.assertIn(PATCH_SHA, MAKEFILE)
        self.assertIn(
            "707474f35d89b556e080aa21766fe5689fd55829b28b267bcbc0d13e1a418941",
            MAKEFILE,
        )
        selection = re.search(
            rf"ifeq \(\$\({FEATURE}\),1\).*?"
            r"PSPGL_GE4_SELECTED_ARCHIVE := \$\(PSPGL_GE4_INDEXED_COPY_ARCHIVE\)",
            MAKEFILE,
            re.DOTALL,
        )
        self.assertIsNotNone(selection)

    def test_backend_native_success_and_atomic_one_draw_client_fallback(self) -> None:
        submit = body(D3D, "PspItemNaturalQuadSubmitResult DrawPspItemNaturalQuads(")
        prepare = submit.index("PrepareState(true)")
        native = submit.index(f"{HOOK}(")
        native_success = submit.index("return PSP_ITEM_NATURAL_SUBMIT_NATIVE", native)
        client_state = submit.index("glEnableClientState(GL_VERTEX_ARRAY)", native_success)
        client_draw = submit.index("glDrawElements(", client_state)
        client_result = submit.index(
            "PSP_ITEM_NATURAL_SUBMIT_CLIENT_AFTER_NATIVE_REJECT", client_draw
        )
        self.assertLess(prepare, native)
        self.assertLess(native, native_success)
        self.assertLess(native_success, client_state)
        self.assertLess(client_state, client_draw)
        self.assertLess(client_draw, client_result)
        self.assertEqual(submit.count("PrepareState(true)"), 1)
        self.assertEqual(submit.count("glDrawElements("), 1)
        self.assertEqual(submit.count(f"{HOOK}("), 1)
        self.assertIn("nativeCopyRejected", submit)
        self.assertIn("no PRIM", submit)
        self.assertIn("RenderPerfNoteDraw(indexCount)", submit)
        # Native success skips only the nine public client-array calls; it
        # retains one logical draw/index count and all PrepareState work.
        self.assertGreater(
            submit.index("RenderPerfNoteStateEmitted(9U)"), client_draw
        )

    def test_frontend_accounts_native_rejection_without_canonical_redraw(self) -> None:
        submit = body(
            ANM,
            "PspItemNaturalFlushResult TrySubmitPspItemNaturalQuadsAtCanonicalBoundary(",
        )
        self.assertIn("PSP_ITEM_NATURAL_SUBMIT_CLIENT_AFTER_NATIVE_REJECT", submit)
        fallback_result = submit.index(
            "PSP_ITEM_NATURAL_SUBMIT_CLIENT_AFTER_NATIVE_REJECT"
        )
        native_fallback = submit.index("++stats.nativeFallbacks", fallback_result)
        client_submit = submit.index("++stats.clientFallbackSubmits", native_fallback)
        self.assertLess(fallback_result, native_fallback)
        self.assertLess(native_fallback, client_submit)
        self.assertNotIn("DrawPrimitive", submit)
        self.assertNotIn("PrepareState", submit)

    def test_identity_telemetry_and_storage_contract_remain_explicit(self) -> None:
        self.assertIn("TH08_PSP_FEATURE_ITEM_NATURAL_NATIVE_COPY", MAIN)
        self.assertIn("ITEM_NATURAL_NATIVE_COPY=%d", MAIN)
        for token in (
            "native_no_copy_attempted=0",
            "native_copy_enabled=%d",
            "native_copy_owned_same_call=%d",
            "client_owned_same_call=1",
            "native_fallbacks=%lu",
        ):
            self.assertIn(token, MEMORY)
        self.assertIn("sizeof(PspItemNaturalQuadStats) == 136U", ANM)
        self.assertIn("sizeof(PspItemNaturalQuadStorage) == 152U", ANM)
        self.assertIn(
            "PSP_ITEM_NATURAL_SUBMIT_CLIENT_AFTER_NATIVE_REJECT", INTERNAL
        )


if __name__ == "__main__":
    unittest.main()
