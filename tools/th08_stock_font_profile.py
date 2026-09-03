#!/usr/bin/env python3
"""Derive the stock TH08 PSP font coverage without redistributing game text.

The user's stock ``th08.dat`` remains local.  Reports and the checked-in
authority header contain Unicode codepoint numbers, counts, and hashes only;
decoded dialogue, ending text, spell comments, and archive payloads are never
written into the repository.
"""

from __future__ import annotations

import argparse
import ast
import csv
import hashlib
import re
import struct
import sys
import unicodedata
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence


REPO_ROOT = Path(__file__).resolve().parents[1]
AUTHORITY_HEADER = REPO_ROOT / "src" / "Th08FontCoverage.hpp"
DEFAULT_STOCK_ARCHIVE = (
    REPO_ROOT.parent / "original-data" / "th08-pre100d-20260901" / "th08.dat"
)

PROFILE_VERSION = "th08-1.00d-stock-font-profile-v1"
EXPECTED_ARCHIVE_SHA256 = (
    "9d7edf43b8ddd347cbb641836f6b5050745dd936f688daebbf9382ca557043bb"
)

# Filled from the attested archive/source scan below.  These are deliberately
# strict: modified archives must use the complete-font fallback rather than a
# stock subset whose missing glyph would surface only late in a playthrough.
EXPECTED_CODEPOINT_COUNT = 1531
EXPECTED_CODEPOINT_SHA256 = (
    "9b3d0d5fa1abbc82086d18788c775d939997235b6d11c2072bdb48254bd58ade"
)
EXPECTED_NAME_ENTRY_COUNT = 94
EXPECTED_NAME_ENTRY_SHA256 = (
    "d15aa5848f63221de8a59f330b385188aa999669cb3d7d4023ff1e659697af0f"
)
EXPECTED_GROUP_METRICS: dict[str, tuple[int, int, int]] = {
    "messages": (2240, 2051, 995),
    "spell_names": (431, 134, 356),
    "spell_owners": (431, 16, 70),
    "spell_comments": (862, 391, 921),
    "music_room": (163, 162, 481),
    "endings": (521, 496, 672),
    "static_ui": (331, 276, 457),
}
EXPECTED_MUSIC_LAYOUT = (21, 142)
EXPECTED_SPELL_INSTRUCTION_COUNT = 431
EXPECTED_SPELL_CARD_NUMBERS = frozenset(range(222))

MSG_FILES = (
    "msg1a.dat", "msg1b.dat", "msg1c.dat", "msg1d.dat",
    "msg2a.dat", "msg2b.dat", "msg2c.dat", "msg2d.dat",
    "msg3a.dat", "msg3b.dat", "msg3c.dat", "msg3d.dat",
    "msg4ab.dat", "msg4ac.dat", "msg4ba.dat", "msg4bd.dat", "msg4dm.dat",
    "msg5a.dat", "msg5b.dat", "msg5c.dat", "msg5d.dat",
    "msg6a.dat", "msg6b.dat", "msg6c.dat", "msg6d.dat",
    "msg7a.dat", "msg7b.dat", "msg7c.dat", "msg7d.dat",
    "msg8a.dat", "msg8b.dat", "msg8c.dat", "msg8d.dat",
)

ECL_FILES = (
    "ecldata1.ecl", "ecldata1sp.ecl",
    "ecldata2.ecl", "ecldata2sp.ecl",
    "ecldata3.ecl", "ecldata3sp.ecl",
    "ecldata4a.ecl", "ecldata4asp.ecl",
    "ecldata4b.ecl", "ecldata4bsp.ecl",
    "ecldata5.ecl", "ecldata5sp.ecl",
    "ecldata6.ecl", "ecldata6sp.ecl",
    "ecldata7.ecl", "ecldata7sp.ecl",
    "ecldata8.ecl", "ecldata8sp.ecl",
    "ecldata_al.ecl", "ecldata_rm.ecl", "ecldata_sk.ecl",
    "ecldata_yk.ecl", "ecldata_ym.ecl", "ecldata_yy.ecl",
)

ENDING_FILES = (
    "end00a.end", "end00b.end", "end00c.end",
    "end01a.end", "end01b.end", "end01c.end",
    "end02a.end", "end02b.end", "end02c.end",
    "end03a.end", "end03b.end", "end03c.end",
    "staff00.end", "staff00b.end",
)

STATIC_TEXT_SOURCES = (
    "src/PlayerBomb.cpp",
    "src/EclExIns.cpp",
    "src/ResultScreen.cpp",
    "src/TitleFormatSpellCardInfo.inl",
    "src/TitleSpellCardData.inl",
)

