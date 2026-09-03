from __future__ import annotations

import hashlib
import importlib.util
import gzip
import io
import os
import struct
import sys
import tarfile
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
STAGE_TOOL = ROOT / "tools" / "stage_local_msgothic_subset.py"
AUDIT_TOOL = ROOT / "tools" / "audit_private_font_boundaries.py"
BUILDER_TOOL = ROOT / "tools" / "build_local_msgothic_subset.py"
OFL_BUILDER_TOOL = ROOT / "tools" / "build_ofl_noto_th08_subset.py"


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


stage = load_module("stage_local_msgothic_subset", STAGE_TOOL)
audit = load_module("audit_private_font_boundaries", AUDIT_TOOL)
builder = load_module("build_local_msgothic_subset_boundary", BUILDER_TOOL)
ofl_builder = load_module("build_ofl_noto_th08_subset_boundary", OFL_BUILDER_TOOL)


def make_param_sfo(title: str) -> bytes:
    key = b"TITLE\x00"
    value = title.encode("utf-8") + b"\x00"
    key_start = 20 + 16
    data_start = (key_start + len(key) + 3) & ~3
    header = struct.pack("<4s4I", b"\x00PSF", 0x00000101, key_start, data_start, 1)
    entry = struct.pack("<HHIII", 0, 0x0204, len(value), len(value), 0)
    return header + entry + key + b"\x00" * (data_start - key_start - len(key)) + value


def write_eboot(
    path: Path, title: str = "Touhou 8 PSP SC Engine Bring-up"
) -> None:
    sfo = make_param_sfo(title)
    param_start = 40
    param_end = param_start + len(sfo)
    offsets = (param_start,) + (param_end,) * 7
    path.write_bytes(struct.pack("<4sI8I", b"\x00PBP", 0x00010000, *offsets) + sfo)


