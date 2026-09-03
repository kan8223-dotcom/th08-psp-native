#!/usr/bin/env python3
"""Content-aware boundary audit for private MS Gothic-derived artifacts."""

from __future__ import annotations

import argparse
import gzip
import hashlib
import io
import re
import struct
import subprocess
import sys
import tarfile
import unicodedata
import zipfile
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO, Iterable, Sequence


REPO_ROOT = Path(__file__).resolve().parents[1]
FONT_EXTENSIONS = (".ttf", ".ttc", ".otf", ".woff", ".woff2")
FONT_MAGIC = (b"\x00\x01\x00\x00", b"OTTO", b"ttcf", b"true", b"typ1", b"wOFF", b"wOF2")
MAX_ARCHIVE_DEPTH = 5
MAX_ARCHIVE_MEMBERS = 4096
MAX_MEMBER_BYTES = 64 * 1024 * 1024
MAX_TOTAL_MEMBER_BYTES = 256 * 1024 * 1024
MAX_TOP_LEVEL_BYTES = 256 * 1024 * 1024
MAX_ZIP_CENTRAL_DIRECTORY_BYTES = 8 * 1024 * 1024
READ_CHUNK_BYTES = 1024 * 1024
SAFE_ZIP_COMPRESSION_TYPES = frozenset(
    {zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED, zipfile.ZIP_BZIP2}
)
KNOWN_MS_GOTHIC_SHA256 = frozenset(
    {
        "4bde3e6392b96910fb59094c6c1a4dbfae18fee78d0bf13dc30616837c4f95db",
        "cd21262fb7a1cf7b8539ecca8fb69563943f28b69fc307f76bde718892fc196c",
        "82c5a071d25da573ce74d86f9350930145a1d611f7d5924c0427a5d3f33109ae",
    }
)
SUSPICIOUS_NAME = re.compile(
    r"(^|/)[^/]*msgothic[^/]*\.(?:ttc|ttf|otf|woff2?)(?:[^/]*)$",
    re.IGNORECASE,
)


class AuditError(RuntimeError):
    pass


def normalize(path: str) -> str:
    return path.replace("\\", "/")


def suspicious_font_names(names: Iterable[str]) -> tuple[str, ...]:
    return tuple(
        sorted(
            {
                normalize(name)
                for name in names
                if SUSPICIOUS_NAME.search(normalize(name))
            }
        )
    )


forbidden_names = suspicious_font_names


def _looks_like_font(name: str, payload: bytes) -> bool:
    lowered = normalize(name).casefold()
    return lowered.endswith(FONT_EXTENSIONS) or payload[:4] in FONT_MAGIC


def _has_font_extension(name: str) -> bool:
    return normalize(name).casefold().endswith(FONT_EXTENSIONS)


def _normalized_font_name(value: str) -> str:
    normalized = unicodedata.normalize("NFKC", value).casefold()
    return "".join(char for char in normalized if char.isalnum() or char in "ゴシック")


def _is_ms_gothic_name(value: str) -> bool:
    compact = _normalized_font_name(value)
    return (
        "msgothic" in compact
        or "msuigothic" in compact
        or "mspgothic" in compact
        or "msゴシック" in compact
        or "msuiゴシック" in compact
        or "mspゴシック" in compact
    )


def _font_name_values(payload: bytes) -> tuple[str, ...]:
    try:
        from fontTools.ttLib import TTCollection, TTFont
    except ModuleNotFoundError as exc:
        raise AuditError(
            "fontTools is required for content-level release auditing; install it with: "
            f"{sys.executable} -m pip install --user fonttools"
        ) from exc

    stream = io.BytesIO(payload)
    fonts = []
    collection = None
    try:
        if payload[:4] == b"ttcf":
            collection = TTCollection(stream, lazy=False)
            fonts = list(collection.fonts)
        else:
            fonts = [TTFont(stream, lazy=False, recalcTimestamp=False)]
        values: set[str] = set()
        for font in fonts:
            if "name" not in font:
                continue
            for record in font["name"].names:
                if record.nameID not in (1, 3, 4, 6, 16, 17):
                    continue
                try:
                    value = record.toUnicode().strip()
                except Exception:
                    continue
                if value:
                    values.add(value)
        return tuple(sorted(values))
    finally:
        if collection is not None:
            collection.close()
        else:
            for font in fonts:
                font.close()


