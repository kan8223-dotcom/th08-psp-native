#!/usr/bin/env python3
"""Source-contract tests for the PSP Go-only storage activity lamp."""

from __future__ import annotations

import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SWITCH = "TH08_PSP_GO_IO_LAMP"


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


class PspGoIoLampTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.makefile = read("Makefile.psp")
        cls.header = read("psp/io_activity_lamp.hpp")
        cls.lamp = read("psp/io_activity_lamp.cpp")
        cls.fileio = read("psp/fileio.cpp")
        cls.compat = read("src/modern/linux/linux_compat.cpp")
        cls.runtime = read("src/modern/linux/linux_runtime.cpp")
        cls.game_main = read("src/main.cpp")
        cls.psp_main = read("psp/main.cpp")

    def test_makefile_default_off_stamp_and_exact_closure(self) -> None:
        m = self.makefile
        self.assertIn(f"{SWITCH} ?= 0", m)
        self.assertIn(f"$(error {SWITCH} must be 0 or 1)", m)
        self.assertIn(f"CXXFLAGS += -D{SWITCH}=1", m)
        self.assertIn("PSP_GO_IO_LAMP_SRCS := psp/io_activity_lamp.cpp", m)
        for stamp in ("go-io-lamp-0.stamp", "go-io-lamp-1.stamp"):
            self.assertIn(stamp, m)
        self.assertIn("$(GO_IO_LAMP_CONFIG_STAMPS)", m)
        self.assertIn(
            "@rm -f $(filter-out $@,$(GO_IO_LAMP_CONFIG_STAMPS))", m
        )
        closure = (
            "psp/io_activity_lamp.o psp/fileio.o src/main.o \\\n"
            "\tsrc/modern/linux/linux_compat.o src/modern/linux/linux_runtime.o \\\n"
            "\tpsp/main.o: \\\n\t$(GO_IO_LAMP_CONFIG_STAMP)"
        )
        self.assertIn(closure, m)
        self.assertIn('"GO_IO_LAMP=%d "', self.psp_main)
        self.assertIn("        TH08_PSP_FEATURE_GO_IO_LAMP,", self.psp_main)

    def test_runtime_gate_is_go_only_and_initialized_after_fileio(self) -> None:
        init = function_body(self.lamp, "void IoActivityLampInitialize()")
        self.assertIn("const int model = kuKernelGetModel();", init)
        self.assertIn("model == kPspGoModel ? 1U : 0U", init)
        self.assertIn("constexpr int kPspGoModel = 4;", self.lamp)
        self.assertNotIn("model > 0", self.lamp)

        main = function_body(self.psp_main, "int main(int argc, char **argv)")
        fileio = main.index("th08::psp::FileIoInitialize")
        lamp = main.index("th08::psp::IoActivityLampInitialize")
        self.assertLess(fileio, lamp)

    def test_activity_and_post_stall_latches_are_bounded(self) -> None:
        self.assertIn("kActivityHoldUs = 200U * 1000U", self.lamp)
        self.assertIn("kSlowThresholdUs = 100U * 1000U", self.lamp)
        self.assertIn("kSlowHoldUs = 3U * 1000U * 1000U", self.lamp)
        query = function_body(self.lamp, "IoActivityLampState IoActivityLampQuery()")
        self.assertLess(query.index("gSlowEver"), query.index("gActiveDepth"))
        self.assertIn("now - lastSlow < kSlowHoldUs", query)
        self.assertIn("now - lastActivity < kActivityHoldUs", query)
        self.assertIn('BootLog("IO_LAMP SLOW', self.lamp)
        self.assertNotIn("FlushBootLog", self.lamp)

    def test_core_syscalls_are_bracketed_outside_retry_loops(self) -> None:
        cases = (
            ("HANDLE CreateFileA(", "IoActivityKind::Open", "open("),
            ("BOOL ReadFile(", "IoActivityKind::Read", "while (total < size)"),
            ("BOOL WriteFile(", "IoActivityKind::Write", "while (total < size)"),
            ("DWORD SetFilePointer(", "IoActivityKind::Seek", "lseek("),
            ("DWORD GetFileSize(", "IoActivityKind::Metadata", "fstat("),
            ("BOOL FlushFileBuffers(", "IoActivityKind::Sync", "fsync("),
            ("HANDLE FindFirstFileA(", "IoActivityKind::Directory", "glob("),
        )
        for signature, scope_kind, operation in cases:
            with self.subTest(signature=signature):
                body = function_body(self.compat, signature)
                self.assertEqual(body.count(scope_kind), 1)
                self.assertLess(body.index(scope_kind), body.index(operation))

        fill = function_body(self.compat, "bool FillFindData(")
        self.assertEqual(fill.count("IoActivityKind::Metadata"), 1)
        self.assertLess(fill.index("IoActivityKind::Metadata"), fill.index("stat("))

    def test_archive_request_and_boot_log_reopens_are_visible(self) -> None:
        archive = function_body(self.runtime, "void LogArchiveRequest(const char *path)")
        self.assertEqual(archive.count("IoActivityKind::Write"), 1)
        self.assertLess(archive.index("IoActivityKind::Write"), archive.index("fopen("))
        self.assertLess(archive.index("fopen("), archive.index("fclose("))

        boot = function_body(self.fileio, "bool WriteBootLogChunk()")
        self.assertIn('IoActivityKind::Write, "TH08PSP_BOOT.LOG",\n                               false', boot)
        self.assertLess(boot.index("IoActivityKind::Write"), boot.index("sceIoOpen("))

    def test_overlay_is_last_and_uses_full_viewport(self) -> None:
        draw = function_body(self.game_main, "static void DrawPspGoIoActivityLamp()")
        for field in (
            "viewport.X = 0",
            "viewport.Y = 0",
            "viewport.Width = 640",
            "viewport.Height = 480",
            "viewport.MinZ = 0.0f",
            "viewport.MaxZ = 1.0f",
        ):
            self.assertIn(field, draw)
        self.assertIn("0xffff3030U", draw)
        self.assertIn("0xffffb000U", draw)
        self.assertIn("ScreenEffect::DrawSquare(&rect, color);", draw)

        render = function_body(self.game_main, "RenderResult GameWindow::Render()")
        final = (
            "g_AnmManager->FlushVertexBuffer();\n"
            "#if defined(PSP) && defined(TH08_PSP_GO_IO_LAMP) && TH08_PSP_GO_IO_LAMP\n"
            "            DrawPspGoIoActivityLamp();\n"
            "#endif\n"
            "            g_Supervisor.d3dDevice->SetTexture(0, NULL);\n"
            "            g_Supervisor.d3dDevice->EndScene();"
        )
        self.assertIn(final, render)
        self.assertLess(render.rindex("g_Chain.RunDrawChain();"),
                        render.index("DrawPspGoIoActivityLamp();"))


if __name__ == "__main__":
    unittest.main()
