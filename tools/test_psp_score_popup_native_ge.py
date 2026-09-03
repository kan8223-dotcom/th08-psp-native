#!/usr/bin/env python3
"""Static M0 gates for the score-popup-only PSPGL GE_SPRITES submit."""

from __future__ import annotations

import hashlib
import re
import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAKEFILE = (ROOT / "Makefile.psp").read_text(encoding="utf-8")
MAIN = (ROOT / "psp" / "main.cpp").read_text(encoding="utf-8")
D3D = (ROOT / "src/modern/linux/d3d8_compat.cpp").read_text(encoding="utf-8")
INTERNAL = (ROOT / "src/modern/linux/d3d8_internal.hpp").read_text(
    encoding="utf-8"
)
MEMORY = (ROOT / "psp/memory_telemetry.cpp").read_text(encoding="utf-8")
ANM = (ROOT / "src/AnmManager.cpp").read_text(encoding="utf-8")
PATCH_PATH = ROOT / "deps/pspgl-ge4/pspgl-th08-native-submit-v3.patch"
PATCH = PATCH_PATH.read_text(encoding="utf-8")
README = (ROOT / "deps/pspgl-ge4/README.md").read_text(encoding="utf-8")
ARCHIVE = ROOT / "deps/pspgl-ge4/libGL_th08_ge4.a"


def body(source: str, signature: str, *, last: bool = False) -> str:
    start = source.rindex(signature) if last else source.index(signature)
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