def _font_content_issues(label: str, payload: bytes) -> tuple[str, ...]:
    issues: list[str] = []
    digest = hashlib.sha256(payload).hexdigest()
    if digest in KNOWN_MS_GOTHIC_SHA256:
        issues.append(f"{label}:known-ms-gothic-sha256:{digest}")
    if not _looks_like_font(label, payload):
        return tuple(issues)
    try:
        names = _font_name_values(payload)
    except Exception as exc:
        issues.append(f"{label}:font-content-unreadable:{exc}")
        return tuple(issues)
    offending = tuple(value for value in names if _is_ms_gothic_name(value))
    if offending:
        issues.append(f"{label}:ms-gothic-name-table:{offending[0]}")
    return tuple(issues)


def _is_zip_payload(name: str, payload: bytes) -> bool:
    return payload[:4] in (b"PK\x03\x04", b"PK\x05\x06", b"PK\x07\x08") or normalize(
        name
    ).casefold().endswith(".zip")


def _looks_like_tar_payload(name: str, payload: bytes) -> bool:
    lowered = normalize(name).casefold()
    return (
        lowered.endswith((".tar", ".tar.gz", ".tgz"))
        or payload[:2] == b"\x1f\x8b"
        or (len(payload) >= 262 and payload[257:262] == b"ustar")
    )


def _archive_limit_issue(label: str, reason: str) -> tuple[str, ...]:
    return (f"{label}:archive-limit:{reason}",)


@dataclass
class _ScanBudget:
    """Bound recursive extraction work, not merely each individual archive."""

    members: int = 0
    extracted_bytes: int = 0
    exhausted: bool = False

    def reserve(self, label: str, size: int) -> tuple[str, ...]:
        self.members += 1
        if self.members > MAX_ARCHIVE_MEMBERS:
            self.exhausted = True
            return _archive_limit_issue(label, "recursive-member-count")
        if size < 0 or size > MAX_MEMBER_BYTES:
            return _archive_limit_issue(label, "uncompressed-size")
        if self.extracted_bytes + size > MAX_TOTAL_MEMBER_BYTES:
            self.exhausted = True
            return _archive_limit_issue(label, "recursive-uncompressed-size")
        self.extracted_bytes += size
        return ()

    def reserve_unknown(self, label: str) -> tuple[int, tuple[str, ...]]:
        self.members += 1
        if self.members > MAX_ARCHIVE_MEMBERS:
            self.exhausted = True
            return 0, _archive_limit_issue(label, "recursive-member-count")
        remaining = MAX_TOTAL_MEMBER_BYTES - self.extracted_bytes
        if remaining <= 0:
            self.exhausted = True
            return 0, _archive_limit_issue(label, "recursive-uncompressed-size")
        return min(MAX_MEMBER_BYTES, remaining), ()

    def commit_unknown(self, size: int) -> None:
        self.extracted_bytes += size


def _read_bounded(stream: BinaryIO, limit: int) -> tuple[bytes, bool]:
    """Read at most limit + 1 bytes without asking a decoder for one huge buffer."""

    payload = bytearray()
    while len(payload) <= limit:
        wanted = min(READ_CHUNK_BYTES, limit + 1 - len(payload))
        if wanted <= 0:
            break
        chunk = stream.read(wanted)
        if not chunk:
            break
        payload.extend(chunk)
    if len(payload) > limit:
        return b"", True
    return bytes(payload), False


def _label_issues(label: str) -> list[str]:
    if suspicious_font_names((label,)):
        return [f"{label}:suspicious-ms-gothic-filename"]
    return []