# Only i18n values that reach AnmManager::DrawText* are selected.  Diagnostic
# logger/window/font-family strings do not pass through SDL_ttf and therefore
# must not inflate the gameplay subset.
UI_I18N_EXACT = frozenset(
    {
        "SONG_NAME_NOT_UNLOCKED",
        "TITLE_SPELL_STAGE_INFO",
        "TITLE_SPELL_CAPTURE_PERCENTAGE",
        "TITLE_SPELLCARD_NOT_UNLOCKED",
        "TITLE_SPELLCARD_AVAILABLE",
        "TITLE_SPELL_CARD_INFO",
        "RESULT_SPELLCARD_NOT_UNLOCKED",
        "RESULT_SPELLCARD_NAME",
        "RESULT_TOTAL_TIME",
        "RESULT_TOTAL_PLAYTIME",
        "RESULT_PLAYCOUNT_INFO",
        "RESULT_CLEAR_COUNT",
        "RESULT_CONTINUE_COUNT",
        "RESULT_PRACTICE_COUNT",
    }
)
UI_I18N_PREFIXES = (
    "WARN_BGM_NOT_UNLOCKED",
    "TITLE_STARTMENU_HELPTEXT",
    "TITLE_OPTIONS_HELPTEXT",
    "TITLE_KEYCONFIG_HELPTEXT",
    "TITLE_FULLWIDTH_DIGIT_",
    "RESULT_SHOT_",
)


class StockProfileError(RuntimeError):
    """The local archive/source tree is not the supported stock profile."""


@dataclass(frozen=True)
class ProfileGroup:
    name: str
    sources: tuple[str, ...]
    row_count: int
    unique_row_count: int
    codepoints: frozenset[int]
    content_sha256: str


@dataclass(frozen=True)
class StockFontProfile:
    archive_filename: str
    archive_sha256: str
    groups: tuple[ProfileGroup, ...]
    codepoints: frozenset[int]
    codepoint_sha256: str
    name_entry_codepoints: frozenset[int]
    name_entry_sha256: str


@dataclass(frozen=True)
class PbgzEntry:
    name: str
    data_offset: int
    decompressed_size: int
    metadata: int


