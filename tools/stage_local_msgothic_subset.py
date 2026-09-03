#!/usr/bin/env python3
"""Validate and safely stage the user-local TH08 MS Gothic subset.

This tool never builds or packages the font.  It accepts only the local output
of ``build_local_msgothic_subset.py``, checks it against the tracked 1,531
codepoint authority, and copies it beside an identified TH08 personal EBOOT.
Dry-run is the default. Every apply mode fails closed while any Windows or
native PPSSPP process is running, with a second process check immediately
before target replacement.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import io
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Sequence


REPO_ROOT = Path(__file__).resolve().parents[1]
AUTHORITY_HEADER = REPO_ROOT / "src" / "Th08FontCoverage.hpp"
EXPECTED_POLICY = "th08-psp-msgothic-subset-v1"
EXPECTED_CODEPOINT_COUNT = 1531
FONT_FILENAME = "msgothic-subset.ttf"
MANIFEST_SUFFIX = ".manifest.json"
COVERAGE_SUFFIX = ".coverage.txt"
ALLOWED_EBOOT_TITLES = frozenset({"Touhou 8 PSP SC Engine Bring-up"})


class StageError(RuntimeError):
    """Expected validation or deployment refusal."""


@dataclass(frozen=True)
class Authority:
    codepoints: frozenset[int]
    profile_sha256: str


@dataclass(frozen=True)
class ValidatedSubset:
    font: Path
    manifest: Path
    coverage_report: Path
    bytes: int
    sha256: str
    codepoint_count: int
    profile_sha256: str


@dataclass(frozen=True)
class StageResult:
    destination: Path
    applied: bool
    unchanged: bool
    backup: Path | None
    sha256: str
    bytes: int


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def stable_codepoint_hash(codepoints: frozenset[int]) -> str:
    payload = "".join(f"U+{value:04X}\n" for value in sorted(codepoints))
    return hashlib.sha256(payload.encode("ascii")).hexdigest()


def _canonical_path_and_existing_anchor(path: Path) -> tuple[Path, Path]:
    absolute = Path(os.path.abspath(os.fspath(path.expanduser())))
    current = absolute
    tail: list[str] = []
    while not os.path.lexists(current):
        parent = current.parent
        if parent == current:
            break
        tail.append(current.name)
        current = parent
    anchor = current.resolve(strict=True)
    return anchor.joinpath(*reversed(tail)), anchor


def _drvfs_casefold_parts(path: Path) -> tuple[str, ...]:
    normalized = os.path.normpath(os.fspath(path)).replace("\\", "/")
    return tuple(part.casefold() for part in normalized.split("/") if part)


def is_within(path: Path, directory: Path) -> bool:
    try:
        canonical, existing_anchor = _canonical_path_and_existing_anchor(path)
        base = directory.resolve(strict=True)
        cursor = existing_anchor
        while True:
            if os.path.samefile(cursor, base):
                return True
            parent = cursor.parent
            if parent == cursor:
                break
            cursor = parent
        try:
            canonical.relative_to(base)
            return True
        except ValueError:
            pass
        base_text = str(base).replace("\\", "/")
        if re.match(r"^/mnt/[A-Za-z](?:/|$)", base_text) or re.match(
            r"^[A-Za-z]:/", base_text
        ):
            candidate_parts = _drvfs_casefold_parts(canonical)
            base_parts = _drvfs_casefold_parts(base)
            return candidate_parts[: len(base_parts)] == base_parts
        return False
    except OSError:
        return True


def is_same_or_within_allowed_root(path: Path, directory: Path) -> bool:
    """Prove that *path* resolves to *directory* or one of its descendants.

    Unlike the deny-oriented ``is_within`` helper, uncertainty fails closed by
    returning False. Resolving the deepest existing ancestor prevents a
    symlink below the approved private root from redirecting backups into a
    PPSSPP memstick (or any other external tree).
    """

    try:
        canonical, _ = _canonical_path_and_existing_anchor(path)
        base, _ = _canonical_path_and_existing_anchor(directory)
        try:
            canonical.relative_to(base)
            return True
        except ValueError:
            pass
        base_text = str(base).replace("\\", "/")
        if re.match(r"^/mnt/[A-Za-z](?:/|$)", base_text) or re.match(
            r"^[A-Za-z]:/", base_text
        ):
            candidate_parts = _drvfs_casefold_parts(canonical)
            base_parts = _drvfs_casefold_parts(base)
            return candidate_parts[: len(base_parts)] == base_parts
        return False
    except OSError:
        return False


def load_authority(path: Path = AUTHORITY_HEADER) -> Authority:
    source = path.read_text(encoding="utf-8")
    array = re.search(
        r"kTh08PspStockFontCodepoints\[\]\s*=\s*\{(?P<body>.*?)\};",
        source,
        re.DOTALL,
    )
    count = re.search(
        r"kTh08PspStockFontCodepointCount\s*=\s*(\d+)u;", source
    )
    profile = re.search(
        r"kTh08PspStockFontProfileSha256\[\]\s*=\s*\n?\s*"
        r'"([0-9a-f]{64})";',
        source,
    )
    if array is None or count is None or profile is None:
        raise StageError(f"cannot parse font authority header: {path}")
    codepoints = frozenset(
        int(value, 16)
        for value in re.findall(r"0x([0-9A-Fa-f]+)u", array.group("body"))
    )
    declared_count = int(count.group(1))
    if declared_count != EXPECTED_CODEPOINT_COUNT or len(codepoints) != declared_count:
        raise StageError(
            f"font authority count mismatch: declared={declared_count} "
            f"unique={len(codepoints)} expected={EXPECTED_CODEPOINT_COUNT}"
        )
    computed_profile = stable_codepoint_hash(codepoints)
    if computed_profile != profile.group(1):
        raise StageError(
            "font authority profile hash mismatch: "
            f"header={profile.group(1)} computed={computed_profile}"
        )
    return Authority(codepoints, profile.group(1))


def _require_fonttools() -> Any:
    try:
        from fontTools.ttLib import TTFont
    except ModuleNotFoundError as exc:
        raise StageError(
            "fontTools is required to validate the staged subset; install it with: "
            f"{sys.executable} -m pip install --user fonttools"
        ) from exc
    return TTFont


def _manifest_int(mapping: dict[str, Any], key: str) -> int:
    value = mapping.get(key)
    if type(value) is not int:
        raise StageError(f"manifest field {key} is not an integer")
    return value


def validate_subset(
    font_path: Path,
    manifest_path: Path | None = None,
    coverage_path: Path | None = None,
) -> ValidatedSubset:
    font_path = font_path.resolve(strict=True)
    if font_path.name != FONT_FILENAME:
        raise StageError(f"local subset must be named {FONT_FILENAME}")
    if is_within(font_path, REPO_ROOT):
        raise StageError("refusing a proprietary derived font stored inside the repository")

    manifest_path = (
        manifest_path or font_path.with_name(font_path.name + MANIFEST_SUFFIX)
    ).resolve(strict=True)
    coverage_path = (
        coverage_path or font_path.with_name(font_path.name + COVERAGE_SUFFIX)
    ).resolve(strict=True)
    if is_within(manifest_path, REPO_ROOT) or is_within(coverage_path, REPO_ROOT):
        raise StageError("local font reports must remain outside the repository")

    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise StageError(f"cannot read subset manifest: {exc}") from exc
    if not isinstance(manifest, dict):
        raise StageError("subset manifest root is not an object")
    if manifest.get("policy_version") != EXPECTED_POLICY:
        raise StageError(
            f"unexpected subset policy: {manifest.get('policy_version')!r}"
        )

    output = manifest.get("output")
    coverage = manifest.get("coverage")
    stock = manifest.get("stock_profile")
    validation = manifest.get("validation")
    if not all(isinstance(value, dict) for value in (output, coverage, stock, validation)):
        raise StageError("subset manifest is missing output/coverage/profile/validation")
    assert isinstance(output, dict)
    assert isinstance(coverage, dict)
    assert isinstance(stock, dict)
    assert isinstance(validation, dict)

    authority = load_authority()
    actual_bytes = font_path.stat().st_size
    actual_sha256 = sha256_file(font_path)
    if output.get("filename") != FONT_FILENAME:
        raise StageError("manifest output filename is not the runtime subset filename")
    if _manifest_int(output, "bytes") != actual_bytes:
        raise StageError("subset byte count does not match its manifest")
    if output.get("sha256") != actual_sha256:
        raise StageError("subset SHA-256 does not match its manifest")

    expected_coverage = {
        "requested_count": EXPECTED_CODEPOINT_COUNT,
        "source_present_count": EXPECTED_CODEPOINT_COUNT,
        "source_missing_count": 0,
        "output_present_count": EXPECTED_CODEPOINT_COUNT,
        "output_missing_count": 0,
        "name_entry_charset_count": 94,
    }
    for key, expected in expected_coverage.items():
        actual = _manifest_int(coverage, key)
        if actual != expected:
            raise StageError(
                f"subset coverage field {key}={actual}, expected {expected}"
            )
    if stock.get("authority_header_match") is not True:
        raise StageError("subset manifest did not pass its authority-header gate")
    if stock.get("codepoint_sha256") != authority.profile_sha256:
        raise StageError("subset manifest profile differs from the tracked authority")

    structural = validation.get("structural")
    raster = validation.get("raster")
    if not isinstance(structural, dict) or structural.get("status") != "passed":
        raise StageError("subset structural validation was not passed")
    if (
        not isinstance(raster, dict)
        or raster.get("status") != "passed"
        or raster.get("pixel_and_metrics_exact") is not True
    ):
        raise StageError("subset pixel/metric validation was not passed")

    coverage_text = coverage_path.read_text(encoding="utf-8")
    for marker in (
        "output_missing: 0",
        f"requested: {EXPECTED_CODEPOINT_COUNT}",
        f"stock_profile_count: {EXPECTED_CODEPOINT_COUNT}",
        f"output_sha256: {actual_sha256}",
    ):
        if marker not in coverage_text:
            raise StageError(f"coverage report is stale or incomplete: missing {marker!r}")

    TTFont = _require_fonttools()
    try:
        font = TTFont(font_path, lazy=False, recalcTimestamp=False)
        cmap = font.getBestCmap() or {}
        missing = sorted(authority.codepoints.difference(cmap))
        font.close()
    except Exception as exc:
        raise StageError(f"cannot inspect subset cmap: {exc}") from exc
    if missing:
        raise StageError(
            f"subset lacks {len(missing)} authority glyphs; first=U+{missing[0]:04X}"
        )

    return ValidatedSubset(
        font=font_path,
        manifest=manifest_path,
        coverage_report=coverage_path,
        bytes=actual_bytes,
        sha256=actual_sha256,
        codepoint_count=len(authority.codepoints),
        profile_sha256=authority.profile_sha256,
    )


def _parse_param_sfo(payload: bytes) -> dict[str, str | int]:
    if len(payload) < 20:
        raise StageError("EBOOT PARAM.SFO is truncated")
    magic, _version, key_start, data_start, entry_count = struct.unpack_from(
        "<4s4I", payload, 0
    )
    if magic != b"\x00PSF":
        raise StageError("EBOOT PARAM.SFO magic is invalid")
    if entry_count > 256 or 20 + entry_count * 16 > len(payload):
        raise StageError("EBOOT PARAM.SFO entry table is invalid")
    entry_table_end = 20 + entry_count * 16
    if (
        key_start < entry_table_end
        or key_start >= len(payload)
        or data_start < key_start
        or data_start > len(payload)
    ):
        raise StageError("EBOOT PARAM.SFO key/data offsets are invalid")

    values: dict[str, str | int] = {}
    for index in range(entry_count):
        key_offset, value_format, value_length, value_capacity, value_offset = (
            struct.unpack_from("<HHIII", payload, 20 + index * 16)
        )
        key_position = key_start + key_offset
        if key_position >= len(payload):
            raise StageError("EBOOT PARAM.SFO key points outside the section")
        key_end = payload.find(b"\x00", key_position, data_start)
        if key_end < 0:
            raise StageError("EBOOT PARAM.SFO key is unterminated")
        try:
            key = payload[key_position:key_end].decode("ascii")
        except UnicodeDecodeError as exc:
            raise StageError("EBOOT PARAM.SFO key is not ASCII") from exc
        value_position = data_start + value_offset
        if (
            value_length > value_capacity
            or value_position > len(payload)
            or value_length > len(payload) - value_position
        ):
            raise StageError(f"EBOOT PARAM.SFO value for {key!r} is invalid")
        raw = payload[value_position : value_position + value_length]
        if value_format == 0x0404:
            if len(raw) != 4:
                raise StageError(f"EBOOT PARAM.SFO integer {key!r} has wrong size")
            values[key] = struct.unpack("<I", raw)[0]
        elif value_format in (0x0004, 0x0204):
            try:
                values[key] = raw.rstrip(b"\x00").decode("utf-8")
            except UnicodeDecodeError as exc:
                raise StageError(f"EBOOT PARAM.SFO string {key!r} is not UTF-8") from exc
    return values


def validate_th08_eboot(path: Path) -> str:
    try:
        file_bytes = path.stat().st_size
        with path.open("rb") as stream:
            header = stream.read(40)
            if len(header) != 40:
                raise StageError("runtime EBOOT.PBP is truncated")
            unpacked = struct.unpack("<4s9I", header)
            if unpacked[0] != b"\x00PBP":
                raise StageError("runtime EBOOT.PBP magic is invalid")
            offsets = tuple(unpacked[2:])
            if (
                offsets[0] < 40
                or tuple(sorted(offsets)) != offsets
                or offsets[-1] > file_bytes
                or offsets[1] <= offsets[0]
                or offsets[1] - offsets[0] > 1024 * 1024
            ):
                raise StageError("runtime EBOOT.PBP section offsets are invalid")
            stream.seek(offsets[0])
            param_sfo = stream.read(offsets[1] - offsets[0])
    except OSError as exc:
        raise StageError(f"cannot inspect runtime EBOOT.PBP: {exc}") from exc

    values = _parse_param_sfo(param_sfo)
    title = values.get("TITLE")
    if not isinstance(title, str) or not title.strip():
        raise StageError("runtime EBOOT PARAM.SFO has no TITLE")
    if title not in ALLOWED_EBOOT_TITLES:
        allowed = ", ".join(repr(value) for value in sorted(ALLOWED_EBOOT_TITLES))
        raise StageError(
            f"runtime EBOOT title is not an approved TH08 PSP build: {title!r}; "
            f"expected exactly {allowed}"
        )
    return title


def is_ppsspp_process_name(name: str) -> bool:
    return "ppsspp" in Path(name.strip().strip('"')).name.casefold()


def _list_windows_ppsspp_processes() -> tuple[tuple[str, ...], bool, str]:
    """Return (matches, tasklist_succeeded, last_error)."""

    commands = (
        ("tasklist.exe", "/fo", "csv", "/nh"),
        ("cmd.exe", "/c", "tasklist", "/fo", "csv", "/nh"),
    )
    last_error = "tasklist command unavailable"
    for command in commands:
        try:
            result = subprocess.run(
                command,
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                timeout=10,
            )
        except (OSError, subprocess.SubprocessError) as exc:
            last_error = str(exc)
            continue
        if result.returncode != 0:
            last_error = result.stderr.strip() or f"exit {result.returncode}"
            continue
        active: list[str] = []
        for row in csv.reader(io.StringIO(result.stdout)):
            if len(row) < 2:
                continue
            name = row[0].strip()
            if is_ppsspp_process_name(name):
                active.append(f"{name}:{row[1].strip()}")
        return tuple(sorted(active)), True, ""
    return (), False, last_error


def _list_linux_ppsspp_processes(proc_root: Path = Path("/proc")) -> tuple[str, ...]:
    if not proc_root.is_dir():
        return ()
    active: set[str] = set()
    for process in proc_root.iterdir():
        if not process.name.isdigit():
            continue
        names: set[str] = set()
        try:
            names.add((process / "comm").read_text(encoding="utf-8").strip())
        except OSError:
            pass
        try:
            names.add((process / "exe").resolve(strict=True).name)
        except OSError:
            pass
        for name in names:
            if name and is_ppsspp_process_name(name):
                active.add(f"{name}:{process.name}")
    return tuple(sorted(active))


def list_active_ppsspp_processes() -> tuple[str, ...]:
    """Find Windows and native Qt/SDL PPSSPP processes, failing closed on WSL."""

    windows, windows_ok, windows_error = _list_windows_ppsspp_processes()
    windows_probe_required = (
        os.name == "nt"
        or "WSL_INTEROP" in os.environ
        or Path("/mnt/c/Windows/System32").is_dir()
    )
    if windows_probe_required and not windows_ok:
        raise StageError(f"cannot verify Windows PPSSPP process state: {windows_error}")
    linux = _list_linux_ppsspp_processes()
    if not windows_ok and not Path("/proc").is_dir():
        raise StageError("no supported PPSSPP process probe is available")
    return tuple(sorted(set(windows).union(linux)))


def ensure_ppsspp_stopped(process_probe: Callable[[], tuple[str, ...]]) -> None:
    processes = process_probe()
    if processes:
        raise StageError(
            "refusing runtime deployment while PPSSPP is running: "
            + ", ".join(processes)
        )


def is_obvious_ppsspp_path(destination: Path) -> bool:
    parts = tuple(part.casefold() for part in destination.parts)
    return "memstick" in parts or any("ppsspp" in part for part in parts)


def infer_runtime_kind(destination: Path) -> str:
    return "ppsspp" if is_obvious_ppsspp_path(destination) else "hardware"


def _atomic_copy(
    source: Path,
    destination: Path,
    *,
    before_replace: Callable[[], None] | None = None,
) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="wb",
            prefix=f".{destination.name}.",
            suffix=".tmp",
            dir=destination.parent,
            delete=False,
        ) as stream:
            temporary = Path(stream.name)
            with source.open("rb") as source_stream:
                shutil.copyfileobj(source_stream, stream, length=1024 * 1024)
            stream.flush()
            os.fsync(stream.fileno())
        if before_replace is not None:
            before_replace()
        os.replace(temporary, destination)
        temporary = None
        try:
            directory_fd = os.open(destination.parent, os.O_RDONLY | os.O_DIRECTORY)
        except (AttributeError, OSError):
            directory_fd = -1
        if directory_fd >= 0:
            try:
                os.fsync(directory_fd)
            finally:
                os.close(directory_fd)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def stage_subset(
    subset: ValidatedSubset,
    destination_directory: Path,
    *,
    apply: bool,
    runtime_kind: str,
    backup_directory: Path | None = None,
    process_probe: Callable[[], tuple[str, ...]] | None = None,
) -> StageResult:
    requested_destination = Path(
        os.path.abspath(os.fspath(destination_directory.expanduser()))
    )
    destination_directory = destination_directory.resolve(strict=True)
    if not destination_directory.is_dir():
        raise StageError(f"runtime destination is not a directory: {destination_directory}")
    if is_within(destination_directory, REPO_ROOT):
        raise StageError("personal Microsoft-derived fonts must not be staged inside the repo")
    eboot = destination_directory / "EBOOT.PBP"
    if not eboot.is_file():
        raise StageError("runtime destination has no EBOOT.PBP")
    validate_th08_eboot(eboot)
    obvious_ppsspp = is_obvious_ppsspp_path(
        requested_destination
    ) or is_obvious_ppsspp_path(destination_directory)
    if runtime_kind == "hardware" and obvious_ppsspp:
        raise StageError("an obvious PPSSPP/memstick path cannot be declared hardware")
    if runtime_kind == "auto":
        runtime_kind = infer_runtime_kind(destination_directory)
    if runtime_kind not in {"ppsspp", "hardware"}:
        raise StageError(f"invalid runtime kind: {runtime_kind}")

    target = destination_directory / FONT_FILENAME
    existing_sha = sha256_file(target) if target.is_file() else None
    unchanged = existing_sha == subset.sha256

    selected_backup_directory = backup_directory or default_private_backup_root()
    selected_backup_directory = Path(
        os.path.abspath(os.fspath(selected_backup_directory.expanduser()))
    )
    if is_within(selected_backup_directory, REPO_ROOT):
        raise StageError("backup directory must remain outside the repository")
    if is_within(selected_backup_directory, destination_directory):
        raise StageError("backup directory must not be inside the runtime tree")
    try:
        canonical_backup, _ = _canonical_path_and_existing_anchor(
            selected_backup_directory
        )
    except OSError as exc:
        raise StageError(f"cannot prove backup directory location: {exc}") from exc
    if is_obvious_ppsspp_path(
        selected_backup_directory
    ) or is_obvious_ppsspp_path(canonical_backup):
        raise StageError("backup directory must not be in any PPSSPP/memstick tree")
    approved_backup_root = default_private_backup_root()
    # The fixed layout is <local-app-data>/TH08PSP/backups. Prove that neither
    # TH08PSP nor backups is a symlink escape from the local application-data
    # tree before treating the final component as an allow-list root.
    local_app_data_root = approved_backup_root.parent.parent
    if not is_same_or_within_allowed_root(
        approved_backup_root, local_app_data_root
    ):
        raise StageError("private backup root redirects outside local application data")
    if not is_same_or_within_allowed_root(
        selected_backup_directory, approved_backup_root
    ):
        raise StageError(
            "backup directory must be the private local backup root or one of its "
            f"descendants: {approved_backup_root}"
        )

    if not apply:
        return StageResult(target, False, unchanged, None, subset.sha256, subset.bytes)

    process_probe = process_probe or list_active_ppsspp_processes
    process_guard = lambda: ensure_ppsspp_stopped(process_probe)
    # This guard applies to PSP hardware destinations too: a caller cannot
    # bypass protection by spoofing --runtime-kind or by using an auto/custom
    # path while the emulator still owns files.
    process_guard()
    if unchanged:
        return StageResult(target, True, True, None, subset.sha256, subset.bytes)

    backup: Path | None = None
    if target.is_file():
        selected_backup_directory.mkdir(parents=True, exist_ok=True)
        assert existing_sha is not None
        backup = selected_backup_directory / f"{FONT_FILENAME}.before-{existing_sha[:16]}"
        if backup.exists() and sha256_file(backup) != existing_sha:
            raise StageError(f"backup collision with different contents: {backup}")
        if not backup.exists():
            _atomic_copy(target, backup)
        if sha256_file(backup) != existing_sha:
            raise StageError("backup readback SHA-256 mismatch")

    try:
        _atomic_copy(subset.font, target, before_replace=process_guard)
        if target.stat().st_size != subset.bytes or sha256_file(target) != subset.sha256:
            raise StageError("staged font readback differs from validated source")
    except Exception:
        current_sha = sha256_file(target) if target.is_file() else None
        # A second process guard can abort before os.replace. In that case the
        # old target is already intact; do not create a pointless runtime temp
        # while PPSSPP is known to be running.
        if current_sha == existing_sha:
            raise
        if backup is not None and backup.is_file():
            _atomic_copy(backup, target, before_replace=process_guard)
        elif target.exists():
            process_guard()
            target.unlink()
        raise

    return StageResult(target, True, False, backup, subset.sha256, subset.bytes)


def default_private_root() -> Path:
    local_app_data = os.environ.get("LOCALAPPDATA")
    if local_app_data:
        return Path(local_app_data) / "TH08PSP"
    return (
        Path("/mnt/c/Users")
        / os.environ.get("USER", "NAME")
        / "AppData"
        / "Local"
        / "TH08PSP"
    )


def default_local_subset() -> Path:
    return default_private_root() / FONT_FILENAME


def default_private_backup_root() -> Path:
    return default_private_root() / "backups"


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Validate and stage a personal TH08 MS Gothic subset safely."
    )
    parser.add_argument("--source", type=Path, default=default_local_subset())
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--coverage", type=Path)
    parser.add_argument("--destination", type=Path, required=True)
    parser.add_argument(
        "--runtime-kind", choices=("auto", "ppsspp", "hardware"), default="auto"
    )
    parser.add_argument(
        "--backup-directory",
        type=Path,
        help=(
            "optional subdirectory of the fixed private root "
            "%LOCALAPPDATA%/TH08PSP/backups; runtime/memstick paths are refused"
        ),
    )
    parser.add_argument(
        "--apply",
        action="store_true",
        help="perform the atomic copy; without this flag the command is read-only",
    )
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    subset = validate_subset(args.source, args.manifest, args.coverage)
    result = stage_subset(
        subset,
        args.destination,
        apply=args.apply,
        runtime_kind=args.runtime_kind,
        backup_directory=args.backup_directory,
    )
    action = "unchanged" if result.unchanged else ("staged" if result.applied else "would-stage")
    print(
        f"{action}: {result.destination} bytes={result.bytes} "
        f"coverage={subset.codepoint_count}/{EXPECTED_CODEPOINT_COUNT} "
        f"sha256={result.sha256}"
    )
    if result.backup is not None:
        print(f"backup: {result.backup}")
    if not args.apply:
        print("dry-run only; pass --apply after the target runtime is not in use")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, StageError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(2)