def _zip_preflight(
    stream: BinaryIO, total_size: int, label: str
) -> tuple[str, ...]:
    """Bound ZIP metadata before ZipFile constructs a ZipInfo for every member."""

    eocd_min_size = 22
    max_comment_size = 65535
    tail_size = min(total_size, eocd_min_size + max_comment_size)
    if tail_size < eocd_min_size:
        return (f"{label}:invalid-zip-preflight:missing-eocd",)
    stream.seek(total_size - tail_size)
    tail = stream.read(tail_size)
    if len(tail) != tail_size:
        return (f"{label}:invalid-zip-preflight:short-eocd-read",)

    eocd_pos = len(tail)
    while True:
        eocd_pos = tail.rfind(b"PK\x05\x06", 0, eocd_pos)
        if eocd_pos < 0:
            return (f"{label}:invalid-zip-preflight:missing-eocd",)
        if eocd_pos + eocd_min_size <= len(tail):
            comment_size = struct.unpack_from("<H", tail, eocd_pos + 20)[0]
            if eocd_pos + eocd_min_size + comment_size == len(tail):
                break

    (
        _signature,
        disk_number,
        central_disk,
        entries_on_disk,
        entry_count,
        central_size,
        central_offset,
        _comment_size,
    ) = struct.unpack_from("<4s4H2LH", tail, eocd_pos)
    eocd_offset = total_size - tail_size + eocd_pos
    if disk_number or central_disk or entries_on_disk != entry_count:
        return (f"{label}:invalid-zip-preflight:multi-disk-unsupported",)
    if (
        entry_count == 0xFFFF
        or central_size == 0xFFFFFFFF
        or central_offset == 0xFFFFFFFF
        or (eocd_pos >= 20 and tail[eocd_pos - 20 : eocd_pos - 16] == b"PK\x06\x07")
    ):
        return (f"{label}:invalid-zip-preflight:zip64-unsupported",)
    if entry_count > MAX_ARCHIVE_MEMBERS:
        return _archive_limit_issue(label, "member-count")
    if central_size > MAX_ZIP_CENTRAL_DIRECTORY_BYTES:
        return _archive_limit_issue(label, "zip-central-directory-size")
    if central_offset < 0 or central_offset + central_size != eocd_offset:
        # Reject self-extracting/concatenated and otherwise ambiguous layouts.
        return (f"{label}:invalid-zip-preflight:ambiguous-central-directory",)

    stream.seek(central_offset)
    remaining = central_size
    actual_entries = 0
    while remaining:
        if remaining < 46:
            return (f"{label}:invalid-zip-preflight:truncated-central-header",)
        header = stream.read(46)
        if len(header) != 46 or header[:4] != b"PK\x01\x02":
            return (f"{label}:invalid-zip-preflight:bad-central-header",)
        filename_size, extra_size, comment_size = struct.unpack_from("<3H", header, 28)
        record_size = 46 + filename_size + extra_size + comment_size
        if record_size > remaining:
            return (f"{label}:invalid-zip-preflight:truncated-central-record",)
        stream.seek(record_size - 46, io.SEEK_CUR)
        remaining -= record_size
        actual_entries += 1
        if actual_entries > MAX_ARCHIVE_MEMBERS:
            return _archive_limit_issue(label, "member-count")
    if actual_entries != entry_count:
        return (f"{label}:invalid-zip-preflight:entry-count-mismatch",)
    stream.seek(0)
    return ()


def _scan_zip_archive(
    label: str, archive: zipfile.ZipFile, depth: int, budget: _ScanBudget
) -> tuple[str, ...]:
    issues: list[str] = []
    infos = archive.infolist()
    if len(infos) > MAX_ARCHIVE_MEMBERS:
        return _archive_limit_issue(label, "member-count")

    archive_total = 0
    for info in infos:
        member_label = f"{label}!{normalize(info.filename)}"
        issues.extend(_label_issues(member_label))
        if budget.exhausted:
            break
        if info.is_dir():
            reservation = budget.reserve(member_label, 0)
            issues.extend(reservation)
            continue
        if info.flag_bits & 0x1:
            issues.append(f"{member_label}:encrypted-archive-member")
            continue
        if info.compress_type not in SAFE_ZIP_COMPRESSION_TYPES:
            issues.append(
                f"{member_label}:unsupported-zip-compression:{info.compress_type}"
            )
            continue

        archive_total += info.file_size
        if info.file_size < 0 or info.file_size > MAX_MEMBER_BYTES:
            issues.extend(_archive_limit_issue(member_label, "uncompressed-size"))
            continue
        if archive_total > MAX_TOTAL_MEMBER_BYTES:
            issues.extend(_archive_limit_issue(member_label, "archive-uncompressed-size"))
            break
        reservation = budget.reserve(member_label, info.file_size)
        if reservation:
            issues.extend(reservation)
            if budget.exhausted:
                break
            continue
        try:
            with archive.open(info, "r") as stream:
                member, overflow = _read_bounded(stream, info.file_size)
        except (OSError, RuntimeError, zipfile.BadZipFile, EOFError) as exc:
            issues.append(f"{member_label}:archive-read-error:{exc}")
            continue
        if overflow or len(member) != info.file_size:
            issues.append(f"{member_label}:archive-size-mismatch")
            continue
        issues.extend(_scan_payload(member_label, member, depth + 1, budget))
    return tuple(sorted(set(issues)))


