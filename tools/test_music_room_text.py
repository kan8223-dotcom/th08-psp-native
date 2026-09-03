#!/usr/bin/env python3
"""Regression gate for the portable TH08 Music Room text path.

The report exposes only byte counts, positions, and hashes.  Original game
text stays inside the user's local archive and is never written to the tree.
"""

from __future__ import annotations

import argparse
import hashlib
import sys
from dataclasses import dataclass
from pathlib import Path

from th08_stock_font_profile import (
    DEFAULT_STOCK_ARCHIVE,
    EXPECTED_ARCHIVE_SHA256,
    PbgzArchive,
    REPO_ROOT,
    StockProfileError,
    sha256_file,
)


EXPECTED_PAYLOAD_BYTES = 7427
EXPECTED_PAYLOAD_SHA256 = (
    "cf3ccfdf98b05daa67b46fa2f6c6e93fbefa46c21570a03b7c02b48c5e4f7797"
)
EXPECTED_TRACKS = 21
EXPECTED_DESCRIPTIONS = 142
EXPECTED_DESCRIPTION_LENGTH_COUNTS = {64: 21, 65: 1, 66: 2, 68: 1}
PORTABLE_DESCRIPTION_CAPACITY = 69


@dataclass(frozen=True)
class Track:
    path: bytes
    title: bytes
    descriptions: tuple[bytes, ...]


def read_line(payload: bytes, cursor: int) -> tuple[bytes, int]:
    start = cursor
    while cursor < len(payload) and payload[cursor] not in (10, 13):
        cursor += 1
    line = payload[start:cursor]
    while cursor < len(payload) and payload[cursor] in (10, 13):
        cursor += 1
    return line, cursor


def parse_tracks(payload: bytes) -> tuple[Track, ...]:
    cursor = 0
    tracks: list[Track] = []
    while cursor < len(payload):
        if payload[cursor] != ord("@"):
            cursor += 1
            continue
        path, cursor = read_line(payload, cursor + 1)
        title, cursor = read_line(payload, cursor)
        descriptions: list[bytes] = []
        for _ in range(7):
            if cursor >= len(payload) or payload[cursor] == ord("@"):
                break
            line, cursor = read_line(payload, cursor)
            descriptions.append(line)
        tracks.append(Track(path, title, tuple(descriptions)))
    return tuple(tracks)


def is_cp932_lead(value: int) -> bool:
    return 0x81 <= value <= 0x9F or 0xE0 <= value <= 0xFC


def is_cp932_trail(value: int) -> bool:
    return 0x40 <= value <= 0x7E or 0x80 <= value <= 0xFC


def bounded_cp932_copy(source: bytes, capacity: int) -> bytes:
    """Mirror CopyMusicRoomCp932Text's complete-character prefix rule."""

    output = bytearray()
    cursor = 0
    while cursor < len(source):
        character_bytes = 1
        if is_cp932_lead(source[cursor]):
            if cursor + 1 >= len(source) or not is_cp932_trail(source[cursor + 1]):
                break
            character_bytes = 2
        if len(output) + character_bytes >= capacity:
            break
        output.extend(source[cursor : cursor + character_bytes])
        cursor += character_bytes
    return bytes(output)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise StockProfileError(message)


def check_source_contract(repo_root: Path) -> None:
    header = (repo_root / "src/MusicRoom.hpp").read_text(encoding="utf-8")
    source = (repo_root / "src/MusicRoom.cpp").read_text(encoding="utf-8")
    compat = (repo_root / "src/modern/linux/linux_compat.cpp").read_text(
        encoding="utf-8"
    )
    require(
        "MUSIC_ROOM_DESCRIPTION_CAPACITY = 69" in header,
        "portable Music Room description capacity is not 68 bytes plus NUL",
    )
    require(
        "ParseMusicRoomComments" in source and "CopyMusicRoomCp932Text" in source,
        "bounded portable Music Room parser/copy path is missing",
    )
    require('0x300000, "%s", buf1' in source, "buf1 is still used as a format string")
    require('0x300000, "%s", buf2' in source, "buf2 is still used as a format string")
    require(
        '0xc0e0ff, 0x302080, "%s"' in source,
        "Music Room title is still used as a format string",
    )
    require(
        compat.count("if (face == NULL && converterMatchesAuthority)") >= 2,
        "an authority-derived subset can bypass the runtime CP932 gate",
    )


def run(archive: Path, repo_root: Path) -> None:
    require(archive.is_file(), f"stock archive does not exist: {archive}")
    require(
        sha256_file(archive) == EXPECTED_ARCHIVE_SHA256,
        "archive SHA-256 is not the attested TH08 1.00d stock data",
    )
    payload = PbgzArchive(archive).extract("musiccmt.txt")
    require(len(payload) == EXPECTED_PAYLOAD_BYTES, "musiccmt payload size changed")
    require(
        hashlib.sha256(payload).hexdigest() == EXPECTED_PAYLOAD_SHA256,
        "musiccmt payload SHA-256 changed",
    )

    tracks = parse_tracks(payload)
    descriptions = tuple(line for track in tracks for line in track.descriptions)
    require(len(tracks) == EXPECTED_TRACKS, f"track count changed: {len(tracks)}")
    require(
        len(descriptions) == EXPECTED_DESCRIPTIONS,
        f"description count changed: {len(descriptions)}",
    )
    require(max(len(track.path) for track in tracks) == 16, "path maximum changed")
    require(max(len(track.title) for track in tracks) == 34, "title maximum changed")
    require(max(map(len, descriptions)) == 68, "description maximum changed")

    actual_lengths = {
        length: sum(len(line) == length for line in descriptions)
        for length in EXPECTED_DESCRIPTION_LENGTH_COUNTS
    }
    require(
        actual_lengths == EXPECTED_DESCRIPTION_LENGTH_COUNTS,
        f"long-row distribution changed: {actual_lengths}",
    )

    all_rows = tuple(
        row
        for track in tracks
        for row in (track.path, track.title, *track.descriptions)
    )
    for index, row in enumerate(all_rows):
        try:
            row.decode("cp932", errors="strict")
        except UnicodeDecodeError as exc:
            raise StockProfileError(f"row {index} is invalid CP932: {exc}") from exc
        copied = bounded_cp932_copy(row, PORTABLE_DESCRIPTION_CAPACITY)
        require(copied == row, f"69-byte portable slot loses row {index}")
        copied.decode("cp932", errors="strict")

    percent_positions = [
        (track_index, description_index, byte_index)
        for track_index, track in enumerate(tracks)
        for description_index, line in enumerate(track.descriptions)
        for byte_index, value in enumerate(line)
        if value == ord("%")
    ]
    require(
        percent_positions == [(2, 4, 38)],
        f"raw-percent inventory changed: {percent_positions}",
    )
    check_source_contract(repo_root)

    print(
        "PASS music_room_text "
        f"payload={len(payload)} tracks={len(tracks)} descriptions={len(descriptions)} "
        "path_max=16 title_max=34 description_max=68 slot=69 "
        "raw_percent=1 cp932_lossless=1"
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, default=DEFAULT_STOCK_ARCHIVE)
    parser.add_argument("--repo-root", type=Path, default=REPO_ROOT)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    run(args.archive, args.repo_root)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except StockProfileError as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(2)