def sha256_bytes(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def codepoints_from_text(text: str) -> frozenset[int]:
    result: set[int] = set()
    for char in unicodedata.normalize("NFC", text):
        if 0xD800 <= ord(char) <= 0xDFFF:
            raise StockProfileError(f"text contains surrogate U+{ord(char):04X}")
        if not unicodedata.category(char).startswith("C"):
            result.add(ord(char))
    return frozenset(result)


def authority_codepoint_hash(codepoints: Iterable[int]) -> str:
    payload = "".join(f"U+{value:04X}\n" for value in sorted(set(codepoints)))
    return sha256_bytes(payload.encode("ascii"))


def _rows_hash(rows: Sequence[str]) -> str:
    payload = bytearray()
    for row in rows:
        encoded = unicodedata.normalize("NFC", row).encode("utf-8")
        payload.extend(struct.pack("<I", len(encoded)))
        payload.extend(encoded)
    return sha256_bytes(bytes(payload))


def _make_group(name: str, sources: Sequence[str], rows: Sequence[str]) -> ProfileGroup:
    normalized = tuple(unicodedata.normalize("NFC", row) for row in rows)
    return ProfileGroup(
        name=name,
        sources=tuple(sources),
        row_count=len(normalized),
        unique_row_count=len(set(normalized)),
        codepoints=codepoints_from_text("".join(normalized)),
        content_sha256=_rows_hash(normalized),
    )


def _decode_cp932(payload: bytes, source: str) -> str:
    try:
        return payload.decode("cp932")
    except UnicodeDecodeError as exc:
        raise StockProfileError(f"{source}: invalid CP932 text: {exc}") from exc


def _decrypt_interleaved(
    payload: bytes, xor_value: int, xor_increment: int, chunk_size: int, max_bytes: int
) -> bytes:
    """Mirror FileSystem::Decrypt, including its short-tail rule."""

    remaining = len(payload)
    unencrypted = (
        remaining % chunk_size if remaining % chunk_size < chunk_size // 4 else 0
    )
    unencrypted += remaining & 1
    remaining -= unencrypted
    source = 0
    destination = 0
    output = bytearray(len(payload))

    while remaining > 0 and max_bytes > 0:
        current_chunk = min(chunk_size, remaining)
        base = destination
        out_pos = base + current_chunk - 1
        for _ in range((current_chunk + 1) // 2):
            output[out_pos] = payload[source] ^ xor_value
            out_pos -= 2
            source += 1
            xor_value = (xor_value + xor_increment) & 0xFF
        out_pos = base + current_chunk - 2
        for _ in range(current_chunk // 2):
            output[out_pos] = payload[source] ^ xor_value
            out_pos -= 2
            source += 1
            xor_value = (xor_value + xor_increment) & 0xFF
        remaining -= current_chunk
        destination += current_chunk
        max_bytes -= current_chunk

    remaining += unencrypted
    output[destination : destination + remaining] = payload[source : source + remaining]
    return bytes(output)


class _BitReader:
    def __init__(self, payload: bytes, source: str):
        self.payload = payload
        self.source = source
        self.byte_pos = 0
        self.bit_mask = 0x80
        self.current = 0
        self.virtual_zero_bits = 0
        self.current_is_virtual = False

    def read_bit(self) -> int:
        if self.bit_mask == 0x80:
            if self.byte_pos >= len(self.payload):
                # The retail decoder supplies a zero byte after the physical
                # range and consumes its zero-offset end marker.  Permit only
                # that one virtual byte; any longer read remains corruption.
                if self.virtual_zero_bits >= 16:
                    raise StockProfileError(f"{self.source}: truncated LZSS bitstream")
                self.current = 0
                self.current_is_virtual = True
            else:
                self.current = self.payload[self.byte_pos]
                self.byte_pos += 1
                self.current_is_virtual = False
        value = 1 if self.current & self.bit_mask else 0
        if self.current_is_virtual:
            self.virtual_zero_bits += 1
        self.bit_mask >>= 1
        if self.bit_mask == 0:
            self.bit_mask = 0x80
        return value

    def read_bits(self, count: int) -> int:
        value = 0
        for _ in range(count):
            value = (value << 1) | self.read_bit()
        return value


def _lzss_decompress(payload: bytes, expected_size: int, source: str) -> bytes:
    dictionary = bytearray(8192)
    head = 1
    output = bytearray()
    bits = _BitReader(payload, source)
    while True:
        if bits.read_bit():
            value = bits.read_bits(8)
            output.append(value)
            dictionary[head] = value
            head = (head + 1) & 0x1FFF
        else:
            offset = bits.read_bits(13)
            if offset == 0:
                break
            length = bits.read_bits(4) + 3
            for index in range(length):
                value = dictionary[(offset + index) & 0x1FFF]
                output.append(value)
                dictionary[head] = value
                head = (head + 1) & 0x1FFF
                if len(output) > expected_size:
                    raise StockProfileError(f"{source}: LZSS output exceeds table size")
    if len(output) != expected_size:
        raise StockProfileError(
            f"{source}: decompressed {len(output)} bytes, expected {expected_size}"
        )
    return bytes(output)


_RESOURCE_DECRYPT_PARAMS = (
    (0x5D, 0x1B, 0x37, 0x0040, 0x2800),
    (0x74, 0x51, 0xE9, 0x0040, 0x3000),
    (0x71, 0xC1, 0x51, 0x1400, 0x2000),
    (0x8A, 0x03, 0x19, 0x1400, 0x7800),
    (0x95, 0xAB, 0xCD, 0x0200, 0x1000),
    (0xB7, 0x12, 0x34, 0x0400, 0x2800),
    (0x9D, 0x35, 0x97, 0x0080, 0x2800),
    (0xAA, 0x99, 0x37, 0x0400, 0x1000),
)


def _decrypt_resource(payload: bytes, source: str) -> bytes:
    if len(payload) < 4 or payload[:3] != b"edz":
        return payload
    for index, (key, xor_value, increment, chunk_size, max_bytes) in enumerate(
        _RESOURCE_DECRYPT_PARAMS
    ):
        encoded_key = (key - (index << 4) - 0x10) & 0xFF
        if payload[3] == encoded_key:
            return _decrypt_interleaved(
                payload[4:], xor_value, increment, chunk_size, max_bytes
            )
    raise StockProfileError(f"{source}: unknown encrypted-resource key 0x{payload[3]:02X}")


class PbgzArchive:
    """Small read-only TH08 PBGZ reader used only by the local audit tool."""

    def __init__(self, path: Path):
        self.path = path
        self.payload = path.read_bytes()
        if len(self.payload) < 16 or self.payload[:4] != b"PBGZ":
            raise StockProfileError(f"{path}: not a TH08 PBGZ archive")
        header = _decrypt_interleaved(self.payload[4:16], 0x1B, 0x37, 12, 0x400)
        encoded_count, encoded_table_offset, encoded_table_size = struct.unpack(
            "<iii", header
        )
        count = encoded_count - 123456
        table_offset = encoded_table_offset - 345678
        table_size = encoded_table_size - 567891
        if count <= 0 or count > 4096:
            raise StockProfileError(f"{path.name}: invalid PBGZ entry count {count}")
        if table_offset < 16 or table_offset >= len(self.payload) or table_size <= 0:
            raise StockProfileError(f"{path.name}: invalid PBGZ table range")

        encrypted_table = self.payload[table_offset:]
        compressed_table = _decrypt_interleaved(
            encrypted_table, 0x3E, 0x9B, 0x80, 0x400
        )
        table = _lzss_decompress(compressed_table, table_size, "PBGZ file table")
        cursor = 0
        entries: list[PbgzEntry] = []
        for _ in range(count):
            try:
                end = table.index(0, cursor)
            except ValueError as exc:
                raise StockProfileError("PBGZ file table has unterminated name") from exc
            try:
                name = table[cursor:end].decode("ascii")
            except UnicodeDecodeError as exc:
                raise StockProfileError("PBGZ file table has non-ASCII name") from exc
            cursor = end + 1
            if cursor + 12 > len(table):
                raise StockProfileError("PBGZ file table record is truncated")
            data_offset, decompressed_size, metadata = struct.unpack_from(
                "<III", table, cursor
            )
            cursor += 12
            entries.append(PbgzEntry(name, data_offset, decompressed_size, metadata))
        offsets = [entry.data_offset for entry in entries] + [table_offset]
        if any(first > second for first, second in zip(offsets, offsets[1:])):
            raise StockProfileError("PBGZ entry offsets are not monotonic")
        if offsets[0] < 16 or offsets[-1] != table_offset:
            raise StockProfileError("PBGZ data region is invalid")
        self.entries = tuple(entries)
        self.by_name = {entry.name.casefold(): index for index, entry in enumerate(entries)}
        if len(self.by_name) != len(entries):
            raise StockProfileError("PBGZ file table has duplicate case-folded names")
        self.table_offset = table_offset

    def extract(self, wanted_name: str) -> bytes:
        try:
            index = self.by_name[wanted_name.casefold()]
        except KeyError as exc:
            raise StockProfileError(f"archive entry is missing: {wanted_name}") from exc
        entry = self.entries[index]
        next_offset = (
            self.entries[index + 1].data_offset
            if index + 1 < len(self.entries)
            else self.table_offset
        )
        compressed = self.payload[entry.data_offset:next_offset]
        raw = _lzss_decompress(compressed, entry.decompressed_size, entry.name)
        return _decrypt_resource(raw, entry.name)


def _decode_xor_text(field: bytes, key: int, source: str) -> str:
    decoded = bytearray()
    for value in field:
        plain = value ^ key
        if plain == 0:
            return _decode_cp932(bytes(decoded), source)
        decoded.append(plain)
    raise StockProfileError(f"{source}: unterminated XOR text field")


def _decode_xor_fixed_text(field: bytes, key: int, source: str) -> str:
    """Decode a fixed-width field whose last glyph may occupy the full field."""

    decoded = bytes(value ^ key for value in field)
    nul = decoded.find(b"\0")
    if nul >= 0:
        decoded = decoded[:nul]
    return _decode_cp932(decoded, source)


def parse_msg_rows(data: bytes, source: str) -> tuple[str, ...]:
    if len(data) < 4:
        raise StockProfileError(f"{source}: truncated MSG header")
    message_count = struct.unpack_from("<i", data)[0]
    if message_count <= 0 or message_count > (len(data) - 4) // 4:
        raise StockProfileError(f"{source}: invalid message count {message_count}")
    offsets = struct.unpack_from(f"<{message_count}I", data, 4)
    header_size = 4 + message_count * 4
    if any(offset < header_size or offset >= len(data) for offset in offsets):
        raise StockProfileError(f"{source}: invalid message offset")

    rows: list[str] = []
    starts = sorted(set(offsets))
    for stream_index, start in enumerate(starts):
        end = starts[stream_index + 1] if stream_index + 1 < len(starts) else len(data)
        cursor = start
        terminated = False
        while cursor + 4 <= end:
            _time, opcode, payload_size = struct.unpack_from("<HBB", data, cursor)
            instruction_size = 4 + payload_size
            if instruction_size > end - cursor:
                raise StockProfileError(f"{source}: MSG instruction crosses stream boundary")
            payload = data[cursor + 4 : cursor + instruction_size]
            if opcode == 0:
                terminated = True
                break
            if opcode == 3:
                if payload_size < 5:
                    raise StockProfileError(f"{source}: short dialogue-text instruction")
                text = _decode_xor_text(payload[4:], 0x77, source)
                if text:
                    rows.append(text)
            elif opcode in (16, 19, 20):
                text = _decode_xor_text(payload, 0x77, source)
                if text:
                    rows.append(text)
            cursor += instruction_size
        if not terminated:
            raise StockProfileError(f"{source}: MSG stream has no delete instruction")
    return tuple(rows)


def parse_ecl_spell_rows(
    data: bytes, source: str
) -> tuple[
    tuple[str, ...],
    tuple[str, ...],
    tuple[str, ...],
    tuple[tuple[int, str, str, str, str], ...],
]:
    if len(data) < 0x48:
        raise StockProfileError(f"{source}: truncated ECL header")
    version, sub_count, timeline_count = struct.unpack_from("<Ihh", data)
    if version != 0x800 or sub_count <= 0 or timeline_count < 0 or timeline_count > 16:
        raise StockProfileError(f"{source}: invalid ECL header")
    header_size = 0x48 + sub_count * 4
    if header_size > len(data):
        raise StockProfileError(f"{source}: truncated ECL subroutine table")
    timeline_offsets = struct.unpack_from("<16I", data, 8)[:timeline_count]
    sub_offsets = struct.unpack_from(f"<{sub_count}I", data, 0x48)
    section_offsets = tuple(value for value in timeline_offsets + sub_offsets if value)
    if any(value < header_size or value >= len(data) or value % 4 for value in section_offsets):
        raise StockProfileError(f"{source}: invalid ECL section offset")
    boundaries = sorted(set(section_offsets)) + [len(data)]

    names: list[str] = []
    owners: list[str] = []
    comments: list[str] = []
    records: list[tuple[int, str, str, str, str]] = []
    for start in sorted(set(sub_offsets)):
        end = min(value for value in boundaries if value > start)
        cursor = start
        while cursor < end:
            if cursor + 12 > end:
                raise StockProfileError(f"{source}: truncated ECL instruction header")
            _time, opcode, next_offset, _reserved, _difficulty, _flags = struct.unpack_from(
                "<IhhBBH", data, cursor
            )
            if next_offset < 12 or next_offset % 4 or next_offset > end - cursor:
                raise StockProfileError(
                    f"{source}: invalid ECL instruction span {next_offset} at 0x{cursor:X}"
                )
            if opcode == 122:
                if next_offset < 0xF4:
                    raise StockProfileError(f"{source}: short spell-card instruction")
                name = _decode_xor_text(data[cursor + 0x14 : cursor + 0x44], 0xAA, source)
                owner = _decode_xor_text(data[cursor + 0x44 : cursor + 0x74], 0xBB, source)
                # ECL retains the spell-practice comments XORed. EndSpell
                # removes these two keys while committing Catk history.
                comment1 = _decode_xor_fixed_text(
                    data[cursor + 0x74 : cursor + 0xB4], 0xDD, source
                )
                comment2 = _decode_xor_fixed_text(
                    data[cursor + 0xB4 : cursor + 0xF4], 0xEE, source
                )
                spell_card_number = struct.unpack_from("<H", data, cursor + 0x0E)[0]
                records.append((spell_card_number, name, owner, comment1, comment2))
                if name:
                    names.append(name)
                if owner:
                    owners.append(owner)
                if comment1:
                    comments.append(comment1)
                if comment2:
                    comments.append(comment2)
            cursor += next_offset
        if cursor != end:
            raise StockProfileError(f"{source}: ECL stream does not end on its boundary")
    return tuple(names), tuple(owners), tuple(comments), tuple(records)


def _read_line(data: bytes, cursor: int) -> tuple[bytes, int]:
    start = cursor
    while cursor < len(data) and data[cursor] not in (10, 13):
        cursor += 1
    line = data[start:cursor]
    while cursor < len(data) and data[cursor] in (10, 13):
        cursor += 1
    return line, cursor


def parse_music_rows(data: bytes, source: str) -> tuple[tuple[str, ...], int, int]:
    cursor = 0
    titles: list[str] = []
    descriptions: list[str] = []
    while cursor < len(data):
        if data[cursor] != ord("@"):
            cursor += 1
            continue
        cursor += 1
        _path, cursor = _read_line(data, cursor)
        title, cursor = _read_line(data, cursor)
        titles.append(_decode_cp932(title, source))
        for _ in range(7):
            if cursor >= len(data) or data[cursor] == ord("@"):
                break
            line, cursor = _read_line(data, cursor)
            if line:
                descriptions.append(_decode_cp932(line, source))
    return tuple(titles + descriptions), len(titles), len(descriptions)


_ENDING_PARAM_COUNTS = {
    ord("a"): 3,
    ord("V"): 2,
    ord("v"): 1,
    ord("M"): 1,
    ord("s"): 2,
    ord("c"): 1,
    ord("r"): 2,
    ord("w"): 2,
    ord("0"): 1,
    ord("1"): 1,
    ord("2"): 1,
    ord("3"): 1,
}


def parse_ending_rows(data: bytes, source: str) -> tuple[str, ...]:
    cursor = 0
    rows: list[str] = []
    current = bytearray()
    while cursor < len(data):
        value = data[cursor]
        if value == ord("@"):
            if current:
                raise StockProfileError(f"{source}: ending command interrupts a text row")
            cursor += 1
            if cursor >= len(data):
                raise StockProfileError(f"{source}: truncated ending command")
            command = data[cursor]
            cursor += 1
            for _ in range(_ENDING_PARAM_COUNTS.get(command, 0)):
                while cursor < len(data) and data[cursor] != 0:
                    cursor += 1
                if cursor >= len(data):
                    raise StockProfileError(f"{source}: unterminated ending parameter")
                while cursor < len(data) and data[cursor] == 0:
                    cursor += 1
            while cursor < len(data) and data[cursor] not in (10, 13):
                cursor += 1
            while cursor < len(data) and data[cursor] in (10, 13):
                cursor += 1
            continue
        if value in (0, 10, 13):
            if current:
                rows.append(_decode_cp932(bytes(current), source))
                current.clear()
            while cursor < len(data) and data[cursor] in (0, 10, 13):
                cursor += 1
            continue
        if cursor + 1 >= len(data):
            raise StockProfileError(f"{source}: truncated two-byte ending text")
        current.extend(data[cursor : cursor + 2])
        cursor += 2
    if current:
        rows.append(_decode_cp932(bytes(current), source))
    return tuple(rows)


_C_STRING_RE = re.compile(r'(?:(?:u8|u|U|L))?"(?:\\.|[^"\\])*"')


def _strip_cpp_comments(source: str) -> str:
    output: list[str] = []
    cursor = 0
    state = "code"
    while cursor < len(source):
        char = source[cursor]
        next_char = source[cursor + 1] if cursor + 1 < len(source) else ""
        if state == "code":
            if char == '"':
                state = "string"
                output.append(char)
            elif char == "'":
                state = "char"
                output.append(char)
            elif char == "/" and next_char == "/":
                state = "line_comment"
                output.extend("  ")
                cursor += 1
            elif char == "/" and next_char == "*":
                state = "block_comment"
                output.extend("  ")
                cursor += 1
            else:
                output.append(char)
        elif state in ("string", "char"):
            output.append(char)
            if char == "\\" and next_char:
                output.append(next_char)
                cursor += 1
            elif (state == "string" and char == '"') or (state == "char" and char == "'"):
                state = "code"
        elif state == "line_comment":
            output.append("\n" if char == "\n" else " ")
            if char == "\n":
                state = "code"
        else:
            output.append("\n" if char == "\n" else " ")
            if char == "*" and next_char == "/":
                output.append(" ")
                cursor += 1
                state = "code"
        cursor += 1
    if state == "block_comment":
        raise StockProfileError("unterminated C block comment")
    return "".join(output)


def _decode_c_literal(token: str, source: str) -> str:
    quote = token.index('"')
    try:
        value = ast.literal_eval(token[quote:])
    except (SyntaxError, ValueError) as exc:
        raise StockProfileError(f"{source}: cannot decode C string {token!r}: {exc}") from exc
    if any(ord(char) > 0xFF for char in value):
        return value
    return _decode_cp932(value.encode("latin-1"), source)


def parse_static_source_rows(repo_root: Path = REPO_ROOT) -> tuple[str, ...]:
    rows: list[str] = []
    for relative in STATIC_TEXT_SOURCES:
        source = _strip_cpp_comments((repo_root / relative).read_text(encoding="utf-8"))
        rows.extend(_decode_c_literal(token, relative) for token in _C_STRING_RE.findall(source))
    return tuple(rows)


def parse_ui_i18n_rows(repo_root: Path = REPO_ROOT) -> tuple[str, ...]:
    path = repo_root / "config" / "i18n.csv"
    rows: list[str] = []
    selected: set[str] = set()
    with path.open(encoding="utf-8", newline="") as stream:
        for row in csv.DictReader(stream):
            identifier = row["identifier"]
            if identifier in UI_I18N_EXACT or identifier.startswith(UI_I18N_PREFIXES):
                rows.append(row["jp"])
                selected.add(identifier)
    missing = UI_I18N_EXACT.difference(selected)
    if missing:
        raise StockProfileError("i18n UI rows missing: " + ", ".join(sorted(missing)))
    for prefix in UI_I18N_PREFIXES:
        if not any(identifier.startswith(prefix) for identifier in selected):
            raise StockProfileError(f"i18n UI prefix has no rows: {prefix}")
    return tuple(rows)


def extract_name_entry_charset(repo_root: Path = REPO_ROOT) -> str:
    source = (repo_root / "src" / "ResultScreen.cpp").read_text(encoding="utf-8")
    pattern = re.compile(
        r"const\s+char\s*\*\s*g_AlphabetList\s*=\s*"
        r"((?:\"(?:\\.|[^\"\\])*\"\s*)+);"
    )
    match = pattern.search(source)
    if not match:
        raise StockProfileError("cannot extract g_AlphabetList")
    return "".join(
        _decode_c_literal(token, "src/ResultScreen.cpp:g_AlphabetList")
        for token in _C_STRING_RE.findall(match.group(1))
    )


def build_stock_profile(archive: Path, *, repo_root: Path = REPO_ROOT) -> StockFontProfile:
    if not archive.is_file():
        raise StockProfileError(f"stock archive does not exist: {archive}")
    archive_hash = sha256_file(archive)
    if archive_hash != EXPECTED_ARCHIVE_SHA256:
        raise StockProfileError(
            f"unsupported th08.dat SHA-256 {archive_hash}; use the full font for modified data"
        )
    pbgz = PbgzArchive(archive)
    expected_names = set(MSG_FILES + ECL_FILES + ENDING_FILES + ("musiccmt.txt",))
    missing_entries = {name for name in expected_names if name.casefold() not in pbgz.by_name}
    if missing_entries:
        raise StockProfileError(
            "stock archive is missing text entries: " + ", ".join(sorted(missing_entries))
        )
    archive_msg_files = {
        entry.name.casefold()
        for entry in pbgz.entries
        if re.fullmatch(r"msg[^/\\]*\.dat", entry.name, flags=re.IGNORECASE)
    }
    archive_ecl_files = {
        entry.name.casefold() for entry in pbgz.entries if entry.name.casefold().endswith(".ecl")
    }
    archive_ending_files = {
        entry.name.casefold() for entry in pbgz.entries if entry.name.casefold().endswith(".end")
    }
    expected_sets = (
        ("MSG", archive_msg_files, {name.casefold() for name in MSG_FILES}),
        ("ECL", archive_ecl_files, {name.casefold() for name in ECL_FILES}),
        ("ending", archive_ending_files, {name.casefold() for name in ENDING_FILES}),
    )
    for label, actual, expected in expected_sets:
        if actual != expected:
            raise StockProfileError(
                f"{label} archive inventory mismatch; missing={sorted(expected - actual)} "
                f"unexpected={sorted(actual - expected)}"
            )

    message_rows: list[str] = []
    for name in MSG_FILES:
        message_rows.extend(parse_msg_rows(pbgz.extract(name), name))

    spell_names: list[str] = []
    spell_owners: list[str] = []
    spell_comments: list[str] = []
    spell_records: list[tuple[int, str, str, str, str]] = []
    for name in ECL_FILES:
        names, owners, comments, records = parse_ecl_spell_rows(pbgz.extract(name), name)
        spell_names.extend(names)
        spell_owners.extend(owners)
        spell_comments.extend(comments)
        spell_records.extend(records)

    by_spell_number: dict[int, set[tuple[str, str, str, str]]] = {}
    for number, name, owner, comment1, comment2 in spell_records:
        by_spell_number.setdefault(number, set()).add((name, owner, comment1, comment2))
    if len(spell_records) != EXPECTED_SPELL_INSTRUCTION_COUNT:
        raise StockProfileError(
            f"ECL spell instruction count {len(spell_records)}, "
            f"expected {EXPECTED_SPELL_INSTRUCTION_COUNT}"
        )
    if frozenset(by_spell_number) != EXPECTED_SPELL_CARD_NUMBERS:
        raise StockProfileError("ECL scan does not cover every stock spell-card number 0..221")
    conflicting = sorted(number for number, values in by_spell_number.items() if len(values) != 1)
    if conflicting:
        raise StockProfileError(
            "ECL duplicate spell records disagree for numbers: "
            + ", ".join(str(value) for value in conflicting)
        )

    music_rows, music_titles, music_descriptions = parse_music_rows(
        pbgz.extract("musiccmt.txt"), "musiccmt.txt"
    )
    ending_rows: list[str] = []
    for name in ENDING_FILES:
        ending_rows.extend(parse_ending_rows(pbgz.extract(name), name))
    static_rows = parse_static_source_rows(repo_root) + parse_ui_i18n_rows(repo_root)

    groups = (
        _make_group("messages", MSG_FILES, message_rows),
        _make_group("spell_names", ECL_FILES, spell_names),
        _make_group("spell_owners", ECL_FILES, spell_owners),
        _make_group("spell_comments", ECL_FILES, spell_comments),
        _make_group("music_room", ("musiccmt.txt",), music_rows),
        _make_group("endings", ENDING_FILES, ending_rows),
        _make_group(
            "static_ui",
            STATIC_TEXT_SOURCES + ("config/i18n.csv",),
            static_rows,
        ),
    )

    if EXPECTED_GROUP_METRICS:
        for group in groups:
            actual = (group.row_count, group.unique_row_count, len(group.codepoints))
            expected = EXPECTED_GROUP_METRICS.get(group.name)
            if expected is None or actual != expected:
                raise StockProfileError(
                    f"{group.name}: stock profile mismatch {actual}, expected {expected}"
                )
    if EXPECTED_MUSIC_LAYOUT != (0, 0) and (
        music_titles,
        music_descriptions,
    ) != EXPECTED_MUSIC_LAYOUT:
        raise StockProfileError(
            "music_room: parser layout mismatch "
            f"titles={music_titles} descriptions={music_descriptions}"
        )

    name_entry = extract_name_entry_charset(repo_root)
    name_codepoints = codepoints_from_text(name_entry)
    name_hash = authority_codepoint_hash(name_codepoints)
    if EXPECTED_NAME_ENTRY_COUNT and (
        len(name_codepoints) != EXPECTED_NAME_ENTRY_COUNT
        or name_hash != EXPECTED_NAME_ENTRY_SHA256
    ):
        raise StockProfileError(
            "g_AlphabetList authority mismatch: "
            f"count={len(name_codepoints)} hash={name_hash}"
        )

    codepoints: set[int] = set(range(0x20, 0x7F))
    codepoints.update(name_codepoints)
    for group in groups:
        codepoints.update(group.codepoints)
    profile_hash = authority_codepoint_hash(codepoints)
    if EXPECTED_CODEPOINT_COUNT and (
        len(codepoints) != EXPECTED_CODEPOINT_COUNT
        or profile_hash != EXPECTED_CODEPOINT_SHA256
    ):
        raise StockProfileError(
            "archive/source text is not the supported stock profile: "
            f"count={len(codepoints)} hash={profile_hash}; use the full font for modified data"
        )
    return StockFontProfile(
        archive_filename=archive.name,
        archive_sha256=archive_hash,
        groups=groups,
        codepoints=frozenset(codepoints),
        codepoint_sha256=profile_hash,
        name_entry_codepoints=name_codepoints,
        name_entry_sha256=name_hash,
    )


def render_authority_header(profile: StockFontProfile) -> str:
    values = sorted(profile.codepoints)
    rows = []
    for offset in range(0, len(values), 8):
        rows.append(
            "    "
            + ", ".join(f"0x{value:04X}u" for value in values[offset : offset + 8])
            + ","
        )
    return "\n".join(
        (
            "#pragma once",
            "",
            "// Generated numeric coverage metadata only; no font or original text is embedded.",
            "// Source profile SHA-256: " + profile.codepoint_sha256,
            "#include <SDL2/SDL_stdinc.h>",
            "",
            "static const Uint32 kTh08PspStockFontCodepoints[] = {",
            *rows,
            "};",
            f"static const Uint32 kTh08PspStockFontCodepointCount = {len(values)}u;",
            "static const char kTh08PspStockFontProfileSha256[] =",
            f'    "{profile.codepoint_sha256}";',
            f"static const Uint32 kTh08PspNameEntryCodepointCount = {len(profile.name_entry_codepoints)}u;",
            "static const char kTh08PspNameEntryCodepointSha256[] =",
            f'    "{profile.name_entry_sha256}";',
            "static_assert(sizeof(kTh08PspStockFontCodepoints) /",
            "                  sizeof(kTh08PspStockFontCodepoints[0]) ==",
            "              kTh08PspStockFontCodepointCount,",
            '              "TH08 font authority count mismatch");',
            "",
        )
    )


def verify_authority_header(
    profile: StockFontProfile, path: Path = AUTHORITY_HEADER
) -> None:
    expected = render_authority_header(profile)
    try:
        actual = path.read_text(encoding="utf-8")
    except OSError as exc:
        raise StockProfileError(f"cannot read authority header {path}: {exc}") from exc
    if actual != expected:
        raise StockProfileError(
            f"authority header is out of sync: {path}; regenerate/review the numeric table"
        )


def _parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Audit the stock TH08 1.00d font character profile."
    )
    parser.add_argument("--archive", type=Path, default=DEFAULT_STOCK_ARCHIVE)
    parser.add_argument(
        "--render-authority",
        action="store_true",
        help="print the numeric C++ authority header to stdout",
    )
    parser.add_argument(
        "--verify-authority",
        action="store_true",
        help="compare src/Th08FontCoverage.hpp with the derived profile",
    )
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = _parse_args(argv)
    profile = build_stock_profile(args.archive)
    if args.verify_authority:
        verify_authority_header(profile)
    if args.render_authority:
        sys.stdout.write(render_authority_header(profile))
        return 0
    print(
        f"archive_sha256={profile.archive_sha256} codepoints={len(profile.codepoints)} "
        f"codepoint_sha256={profile.codepoint_sha256}"
    )
    print(
        f"name_entry_codepoints={len(profile.name_entry_codepoints)} "
        f"name_entry_sha256={profile.name_entry_sha256}"
    )
    for group in profile.groups:
        print(
            f"{group.name}: rows={group.row_count} unique_rows={group.unique_row_count} "
            f"codepoints={len(group.codepoints)} sha256={group.content_sha256}"
        )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except StockProfileError as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(2)