def _scan_tar_archive(
    label: str, archive: tarfile.TarFile, depth: int, budget: _ScanBudget
) -> tuple[str, ...]:
    issues: list[str] = []
    archive_members = 0
    archive_total = 0
    for member_info in archive:
        archive_members += 1
        member_label = f"{label}!{normalize(member_info.name)}"
        issues.extend(_label_issues(member_label))
        if archive_members > MAX_ARCHIVE_MEMBERS:
            issues.extend(_archive_limit_issue(label, "member-count"))
            break
        if budget.exhausted:
            break
        if not member_info.isfile():
            reservation = budget.reserve(member_label, 0)
            issues.extend(reservation)
            continue

        archive_total += member_info.size
        if member_info.size < 0 or member_info.size > MAX_MEMBER_BYTES:
            issues.extend(_archive_limit_issue(member_label, "uncompressed-size"))
            break
        if archive_total > MAX_TOTAL_MEMBER_BYTES:
            issues.extend(_archive_limit_issue(member_label, "archive-uncompressed-size"))
            break
        reservation = budget.reserve(member_label, member_info.size)
        if reservation:
            issues.extend(reservation)
            break
        try:
            stream = archive.extractfile(member_info)
            if stream is None:
                issues.append(f"{member_label}:archive-read-error")
                continue
            with stream:
                member, overflow = _read_bounded(stream, member_info.size)
        except (OSError, EOFError, tarfile.TarError) as exc:
            issues.append(f"{member_label}:archive-read-error:{exc}")
            continue
        if overflow or len(member) != member_info.size:
            issues.append(f"{member_label}:archive-size-mismatch")
            continue
        issues.extend(_scan_payload(member_label, member, depth + 1, budget))
    return tuple(sorted(set(issues)))


def _scan_gzip_stream(
    label: str, stream: BinaryIO, depth: int, budget: _ScanBudget
) -> tuple[str, ...]:
    member_label = f"{label}!<gzip>"
    allowed, reservation = budget.reserve_unknown(member_label)
    if reservation:
        return reservation
    try:
        with gzip.GzipFile(fileobj=stream, mode="rb") as expanded_stream:
            expanded, overflow = _read_bounded(expanded_stream, allowed)
    except (OSError, EOFError, gzip.BadGzipFile) as exc:
        return (f"{label}:invalid-gzip:{exc}",)
    if overflow:
        if allowed < MAX_MEMBER_BYTES:
            budget.exhausted = True
            return _archive_limit_issue(member_label, "recursive-uncompressed-size")
        return _archive_limit_issue(member_label, "gzip-size")
    budget.commit_unknown(len(expanded))
    return _scan_payload(member_label, expanded, depth + 1, budget)


def _scan_payload(
    label: str, payload: bytes, depth: int, budget: _ScanBudget
) -> tuple[str, ...]:
    label = normalize(label)
    issues = _label_issues(label)

    archive_candidate = payload[:4] not in FONT_MAGIC and (
        _is_zip_payload(label, payload)
        or _looks_like_tar_payload(label, payload)
        or normalize(label).casefold().endswith(".gz")
    )
    if not archive_candidate:
        if len(payload) > MAX_MEMBER_BYTES:
            issues.extend(_archive_limit_issue(label, "payload-size"))
            return tuple(sorted(set(issues)))
        issues.extend(_font_content_issues(label, payload))
        return tuple(issues)
    if depth >= MAX_ARCHIVE_DEPTH:
        issues.extend(_archive_limit_issue(label, "nesting-depth"))
        return tuple(issues)
    if _has_font_extension(label):
        issues.append(
            f"{label}:font-content-unreadable:archive payload uses a font filename"
        )

    payload_limit = MAX_TOP_LEVEL_BYTES if depth == 0 else MAX_MEMBER_BYTES
    if len(payload) > payload_limit:
        issues.extend(_archive_limit_issue(label, "archive-payload-size"))
        return tuple(sorted(set(issues)))

    if _is_zip_payload(label, payload):
        stream = io.BytesIO(payload)
        preflight = _zip_preflight(stream, len(payload), label)
        if preflight:
            issues.extend(preflight)
            return tuple(sorted(set(issues)))
        try:
            with zipfile.ZipFile(stream) as archive:
                issues.extend(_scan_zip_archive(label, archive, depth, budget))
        except (OSError, zipfile.BadZipFile) as exc:
            issues.append(f"{label}:invalid-zip:{exc}")
        return tuple(sorted(set(issues)))

    if _looks_like_tar_payload(label, payload):
        is_gzip = payload[:2] == b"\x1f\x8b"
        try:
            mode = "r|gz" if is_gzip else "r|"
            with tarfile.open(fileobj=io.BytesIO(payload), mode=mode) as archive:
                issues.extend(_scan_tar_archive(label, archive, depth, budget))
            return tuple(sorted(set(issues)))
        except (OSError, tarfile.TarError) as exc:
            if not is_gzip:
                issues.append(f"{label}:invalid-tar:{exc}")
                return tuple(sorted(set(issues)))

    if payload[:2] == b"\x1f\x8b":
        issues.extend(_scan_gzip_stream(label, io.BytesIO(payload), depth, budget))
    return tuple(sorted(set(issues)))


