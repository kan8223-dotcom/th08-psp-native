#!/usr/bin/env python3
"""Host-only source contracts for PSP title-surface return recovery."""

from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    raise AssertionError(f"unterminated function: {signature}")


class PspTitleSurfaceReturnTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.makefile = read("Makefile.psp")
        cls.arena_h = read("psp/render_resource_arena.hpp")
        cls.arena = read("psp/render_resource_arena.cpp")
        cls.decoder = read("src/modern/linux/d3dx8_compat.cpp")
        cls.backend = read("src/modern/linux/d3d8_compat.cpp")
        cls.anm = read("src/AnmManager.cpp")
        cls.load = function_body(
            cls.decoder, "bool th08_linux_surface_load_image_memory"
        )
        cls.load_surface = function_body(
            cls.anm, "ZunResult AnmManager::LoadSurface"
        )
        cls.preload_surface = function_body(
            cls.anm, "ZunResult AnmManager::PreloadSurface"
        )
        cls.blit = function_body(cls.backend, "bool BlitToBackbuffer")

    def test_c_allocator_routing_is_decode_only_and_link_complete(self) -> None:
        for symbol in ("malloc", "calloc", "realloc", "free"):
            self.assertIn(f"-Wl,--wrap={symbol}", self.makefile)
        self.assertIn("class SurfaceDecodeAllocationScope", self.arena_h)
        for wrapper in ("__wrap_malloc", "__wrap_calloc", "__wrap_realloc"):
            body = function_body(self.arena, f'extern "C" void *{wrapper}')
            self.assertIn("SurfaceDecodeAllocationScopeActive()", body)
        # Existing generic renderer scopes must not redirect arbitrary C
        # allocations; only the explicit decoder scope activates the wrappers.
        malloc_body = function_body(self.arena, 'extern "C" void *__wrap_malloc')
        self.assertNotIn("RenderResourceAllocationScopeActive()", malloc_body)
        decode_active = function_body(
            self.arena, "bool SurfaceDecodeAllocationScopeActive"
        )
        depth_check = decode_active.index("gSurfaceDecodeScopeDepth")
        thread_check = decode_active.index("RenderResourceAllocationScopeActive()")
        self.assertLess(depth_check, thread_check)

    def test_decoder_scope_ends_before_surface_gl_and_stdio_work(self) -> None:
        scope = self.load.index("SurfaceDecodeAllocationScope decodeScope")
        image = self.load.index("decoded = IMG_Load_RW(stream, 1)", scope)
        scope_close = self.load.index("\n    }\n#else", image)
        destination = self.load.index("device->CreateImageSurface", scope_close)
        diagnostics = self.load.index("TH08PSP SURFACE_DECODE", destination)
        self.assertLess(scope, image)
        self.assertLess(image, scope_close)
        self.assertLess(scope_close, destination)
        self.assertLess(scope_close, diagnostics)
        self.assertIn("SurfaceDecodeBreadcrumb breadcrumb(size)", self.load)
        self.assertIn('breadcrumb.stage = "image_decode"', self.load)
        self.assertIn('breadcrumb.stage = "complete"', self.load)

    def test_arena_realloc_preserves_the_old_block_on_failure(self) -> None:
        realloc = function_body(
            self.arena, "void *RenderResourceArenaReallocate"
        )
        allocate = realloc.index("void *replacement = RenderResourceArenaAllocate")
        null_check = realloc.index("if (replacement == nullptr)", allocate)
        copy = realloc.index("std::memcpy(replacement", null_check)
        retire = realloc.index("RenderResourceArenaTryFree(memory)", copy)
        self.assertLess(allocate, null_check)
        self.assertLess(null_check, copy)
        self.assertLess(copy, retire)
        calloc = function_body(self.arena, 'extern "C" void *__wrap_calloc')
        self.assertIn("count > static_cast<std::size_t>(-1) / bytes", calloc)
        wrapped_realloc = function_body(
            self.arena, 'extern "C" void *__wrap_realloc'
        )
        self.assertLess(
            wrapped_realloc.index("RenderResourceArenaContains(memory)"),
            wrapped_realloc.index("__real_realloc(memory, bytes)"),
        )
        self.assertIn("RenderResourceArenaReallocate", wrapped_realloc)

    def test_worker_preload_never_destroys_a_live_gl_surface(self) -> None:
        psp_branch = self.preload_surface[
            self.preload_surface.index("#if defined(PSP)") :
            self.preload_surface.index("#else")
        ]
        self.assertNotIn("this->ReleaseSurface(surfaceIdx)", psp_branch)
        self.assertIn("preload=DEFERRED_RELEASE", psp_branch)

        detach = self.load_surface.index(
            "fileData = this->surfaceData[surfaceIdx]"
        )
        clear = self.load_surface.index(
            "this->surfaceData[surfaceIdx] = NULL", detach
        )
        decode = self.load_surface.index(
            "th08_linux_surface_load_image_memory", clear
        )
        rollback = self.load_surface.index(
            "this->surfaceData[surfaceIdx] = fileData", decode
        )
        release = self.load_surface.index("this->ReleaseSurface(surfaceIdx)", rollback)
        commit = self.load_surface.index(
            "this->surfacesBis[surfaceIdx] = decodedSurface", release
        )
        self.assertLess(detach, clear)
        self.assertLess(clear, decode)
        self.assertLess(decode, rollback)
        self.assertLess(rollback, release)
        self.assertLess(release, commit)
        self.assertIn("replace=ROLLED_BACK", self.load_surface)

    def test_product_does_not_dump_surface_sources_to_memory_stick(self) -> None:
        dump = self.load_surface.index("TH08_PSP_SURFACE_SOURCE_DUMP")
        dump_if = self.load_surface.index("if (surfaceIdx == 0 || surfaceIdx == 8)", dump)
        dump_end = self.load_surface.index("#endif", dump_if)
        decode = self.load_surface.index(
            "th08_linux_surface_load_image_memory", dump_end
        )
        self.assertLess(dump, dump_if)
        self.assertLess(dump_if, dump_end)
        self.assertLess(dump_end, decode)
        self.assertNotIn("TH08_PSP_SURFACE_SOURCE_DUMP", self.makefile)

    def test_product_skips_full_surface_pixel_audit_loop(self) -> None:
        audit = self.load.index("TH08_PSP_SURFACE_PIXEL_AUDIT")
        checksum = self.load.index("nonBlackPixels", audit)
        audit_end = self.load.index("#endif", checksum)
        changed = self.load.index("th08_linux_surface_changed", audit_end)
        self.assertLess(audit, checksum)
        self.assertLess(checksum, audit_end)
        self.assertLess(audit_end, changed)
        self.assertNotIn("TH08_PSP_SURFACE_PIXEL_AUDIT", self.makefile)

    def test_cpu_source_is_discarded_only_after_upload_commit(self) -> None:
        upload = self.blit.index("glTexImage2D(GL_TEXTURE_2D")
        error = self.blit.index("const GLenum uploadError", upload)
        reject = self.blit.index("if (uploadError != GL_NO_ERROR)", error)
        discard = self.blit.index("std::vector<BYTE>().swap(pixels)", reject)
        commit = self.blit.index("staticUpload.Finalize", discard)
        self.assertLess(upload, error)
        self.assertLess(error, reject)
        self.assertLess(reject, discard)
        self.assertLess(discard, commit)


if __name__ == "__main__":
    unittest.main(verbosity=2)