class PrivateFontWorkflowTest(unittest.TestCase):
    def test_runtime_uses_th07_order_and_complete_coverage_gate(self) -> None:
        source = (ROOT / "src" / "modern" / "linux" / "linux_compat.cpp").read_text(
            encoding="utf-8"
        )
        start = source.index("bool InitializePspGdiText()")
        end = source.index("void ShutdownPspGdiText()", start)
        body = source[start:end]
        candidates = (
            body.index('getenv("TH08_FONT")'),
            body.index('ExecutableSiblingPath("msgothic-subset.ttf")'),
            body.index('ExecutableSiblingPath("msgothic.ttc")'),
            body.index('ExecutableSiblingPath("TH08PspSubsetSansJP-Regular.otf")'),
            body.index('ExecutableSiblingPath("NotoSansJP-Regular.ttf")'),
        )
        self.assertEqual(candidates, tuple(sorted(candidates)))
        self.assertIn("CheckPspFontCoverage(font)", source)
        self.assertIn("kExpectedStockFontCodepointCount = 1531u", source)
        self.assertIn("main_ram_copy=0", source)

    def test_real_user_local_subset_passes_hash_cmap_and_quality_gates(self) -> None:
        local = Path("/mnt/c/Users/kan82/AppData/Local/TH08PSP/msgothic-subset.ttf")
        if not local.is_file():
            self.skipTest("user-local TH08 subset is absent")
        result = stage.validate_subset(local)
        self.assertEqual(result.bytes, 753976)
        self.assertEqual(
            result.sha256,
            "82c5a071d25da573ce74d86f9350930145a1d611f7d5924c0427a5d3f33109ae",
        )
        self.assertEqual(result.codepoint_count, 1531)
        self.assertEqual(
            result.profile_sha256,
            "9b3d0d5fa1abbc82086d18788c775d939997235b6d11c2072bdb48254bd58ade",
        )

    def test_repo_boundary_rejects_drvfs_case_variation_and_symlink_ancestor(self) -> None:
        case_variant = Path(str(ROOT).replace("/Users/", "/users/").upper())
        candidate = case_variant / "private" / stage.FONT_FILENAME
        self.assertTrue(stage.is_within(candidate, ROOT))
        self.assertTrue(builder.path_is_within(candidate, ROOT))
        self.assertTrue(ofl_builder.path_is_within(candidate, ROOT))
        with tempfile.TemporaryDirectory() as temporary:
            alias = Path(temporary) / "repo-alias"
            alias.symlink_to(ROOT, target_is_directory=True)
            linked_candidate = alias / "future" / stage.FONT_FILENAME
            self.assertTrue(stage.is_within(linked_candidate, ROOT))
            self.assertTrue(builder.path_is_within(linked_candidate, ROOT))
            self.assertTrue(ofl_builder.path_is_within(linked_candidate, ROOT))
            self.assertFalse(stage.is_within(Path(temporary) / "outside.ttf", ROOT))

    def test_builder_force_cannot_alias_any_input_and_preserves_every_input(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source_font = root / "source-font.ttc"
            stock_archive = root / "stock-th08.dat"
            characters = root / "extra-chars.txt"
            source_font.write_bytes(b"font-authority")
            stock_archive.write_bytes(b"archive-authority")
            characters.write_bytes("追加文字".encode("utf-8"))
            inputs = (source_font, stock_archive, characters)
            before = {path: (path.read_bytes(), builder.sha256_file(path)) for path in inputs}

            for output_index in range(3):
                for protected in inputs:
                    with self.subTest(output_index=output_index, protected=protected.name):
                        outputs = [
                            root / "generated" / "msgothic-subset.ttf",
                            root / "generated" / "font.manifest.json",
                            root / "generated" / "font.coverage.txt",
                        ]
                        outputs[output_index] = protected
                        with self.assertRaisesRegex(
                            builder.SubsetToolError, "aliases an input"
                        ):
                            builder.validate_generated_paths(
                                outputs, force=True, protected_inputs=inputs
                            )

            with self.assertRaisesRegex(builder.SubsetToolError, "aliases an input"):
                builder.build_subset(
                    source_font,
                    [characters],
                    source_font,
                    root / "manifest.json",
                    root / "coverage.txt",
                    stock_archive=stock_archive,
                    force=True,
                    validate_raster=False,
                )
            for path, (payload, digest) in before.items():
                self.assertEqual(path.read_bytes(), payload)
                self.assertEqual(builder.sha256_file(path), digest)

    def test_builder_outputs_reject_exact_casefold_symlink_and_hardlink_aliases(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            first = root / "first.out"
            with self.assertRaisesRegex(builder.SubsetToolError, "must be distinct"):
                builder.validate_generated_paths((first, first, root / "third"), True)

            windows_first = Path(
                "/mnt/c/Users/kan82/AppData/Local/TH08PSP/collision/Report.JSON"
            )
            windows_case_alias = Path(
                "/MNT/C/USERS/KAN82/APPDATA/LOCAL/TH08PSP/COLLISION/report.json"
            )
            self.assertTrue(
                builder.paths_refer_to_same_location(
                    windows_first, windows_case_alias
                )
            )
            with self.assertRaisesRegex(builder.SubsetToolError, "aliases an input"):
                builder.validate_generated_paths(
                    (windows_first, root / "manifest", root / "coverage"),
                    True,
                    protected_inputs=(windows_case_alias,),
                )

            real = root / "real"
            real.mkdir()
            alias = root / "alias"
            alias.symlink_to(real, target_is_directory=True)
            with self.assertRaisesRegex(builder.SubsetToolError, "must be distinct"):
                builder.validate_generated_paths(
                    (real / "report", alias / "report", root / "other"), True
                )
            with self.assertRaisesRegex(builder.SubsetToolError, "aliases an input"):
                builder.validate_generated_paths(
                    (real / "font", root / "manifest", root / "coverage"),
                    True,
                    protected_inputs=(alias / "font",),
                )

            hardlink = root / "hardlink"
            first.write_bytes(b"same inode")
            os.link(first, hardlink)
            self.assertTrue(builder.paths_refer_to_same_location(first, hardlink))
            with self.assertRaisesRegex(builder.SubsetToolError, "aliases an input"):
                builder.validate_generated_paths(
                    (hardlink, root / "manifest-2", root / "coverage-2"),
                    True,
                    protected_inputs=(first,),
                )

    def _fake_subset(self, source: Path, payload: bytes):
        source.write_bytes(payload)
        return stage.ValidatedSubset(
            font=source,
            manifest=source.with_suffix(".manifest.json"),
            coverage_report=source.with_suffix(".coverage.txt"),
            bytes=len(payload),
            sha256=hashlib.sha256(payload).hexdigest(),
            codepoint_count=1531,
            profile_sha256="0" * 64,
        )

    def _runtime(self, root: Path, relative: str = "PSP/GAME/TH08PSP") -> Path:
        runtime = root / relative
        runtime.mkdir(parents=True)
        write_eboot(runtime / "EBOOT.PBP")
        return runtime

    def test_wrong_pbp_magic_and_wrong_title_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            subset = self._fake_subset(root / "local-font.ttf", b"new-font")
            runtime = root / "PSP" / "GAME" / "TH08PSP"
            runtime.mkdir(parents=True)
            (runtime / "EBOOT.PBP").write_bytes(b"not a pbp")
            with self.assertRaisesRegex(stage.StageError, "truncated|magic"):
                stage.stage_subset(
                    subset, runtime, apply=False, runtime_kind="hardware"
                )
            for wrong_title in (
                "Unrelated PSP application",
                "Touhou 8 PSP test",
                "Touhou 80 PSP SC Engine Bring-up",
            ):
                with self.subTest(title=wrong_title):
                    write_eboot(runtime / "EBOOT.PBP", wrong_title)
                    with self.assertRaisesRegex(stage.StageError, "not an approved TH08"):
                        stage.stage_subset(
                            subset, runtime, apply=False, runtime_kind="hardware"
                        )
            self.assertFalse((runtime / stage.FONT_FILENAME).exists())

    def test_exact_eboot_identity_matches_build_contract_without_runtime_access(self) -> None:
        makefile = (ROOT / "Makefile.psp").read_text(encoding="utf-8")
        title = "Touhou 8 PSP SC Engine Bring-up"
        self.assertIn(f"PSP_EBOOT_TITLE := {title}", makefile)
        self.assertEqual(stage.ALLOWED_EBOOT_TITLES, frozenset({title}))
        with tempfile.TemporaryDirectory() as temporary:
            eboot = Path(temporary) / "EBOOT.PBP"
            write_eboot(eboot, title)
            self.assertEqual(stage.validate_th08_eboot(eboot), title)

    def test_all_runtime_kinds_refuse_any_active_ppsspp(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            runtime = self._runtime(root)
            subset = self._fake_subset(root / "local-font.ttf", b"new-font")
            for runtime_kind in ("hardware", "auto", "ppsspp"):
                with self.subTest(runtime_kind=runtime_kind):
                    with self.assertRaisesRegex(stage.StageError, "PPSSPP is running"):
                        stage.stage_subset(
                            subset,
                            runtime,
                            apply=True,
                            runtime_kind=runtime_kind,
                            process_probe=lambda: ("PPSSPPQt:42",),
                        )
                    self.assertFalse((runtime / stage.FONT_FILENAME).exists())

    def test_hardware_spoof_of_obvious_ppsspp_path_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            runtime = self._runtime(
                root, "custom-PPSSPP/memstick/PSP/GAME/TH08PSP"
            )
            subset = self._fake_subset(root / "local-font.ttf", b"font")
            with self.assertRaisesRegex(stage.StageError, "cannot be declared hardware"):
                stage.stage_subset(
                    subset, runtime, apply=False, runtime_kind="hardware"
                )
            plain_runtime = self._runtime(root, "plain/PSP/GAME/TH08PSP")
            disguised = root / "PPSSPP-link"
            disguised.symlink_to(plain_runtime, target_is_directory=True)
            with self.assertRaisesRegex(stage.StageError, "cannot be declared hardware"):
                stage.stage_subset(
                    subset, disguised, apply=False, runtime_kind="hardware"
                )

    def test_second_process_guard_blocks_replace_if_ppsspp_starts_mid_stage(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            runtime = self._runtime(root)
            target = runtime / stage.FONT_FILENAME
            target.write_bytes(b"old-font")
            subset = self._fake_subset(root / "local-font.ttf", b"new-font")
            probe_results = ((), ("PPSSPPSDL:99",))
            probe_calls: list[int] = []

            def process_probe():
                probe_calls.append(len(probe_calls))
                return probe_results[len(probe_calls) - 1]

            private_backups = root / "private-local" / "backups"
            with mock.patch.object(
                stage, "default_private_backup_root", return_value=private_backups
            ):
                with self.assertRaisesRegex(stage.StageError, "PPSSPP is running"):
                    stage.stage_subset(
                        subset,
                        runtime,
                        apply=True,
                        runtime_kind="auto",
                        backup_directory=private_backups,
                        process_probe=process_probe,
                    )
            self.assertEqual(target.read_bytes(), b"old-font")
            self.assertEqual(len(probe_calls), 2)
            self.assertFalse(any(runtime.glob(f".{stage.FONT_FILENAME}.*.tmp")))

    def test_backup_directory_inside_runtime_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            runtime = self._runtime(root)
            subset = self._fake_subset(root / "local-font.ttf", b"font")
            with self.assertRaisesRegex(stage.StageError, "inside the runtime tree"):
                stage.stage_subset(
                    subset,
                    runtime,
                    apply=False,
                    runtime_kind="hardware",
                    backup_directory=runtime / "backups",
                )

    def test_backup_rejects_memstick_root_and_sibling_games(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            runtime = self._runtime(
                root, "custom-PPSSPP/memstick/PSP/GAME/TH08PSP"
            )
            subset = self._fake_subset(root / "local-font.ttf", b"font")
            private_backups = root / "private-local" / "backups"
            rejected = (
                root / "custom-PPSSPP" / "memstick",
                root / "custom-PPSSPP" / "memstick" / "PSP" / "GAME" / "TH07PSP",
            )
            with mock.patch.object(
                stage, "default_private_backup_root", return_value=private_backups
            ):
                for backup in rejected:
                    with self.subTest(backup=backup):
                        with self.assertRaisesRegex(
                            stage.StageError, "PPSSPP/memstick tree"
                        ):
                            stage.stage_subset(
                                subset,
                                runtime,
                                apply=False,
                                runtime_kind="ppsspp",
                                backup_directory=backup,
                            )

                plain_runtime = self._runtime(
                    root, "mounted-stick/PSP/GAME/TH08PSP"
                )
                sibling = root / "mounted-stick" / "PSP" / "GAME" / "TH07PSP"
                with self.assertRaisesRegex(stage.StageError, "private local backup root"):
                    stage.stage_subset(
                        subset,
                        plain_runtime,
                        apply=False,
                        runtime_kind="hardware",
                        backup_directory=sibling,
                    )

    def test_backup_symlink_cannot_escape_private_root(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            runtime = self._runtime(root, "mounted-stick/PSP/GAME/TH08PSP")
            subset = self._fake_subset(root / "local-font.ttf", b"font")
            private_backups = root / "private-local" / "backups"
            private_backups.mkdir(parents=True)
            external = root / "mounted-stick" / "PSP" / "GAME" / "TH07PSP"
            external.mkdir(parents=True)
            escape = private_backups / "escape"
            escape.symlink_to(external, target_is_directory=True)
            with mock.patch.object(
                stage, "default_private_backup_root", return_value=private_backups
            ):
                with self.assertRaisesRegex(stage.StageError, "private local backup root"):
                    stage.stage_subset(
                        subset,
                        runtime,
                        apply=False,
                        runtime_kind="hardware",
                        backup_directory=escape / "nested",
                    )

            local_app_data = root / "local-app-data"
            local_app_data.mkdir()
            redirected_private = local_app_data / "TH08PSP"
            redirected_private.symlink_to(external, target_is_directory=True)
            redirected_backups = redirected_private / "backups"
            with mock.patch.object(
                stage,
                "default_private_backup_root",
                return_value=redirected_backups,
            ):
                with self.assertRaisesRegex(
                    stage.StageError, "redirects outside local application data"
                ):
                    stage.stage_subset(
                        subset,
                        runtime,
                        apply=False,
                        runtime_kind="hardware",
                    )

    def test_hardware_stage_is_atomic_verified_and_keeps_external_backup(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            runtime = self._runtime(root)
            target = runtime / stage.FONT_FILENAME
            target.write_bytes(b"old-font")
            subset = self._fake_subset(root / "local-font.ttf", b"new-font")
            backup_dir = root / "private-local" / "backups"
            with mock.patch.object(
                stage, "default_private_backup_root", return_value=backup_dir
            ):
                result = stage.stage_subset(
                    subset,
                    runtime,
                    apply=True,
                    runtime_kind="hardware",
                    backup_directory=backup_dir,
                    process_probe=lambda: (),
                )
            self.assertEqual(target.read_bytes(), b"new-font")
            self.assertIsNotNone(result.backup)
            assert result.backup is not None
            self.assertEqual(result.backup.read_bytes(), b"old-font")
            self.assertEqual(stage.sha256_file(target), subset.sha256)

    def test_qt_sdl_windows_and_linux_process_names_are_recognized(self) -> None:
        for name in (
            "PPSSPPWindows64.exe",
            "PPSSPPQt.exe",
            "PPSSPPSDL",
            "ppsspp",
            "/usr/bin/PPSSPPHeadless",
        ):
            self.assertTrue(stage.is_ppsspp_process_name(name), name)
        with tempfile.TemporaryDirectory() as temporary:
            proc = Path(temporary)
            process = proc / "123"
            process.mkdir()
            (process / "comm").write_text("PPSSPPQt\n", encoding="utf-8")
            self.assertEqual(
                stage._list_linux_ppsspp_processes(proc), ("PPSSPPQt:123",)
            )

    def test_audit_detects_backup_names_invalid_fonts_and_renamed_content(self) -> None:
        self.assertTrue(
            audit.suspicious_font_names(("backup/msgothic-subset.ttf.before-deadbeef",))
        )
        invalid = audit.scan_payload("public/NotoSansJP-Regular.ttf", b"not-font")
        self.assertTrue(any("font-content-unreadable" in issue for issue in invalid))
        local = Path("/mnt/c/Users/kan82/AppData/Local/TH08PSP/msgothic-subset.ttf")
        if not local.is_file():
            self.skipTest("user-local TH08 subset is absent")
        renamed = audit.scan_payload("assets/innocent.bin", local.read_bytes())
        self.assertTrue(any("known-ms-gothic-sha256" in issue for issue in renamed))
        self.assertTrue(any("ms-gothic-name-table" in issue for issue in renamed))

    def test_audit_descends_tar_gz_and_nested_zip_with_renamed_font(self) -> None:
        local = Path("/mnt/c/Users/kan82/AppData/Local/TH08PSP/msgothic-subset.ttf")
        if not local.is_file():
            self.skipTest("user-local TH08 subset is absent")
        font_payload = local.read_bytes()
        tar_buffer = io.BytesIO()
        with tarfile.open(fileobj=tar_buffer, mode="w:gz") as archive:
            info = tarfile.TarInfo("assets/ui-resource.bin")
            info.size = len(font_payload)
            archive.addfile(info, io.BytesIO(font_payload))
        tar_payload = tar_buffer.getvalue()
        tar_issues = audit.scan_payload("release.tar.gz", tar_payload)
        self.assertTrue(any("known-ms-gothic-sha256" in issue for issue in tar_issues))

        zip_buffer = io.BytesIO()
        with zipfile.ZipFile(zip_buffer, "w") as archive:
            archive.writestr("payload/content.dat", tar_payload)
        nested = audit.scan_payload("outer.zip", zip_buffer.getvalue())
        self.assertTrue(any("known-ms-gothic-sha256" in issue for issue in nested))

    def test_archive_limits_fail_closed_before_unbounded_zip_metadata(self) -> None:
        zip_buffer = io.BytesIO()
        with zipfile.ZipFile(zip_buffer, "w") as archive:
            archive.writestr("one.bin", b"1")
            archive.writestr("two.bin", b"2")
            archive.writestr("three.bin", b"3")
        payload = zip_buffer.getvalue()
        with tempfile.TemporaryDirectory() as temporary:
            package = Path(temporary) / "release.zip"
            package.write_bytes(payload)
            with mock.patch.object(audit, "MAX_ARCHIVE_MEMBERS", 2), mock.patch.object(
                audit.zipfile,
                "ZipFile",
                side_effect=AssertionError("ZipFile must not run before preflight"),
            ):
                issues = audit.scan_release_path(package)
        self.assertTrue(any("archive-limit:member-count" in issue for issue in issues))

        with mock.patch.object(audit, "MAX_ZIP_CENTRAL_DIRECTORY_BYTES", 8):
            issues = audit.scan_payload("release.zip", payload)
        self.assertTrue(
            any("archive-limit:zip-central-directory-size" in issue for issue in issues)
        )

    def test_archive_limits_bound_top_level_gzip_tar_and_recursive_totals(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            oversized = Path(temporary) / "oversized-package.bin"
            oversized.write_bytes(b"x" * 65)
            with mock.patch.object(audit, "MAX_TOP_LEVEL_BYTES", 64):
                issues = audit.scan_release_path(oversized)
            self.assertTrue(any("archive-limit:top-level-size" in issue for issue in issues))

        compressed = gzip.compress(b"x" * 33)
        with mock.patch.object(audit, "MAX_MEMBER_BYTES", 32), mock.patch.object(
            audit, "MAX_TOTAL_MEMBER_BYTES", 128
        ):
            issues = audit.scan_payload("expanded.bin.gz", compressed)
        self.assertTrue(any("archive-limit" in issue and "gzip-size" in issue for issue in issues))

        tar_buffer = io.BytesIO()
        with tarfile.open(fileobj=tar_buffer, mode="w:gz") as archive:
            for index in range(3):
                info = tarfile.TarInfo(f"member-{index}.bin")
                info.size = 1
                archive.addfile(info, io.BytesIO(b"x"))
        with mock.patch.object(audit, "MAX_ARCHIVE_MEMBERS", 2):
            issues = audit.scan_payload("release.tar.gz", tar_buffer.getvalue())
        self.assertTrue(any("archive-limit:member-count" in issue for issue in issues))

        zip_buffer = io.BytesIO()
        with zipfile.ZipFile(zip_buffer, "w") as archive:
            archive.writestr("first.bin", b"123")
            archive.writestr("second.bin", b"456")
        with mock.patch.object(audit, "MAX_TOTAL_MEMBER_BYTES", 5):
            issues = audit.scan_payload("total.zip", zip_buffer.getvalue())
        self.assertTrue(any("uncompressed-size" in issue for issue in issues))

    def test_release_gate_rejects_missing_path_and_archive_disguised_as_font(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with self.assertRaisesRegex(audit.AuditError, "does not exist"):
                audit.scan_release_path(root / "missing-package.zip")

            zip_buffer = io.BytesIO()
            with zipfile.ZipFile(zip_buffer, "w") as archive:
                archive.writestr("ordinary.txt", "clean")
            disguised = root / "NotoSansJP-Regular.ttf"
            disguised.write_bytes(zip_buffer.getvalue())
            issues = audit.scan_release_path(disguised)
            self.assertTrue(any("font-content-unreadable" in issue for issue in issues))

    def test_content_audit_allows_non_ms_gothic_noto_font(self) -> None:
        noto = ROOT / "psp" / "assets" / "NotoSansJP-Regular.ttf"
        if not noto.is_file():
            self.skipTest("Noto source asset is absent")
        self.assertEqual(audit.scan_payload(noto.name, noto.read_bytes()), ())

    def test_release_documentation_requires_exact_artifact_gate(self) -> None:
        workflow = (ROOT / "docs" / "PSP_LOCAL_FONT_WORKFLOW.md").read_text(
            encoding="utf-8"
        )
        generation = workflow.split("## Stage without racing PPSSPP", 1)[0]
        self.assertNotIn("  --force\n", generation)
        self.assertIn(
            "--release /absolute/path/to/final/TH08PSP-package.zip", workflow
        )
        self.assertIn("bare repository audit is not package qualification", workflow)
        self.assertRegex(workflow, r"Do not publish or hand\s+off an artifact")
        self.assertIn("%LOCALAPPDATA%/TH08PSP/backups/", workflow)


if __name__ == "__main__":
    unittest.main()