def scan_payload(label: str, payload: bytes, depth: int = 0) -> tuple[str, ...]:
    """Audit an already-bounded payload and recursively cap all extraction work."""

    return _scan_payload(label, payload, depth, _ScanBudget())


def _archive_kind(name: str, head: bytes) -> str | None:
    """Classify from bounded header data; genuine font magic wins over suffixes."""

    if head[:4] in FONT_MAGIC:
        return None
    lowered = normalize(name).casefold()
    if head[:4] in (b"PK\x03\x04", b"PK\x05\x06", b"PK\x07\x08") or lowered.endswith(
        ".zip"
    ):
        return "zip"
    if (
        head[:2] == b"\x1f\x8b"
        or lowered.endswith((".tar.gz", ".tgz", ".gz"))
    ):
        return "tar-or-gzip"
    if lowered.endswith(".tar") or (len(head) >= 262 and head[257:262] == b"ustar"):
        return "tar"
    return None


def _scan_file(
    path: Path, label: str, budget: _ScanBudget | None = None
) -> tuple[str, ...]:
    """Scan one top-level file without materialising an archive as a byte string."""

    budget = budget or _ScanBudget()
    label = normalize(label)
    issues = _label_issues(label)
    size = path.stat().st_size
    if size < 0 or size > MAX_TOP_LEVEL_BYTES:
        issues.extend(_archive_limit_issue(label, "top-level-size"))
        return tuple(sorted(set(issues)))

    with path.open("rb") as raw:
        head = raw.read(512)
        raw.seek(0)
        archive_kind = _archive_kind(label, head)
        if archive_kind is not None and _has_font_extension(label):
            issues.append(
                f"{label}:font-content-unreadable:archive payload uses a font filename"
            )
        if archive_kind == "zip":
            preflight = _zip_preflight(raw, size, label)
            if preflight:
                issues.extend(preflight)
                return tuple(sorted(set(issues)))
            try:
                with zipfile.ZipFile(raw) as archive:
                    issues.extend(_scan_zip_archive(label, archive, 0, budget))
            except (OSError, zipfile.BadZipFile) as exc:
                issues.append(f"{label}:invalid-zip:{exc}")
            return tuple(sorted(set(issues)))

        if archive_kind in ("tar", "tar-or-gzip"):
            is_gzip = head[:2] == b"\x1f\x8b"
            if archive_kind == "tar-or-gzip" and not is_gzip:
                issues.append(f"{label}:invalid-gzip:bad-magic")
                return tuple(sorted(set(issues)))
            try:
                mode = "r|gz" if is_gzip else "r|"
                with tarfile.open(fileobj=raw, mode=mode) as archive:
                    issues.extend(_scan_tar_archive(label, archive, 0, budget))
                return tuple(sorted(set(issues)))
            except (OSError, EOFError, tarfile.TarError) as exc:
                if archive_kind == "tar":
                    issues.append(f"{label}:invalid-tar:{exc}")
                    return tuple(sorted(set(issues)))
                raw.seek(0)
                issues.extend(_scan_gzip_stream(label, raw, 0, budget))
                return tuple(sorted(set(issues)))

        if size > MAX_MEMBER_BYTES:
            issues.extend(_archive_limit_issue(label, "top-level-payload-size"))
            return tuple(sorted(set(issues)))
        payload, overflow = _read_bounded(raw, size)
        if overflow or len(payload) != size:
            issues.append(f"{label}:file-size-changed-during-audit")
            return tuple(sorted(set(issues)))
    issues.extend(_font_content_issues(label, payload))
    return tuple(sorted(set(issues)))