class ScorePopupNativeGeTests(unittest.TestCase):
    def test_gate_is_default_off_stamped_fingerprinted_and_requires_batch(self) -> None:
        for token in (
            "TH08_PSP_SCORE_POPUP_NATIVE_GE ?= 0",
            "score-popup-native-ge-0.stamp",
            "score-popup-native-ge-1.stamp",
            "-DTH08_PSP_SCORE_POPUP_NATIVE_GE=1",
            "requires TH08_PSP_ASCII_POPUP_BATCH=1",
            "SCORE_POPUP_NATIVE_GE=%d",
            "TH08_PSP_FEATURE_SCORE_POPUP_NATIVE_GE",
        ):
            self.assertIn(token, MAKEFILE + MAIN)

    def test_archive_patch_hash_marker_and_symbols_are_frozen(self) -> None:
        patch_hash = hashlib.sha256(PATCH_PATH.read_bytes()).hexdigest()
        archive_hash = hashlib.sha256(ARCHIVE.read_bytes()).hexdigest()
        self.assertEqual(
            patch_hash,
            "ae70bc1a492212989a87fa1656ff002c6b6f5d8f2ade2b2efa50983dc77aaf47",
        )
        self.assertEqual(
            archive_hash,
            "3711ea969b85c839e6d1b36e7faf3ea5922e339a5f1b800ac50f25a45aa226ce",
        )
        self.assertEqual(ARCHIVE.stat().st_size, 1_679_664)
        for symbol in (
            "__pspgl_th08_draw_native_sprite_pairs_copy",
            "__pspgl_th08_draw_native_indexed_triangles",
            "__pspgl_th08_native_submit_marker",
        ):
            self.assertIn(symbol, MAKEFILE + README)
        nm = subprocess.run(
            ["psp-nm", "-g", "--defined-only", str(ARCHIVE)],
            check=True,
            capture_output=True,
            text=True,
        ).stdout
        self.assertRegex(
            nm,
            r"(?m)^\S+ T __pspgl_th08_draw_native_sprite_pairs_copy$",
        )
        arrays_delta, elements_delta = PATCH.split(
            "diff --git a/glDrawElements.c b/glDrawElements.c", 1
        )
        self.assertIn("__pspgl_th08_native_submit_marker", arrays_delta)
        self.assertNotIn("__pspgl_th08_native_submit_marker", elements_delta)
        self.assertIn(
            "src/modern/linux/d3d8_compat.o: $(PSPGL_GE4_ARCHIVE_STAMP)",
            MAKEFILE,
        )

    def test_private_submit_copies_to_pspgl_owned_stream_and_pins_it(self) -> None:
        native = body(
            PATCH,
            "int __pspgl_th08_draw_native_sprite_pairs_copy(",
            last=True,
        )
        for token in (
            "GL_STREAM_DRAW_ARB",
            "__pspgl_buffer_map(vbuf, GL_WRITE_ONLY_ARB)",
            "memcpy(copy, vertices, vertex_bytes)",
            "__pspgl_context_render_prim(pspgl_curctx, GE_SPRITES",
            "__pspgl_dlist_pin_buffer(vbuf, BF_PINNED_RD)",
            "GE_TEXTURE_32BITF",
            "GE_COLOR_8888",
            "GE_VERTEX_32BITF",
            "GE_TRANSFORM_3D",
        ):
            self.assertIn(token, native)
        self.assertLess(native.index("vertex_count > 0xffffu"), native.index("memcpy("))
        self.assertLess(
            native.index("memcpy("),
            native.index("__pspgl_context_render_prim(pspgl_curctx, GE_SPRITES"),
        )
        for forbidden in ("sceGuStart", "sceGuFinish", "sceGuDrawArray"):
            self.assertNotIn(forbidden, native)

    def test_private_submit_is_atomic_and_orders_stream_lifetime_exactly(self) -> None:
        native = body(
            PATCH,
            "int __pspgl_th08_draw_native_sprite_pairs_copy(",
            last=True,
        )
        allocate = native.index("vbuf = __pspgl_buffer_new")
        map_buffer = native.index("copy = __pspgl_buffer_map")
        copy_vertices = native.index("memcpy(copy, vertices, vertex_bytes)")
        unmap_buffer = native.index("__pspgl_buffer_unmap")
        emit = native.index("__pspgl_context_render_prim")
        pin = native.index("__pspgl_dlist_pin_buffer")
        release = native.index("__pspgl_buffer_free(vbuf);", pin)
        success = native.index("return 1;", release)

        # Every caller-controlled validation rejects before allocation and,
        # consequently, before the first possible primitive emission.
        for token in (
            "pspgl_curctx == NULL",
            "pspgl_curctx->draw == NULL",
            "*pspgl_curctx->draw->draw == NULL",
            "__pspgl_actuallist != NULL",
            "vertices == NULL",
            "vertex_bytes == 0",
            "(begin & (sizeof(float) - 1)) != 0",
            "end < begin",
            "(vertex_bytes % vertex_stride) != 0",
            "vertex_count > 0xffffu",
            "(vertex_count % 2u) != 0",
        ):
            self.assertLess(native.index(token), allocate, token)

        self.assertLess(allocate, map_buffer)
        self.assertLess(map_buffer, copy_vertices)
        self.assertLess(copy_vertices, unmap_buffer)
        self.assertLess(unmap_buffer, emit)
        self.assertLess(emit, pin)
        self.assertLess(pin, release)
        self.assertLess(release, success)

        # Allocation and mapping failures release any acquired ownership and
        # return before PRIM.  Once PRIM is emitted there is no fallible branch:
        # the buffer is pinned to that list before the caller reference drops.
        allocation_failure = native.index("if (vbuf == NULL)")
        mapping_failure = native.index("if (copy == NULL)")
        mapping_release = native.index("__pspgl_buffer_free(vbuf);", mapping_failure)
        mapping_return = native.index("return 0;", mapping_release)
        self.assertLess(allocation_failure, map_buffer)
        self.assertLess(mapping_failure, copy_vertices)
        self.assertLess(mapping_release, mapping_return)
        self.assertLess(mapping_return, emit)
        self.assertNotIn("return 0;", native[emit:])

    def test_backend_prepares_pspgl_state_before_private_submit(self) -> None:
        submit = body(D3D, "bool DrawPspSpritePairs(")
        prepare = submit.index("PrepareState(true)")
        convert = submit.index("TransformPosition", prepare)
        native = submit.index("__pspgl_th08_draw_native_sprite_pairs_copy")
        native_success = submit.index("return true;", native)
        public_client_state = submit.index(
            "glEnableClientState(GL_VERTEX_ARRAY)", native_success
        )
        public_draw = submit.index("glDrawArrays(GL_SPRITES_PSP", public_client_state)

        self.assertLess(prepare, convert)
        self.assertLess(convert, native)
        self.assertLess(native, native_success)
        self.assertLess(native_success, public_client_state)
        self.assertLess(public_client_state, public_draw)
        self.assertIn("fvf != (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)", submit)
        self.assertIn("texture == NULL", submit)
        self.assertIn("vertexCount > pspDrawVertexCapacity", submit)

    def test_score_backend_returns_on_native_success_and_keeps_client_fallback(self) -> None:
        submit = body(D3D, "bool DrawPspSpritePairs(")
        self.assertIn("allowScorePopupNativeGe", submit)
        native = submit.index("__pspgl_th08_draw_native_sprite_pairs_copy")
        native_return = submit.index("return true;", native)
        client = submit.index("glEnableClientState(GL_VERTEX_ARRAY)", native_return)
        public_draw = submit.index("glDrawArrays(GL_SPRITES_PSP", client)
        self.assertLess(native, native_return)
        self.assertLess(native_return, client)
        self.assertLess(client, public_draw)
        for counter in (
            "pspScorePopupNativeAttempts",
            "pspScorePopupNativeSubmits",
            "pspScorePopupNativeClientFallbacks",
        ):
            self.assertIn(counter, D3D)
        self.assertIn("converted pair stream immediately takes", submit)

    def test_m0_scope_excludes_time_popups_item_time_and_delayed_arena(self) -> None:
        fast = body(ANM, "ZunResult AnmManager::DrawPspAsciiPopupBatch")
        self.assertIn("popups != g_AsciiManager.scorePopups", fast)
        self.assertIn("score_only=1", D3D)
        self.assertIn("time_popups=0 item_time=0", D3D)
        self.assertIn("Neither `timePopups` nor Item's `ITEM_TIME`", README)
        self.assertIn("delayed_arena=0", D3D)
        native = body(
            PATCH,
            "int __pspgl_th08_draw_native_sprite_pairs_copy(",
            last=True,
        )
        self.assertNotIn("RenderResourceArena", native)
        self.assertNotIn("sceGuGetMemory", native)

    def test_read_only_device_lifetime_stats_query_is_exported_and_bounded(self) -> None:
        for token in (
            "struct PspScorePopupNativeGeStats",
            "unsigned long attempts;",
            "unsigned long submits;",
            "unsigned long clientFallbacks;",
            "th08_psp_query_score_popup_native_ge_stats",
        ):
            self.assertIn(token, INTERNAL)

        method = body(D3D, "bool QueryPspScorePopupNativeGeStats(")
        self.assertIn("PspScorePopupNativeGeStats *stats) const", method)
        self.assertIn("stats->attempts = pspScorePopupNativeAttempts;", method)
        self.assertIn("stats->submits = pspScorePopupNativeSubmits;", method)
        self.assertIn(
            "stats->clientFallbacks = pspScorePopupNativeClientFallbacks;",
            method,
        )
        for forbidden in (
            "++",
            "--",
            "memset",
            "PrepareState",
            "gl",
            "sceGu",
            "AppendLine",
        ):
            self.assertNotIn(forbidden, method)

        exported = body(
            D3D, "bool th08_psp_query_score_popup_native_ge_stats("
        )
        self.assertIn("deviceRaw == NULL || stats == NULL", exported)
        self.assertIn("->QueryPspScorePopupNativeGeStats(stats)", exported)

    def test_memory_marks_and_samples_flush_cumulative_stats_before_snapshot(self) -> None:
        snapshot = body(MEMORY, "void LogSnapshot(")
        native_record = snapshot.index("SCORE_POPUP_NATIVE_GE_TELEMETRY kind=%s")
        general_record = snapshot.index('"%s phase=%s frame=%lu')
        self.assertLess(native_record, general_record)
        for token in (
            "th08_psp_query_score_popup_native_ge_stats(",
            "th08::g_Supervisor.d3dDevice",
            "valid=%d",
            "counter_scope=device_lifetime",
            "cumulative=1",
            "attempts=%lu",
            "submits=%lu",
            "client_fallbacks=%lu",
        ):
            self.assertIn(token, snapshot)
        self.assertIn("SCORE_POPUP_NATIVE_GE_TELEMETRY_POLICY", MEMORY)
        self.assertIn("mark_is_non_destructive=1", MEMORY)
        self.assertIn("record_precedes_general_snapshot=1", MEMORY)

        # Undefined/zero feature builds preprocess every new declaration,
        # device field/query, and telemetry call/record away.
        self.assertRegex(
            INTERNAL,
            re.compile(
                r"#if defined\(TH08_PSP_SCORE_POPUP_NATIVE_GE\).*?"
                r"struct PspScorePopupNativeGeStats.*?"
                r"th08_psp_query_score_popup_native_ge_stats.*?#endif",
                re.DOTALL,
            ),
        )
        # The internal renderer declaration is shared with the independently
        # gated Bullet/Item mixed-backend telemetry.  The score feature still
        # has two private guards (record and policy), while the include itself
        # is one OR guard that preprocesses away when every consumer is off.
        self.assertGreaterEqual(
            MEMORY.count(
                "#if defined(TH08_PSP_SCORE_POPUP_NATIVE_GE) && \\\n    TH08_PSP_SCORE_POPUP_NATIVE_GE"
            ),
            2,
        )
        include_guard = MEMORY[: MEMORY.index("#include <malloc.h>")]
        for token in (
            "TH08_PSP_SCORE_POPUP_NATIVE_GE",
            "TH08_PSP_BULLET_MIXED_QUADS_FASTPATH",
            "TH08_PSP_ITEM_MIXED_QUADS_FASTPATH",
            '#include "modern/linux/d3d8_internal.hpp"',
        ):
            self.assertIn(token, include_guard)

    def test_makefile_link_gate_is_specific_to_native_symbol(self) -> None:
        self.assertRegex(
            MAKEFILE,
            re.compile(
                r'test "\$\(TH08_PSP_SCORE_POPUP_NATIVE_GE\)" = "1"; then.*?'
                r"__pspgl_th08_draw_native_sprite_pairs_copy",
                re.DOTALL,
            ),
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