def tracked_files(repo: Path = REPO_ROOT) -> tuple[str, ...]:
    result = subprocess.run(
        ("git", "ls-files", "-z"),
        cwd=repo,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if result.returncode != 0:
        raise AuditError(result.stderr.decode("utf-8", errors="replace").strip())
    return tuple(
        value.decode("utf-8", errors="surrogateescape")
        for value in result.stdout.split(b"\0")
        if value
    )


def _tracked_payload(repo: Path, name: str) -> bytes:
    path = repo / name
    if path.is_file():
        size = path.stat().st_size
        if size > MAX_TOP_LEVEL_BYTES:
            raise AuditError(f"tracked file exceeds audit limit: {name} ({size} bytes)")
        with path.open("rb") as stream:
            payload, overflow = _read_bounded(stream, size)
        if overflow or len(payload) != size:
            raise AuditError(f"tracked file changed while auditing: {name}")
        return payload
    if path.is_dir():
        # Gitlinks are audited in their own repository; the tracked entry here
        # contains only a commit id and has no file payload.
        return b""
    size_result = subprocess.run(
        ("git", "cat-file", "-s", f":{name}"),
        cwd=repo,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if size_result.returncode != 0:
        raise AuditError(f"cannot size tracked file {name}: {size_result.stderr!r}")
    try:
        size = int(size_result.stdout.strip())
    except ValueError as exc:
        raise AuditError(f"invalid tracked size for {name}: {size_result.stdout!r}") from exc
    if size < 0 or size > MAX_TOP_LEVEL_BYTES:
        raise AuditError(f"tracked file exceeds audit limit: {name} ({size} bytes)")
    result = subprocess.run(
        ("git", "show", f":{name}"),
        cwd=repo,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if result.returncode != 0:
        raise AuditError(f"cannot read tracked file {name}: {result.stderr!r}")
    if len(result.stdout) != size:
        raise AuditError(
            f"tracked file size mismatch for {name}: expected {size}, got {len(result.stdout)}"
        )
    return result.stdout


def scan_release_path(path: Path) -> tuple[str, ...]:
    if path.is_symlink():
        raise AuditError(f"release path must not be a symlink: {path}")
    if not path.exists():
        raise AuditError(f"release path does not exist: {path}")
    if path.is_dir():
        issues: list[str] = []
        budget = _ScanBudget()
        file_count = 0
        total_size = 0
        for candidate in path.rglob("*"):
            if candidate.is_symlink():
                label = normalize(str(candidate.relative_to(path)))
                issues.append(f"{label}:unsupported-release-symlink")
                continue
            if candidate.is_file():
                label = normalize(str(candidate.relative_to(path)))
                file_count += 1
                if file_count > MAX_ARCHIVE_MEMBERS:
                    issues.extend(_archive_limit_issue(label, "top-level-member-count"))
                    break
                size = candidate.stat().st_size
                total_size += size
                if size < 0 or size > MAX_TOP_LEVEL_BYTES:
                    issues.extend(_archive_limit_issue(label, "top-level-size"))
                    continue
                if total_size > MAX_TOTAL_MEMBER_BYTES:
                    issues.extend(_archive_limit_issue(label, "top-level-total-size"))
                    break
                issues.extend(_scan_file(candidate, label, budget))
                if budget.exhausted:
                    break
        return tuple(sorted(set(issues)))
    if not path.is_file():
        raise AuditError(f"release path is not a regular file or directory: {path}")
    return _scan_file(path, path.name)


def audit(repo: Path, release_paths: Sequence[Path]) -> tuple[str, ...]:
    failures: list[str] = []
    for name in tracked_files(repo):
        label = f"tracked:{normalize(name)}"
        path = repo / name
        if path.is_file() and not path.is_symlink():
            failures.extend(_scan_file(path, label))
        elif path.is_dir():
            # Gitlink; its content is not part of this repository artifact.
            continue
        else:
            failures.extend(scan_payload(label, _tracked_payload(repo, name)))
    for path in release_paths:
        for issue in scan_release_path(path):
            failures.append(f"release:{path}:{issue}")
    return tuple(sorted(set(failures)))


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Content-audit repository/releases for private MS Gothic artifacts."
    )
    parser.add_argument("--repo", type=Path, default=REPO_ROOT)
    parser.add_argument("--release", type=Path, action="append", default=[])
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    releases = list(args.release)
    default_dist = args.repo / "dist"
    if not releases and default_dist.exists():
        releases.append(default_dist)
    failures = audit(args.repo, releases)
    if failures:
        for failure in failures:
            print(f"[FAIL] proprietary local font crossed boundary: {failure}")
        return 1
    print("[OK] no MS Gothic font/subset in tracked or selected release content")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, AuditError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(2)
