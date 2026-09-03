#!/usr/bin/env python3
"""Generate the compact, canonical CP932 decoder used by the PSP port.

Python's standard ``cp932`` codec is the authority.  The generated header is
fully derived: no Microsoft NLS file or copied third-party conversion table is
needed.  Invalid pairs use U+0000 as a sentinel; CP932 has no valid double-byte
mapping to U+0000.
"""

from __future__ import annotations

import argparse
import codecs
import hashlib
import os
import struct
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence


REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = REPO_ROOT / "src" / "modern" / "linux" / "cp932_compact.generated.hpp"

POLICY_VERSION = "th08-psp-python-cp932-v1"
EXPECTED_TABLE_SHA256 = "95827531a32e6d0514ebea903523ad4bc20cbd034c0f8c16a0010264d5369785"
EXPECTED_DECODER_SHA256 = "4a25939e9b3bc469615c680d69ae45bf846240bf196c1e9eb0f333e328ac6660"
EXPECTED_HEADER_SHA256 = "1d61c47e1644a6cf321475e5d6403d24c07908602d62540ca71242b35b76e1f1"
EXPECTED_VALID_PAIR_COUNT = 9604
EXPECTED_COMPACT_ROW_COUNT = 55
EXPECTED_TABLE_STORAGE_BYTES = 20740
EXPECTED_SINGLE_STATUS_COUNTS = (196, 0, 60)
EXPECTED_DOUBLE_STATUS_COUNTS = (59780, 5756, 0)

STATUS_VALID = 0
STATUS_INVALID = 1
STATUS_TRUNCATED = 2

LEAD_BYTES = tuple(range(0x81, 0xA0)) + tuple(range(0xE0, 0xFD))
TRAIL_BYTES = tuple(range(0x40, 0x7F)) + tuple(range(0x80, 0xFD))
INVALID_ROW = 0xFF

ANCHORS = {
    bytes((0x5C,)): 0x005C,
    bytes((0x7E,)): 0x007E,
    bytes((0x81, 0x60)): 0xFF5E,
    bytes((0x81, 0x61)): 0x2225,
    bytes((0x81, 0x7C)): 0xFF0D,
    bytes((0x81, 0x91)): 0xFFE0,
    bytes((0x81, 0x92)): 0xFFE1,
    bytes((0x81, 0xCA)): 0xFFE2,
}


class Cp932GenerationError(RuntimeError):
    """The host authority or generated data failed a reproducibility gate."""


@dataclass(frozen=True)
class DecodeResult:
    status: int
    consumed: int
    codepoint: int


@dataclass(frozen=True)
class Cp932Model:
    lead_rows: tuple[int, ...]
    double_byte_codepoints: tuple[int, ...]
    valid_pair_count: int
    table_sha256: str
    decoder_sha256: str
    single_status_counts: tuple[int, int, int]
    double_status_counts: tuple[int, int, int]


def is_lead_byte(value: int) -> bool:
    return 0x81 <= value <= 0x9F or 0xE0 <= value <= 0xFC


def expected_single_codepoint(value: int) -> int | None:
    """Return the compact runtime formula, independently of Python decoding."""

    if 0x00 <= value <= 0x80:
        return value
    if value == 0xA0:
        return 0xF8F0
    if 0xA1 <= value <= 0xDF:
        return 0xFF61 + value - 0xA1
    if 0xFD <= value <= 0xFF:
        return 0xF8F1 + value - 0xFD
    return None


def decode_authority(data: bytes) -> DecodeResult:
    """Decode one character with streaming-friendly error consumption.

    A valid result consumes one or two bytes.  An invalid lead consumes one
    byte, matching Python's CP932 error span and allowing the following byte to
    be reconsidered.  A truncated lead consumes zero so a streaming caller can
    retain it for the next input chunk.
    """

    if not data:
        return DecodeResult(STATUS_TRUNCATED, 0, 0)
    first = data[0]
    if is_lead_byte(first):
        if len(data) < 2:
            return DecodeResult(STATUS_TRUNCATED, 0, 0)
        try:
            decoded = data[:2].decode("cp932", errors="strict")
        except UnicodeDecodeError as exc:
            if exc.start != 0 or exc.end != 1:
                raise Cp932GenerationError(
                    f"unexpected CP932 error span for {data[:2].hex()}: "
                    f"{exc.start}..{exc.end}"
                ) from exc
            return DecodeResult(STATUS_INVALID, 1, 0)
        if len(decoded) != 1:
            raise Cp932GenerationError(
                f"CP932 pair {data[:2].hex()} expands to {len(decoded)} characters"
            )
        codepoint = ord(decoded)
        if codepoint == 0 or codepoint > 0xFFFF:
            raise Cp932GenerationError(
                f"CP932 pair {data[:2].hex()} cannot use compact U16 storage"
            )
        return DecodeResult(STATUS_VALID, 2, codepoint)

    try:
        decoded = bytes((first,)).decode("cp932", errors="strict")
    except UnicodeDecodeError as exc:
        if exc.start != 0 or exc.end != 1:
            raise Cp932GenerationError(
                f"unexpected CP932 single-byte error span for {first:02X}"
            ) from exc
        return DecodeResult(STATUS_INVALID, 1, 0)
    if len(decoded) != 1:
        raise Cp932GenerationError(
            f"CP932 byte {first:02X} expands to {len(decoded)} characters"
        )
    return DecodeResult(STATUS_VALID, 1, ord(decoded))


def packed_result(result: DecodeResult) -> bytes:
    return struct.pack("<BBHI", result.status, result.consumed, 0, result.codepoint)


def exhaustive_decoder_records() -> tuple[bytes, tuple[int, int, int], tuple[int, int, int]]:
    records = bytearray()
    single_counts = [0, 0, 0]
    double_counts = [0, 0, 0]
    for first in range(0x100):
        result = decode_authority(bytes((first,)))
        single_counts[result.status] += 1
        records.extend(packed_result(result))
    for encoded in range(0x10000):
        result = decode_authority(bytes((encoded >> 8, encoded & 0xFF)))
        double_counts[result.status] += 1
        records.extend(packed_result(result))
    return bytes(records), tuple(single_counts), tuple(double_counts)


def _table_payload(lead_rows: Iterable[int], codepoints: Iterable[int]) -> bytes:
    payload = bytearray(lead_rows)
    for codepoint in codepoints:
        payload.extend(struct.pack("<H", codepoint))
    return bytes(payload)


def build_model() -> Cp932Model:
    codec = codecs.lookup("cp932")
    if codec.name != "cp932":
        raise Cp932GenerationError(f"unexpected codec authority name: {codec.name}")

    for value in range(0x100):
        result = decode_authority(bytes((value,)))
        compact = expected_single_codepoint(value)
        if is_lead_byte(value):
            if result != DecodeResult(STATUS_TRUNCATED, 0, 0):
                raise Cp932GenerationError(f"lead byte {value:02X} is not truncated alone")
        elif result != DecodeResult(STATUS_VALID, 1, compact if compact is not None else 0):
            raise Cp932GenerationError(f"single-byte formula differs at {value:02X}")

    rows: list[tuple[int, ...]] = []
    lead_rows: list[int] = []
    valid_pairs: set[tuple[int, int]] = set()
    for lead in LEAD_BYTES:
        row: list[int] = []
        for trail in TRAIL_BYTES:
            result = decode_authority(bytes((lead, trail)))
            value = result.codepoint if result.status == STATUS_VALID else 0
            row.append(value)
            if value:
                valid_pairs.add((lead, trail))
        if any(row):
            lead_rows.append(len(rows))
            rows.append(tuple(row))
        else:
            lead_rows.append(INVALID_ROW)

    # A valid CP932 pair outside the compact trail domain would otherwise be
    # silently lost.  Enumerate every possible second byte to prove none exist.
    exhaustive_valid_pairs = {
        (lead, trail)
        for lead in LEAD_BYTES
        for trail in range(0x100)
        if decode_authority(bytes((lead, trail))).status == STATUS_VALID
    }
    if exhaustive_valid_pairs != valid_pairs:
        missing = sorted(exhaustive_valid_pairs - valid_pairs)
        raise Cp932GenerationError(f"compact trail domain misses pairs: {missing[:8]}")

    codepoints = tuple(value for row in rows for value in row)
    payload = _table_payload(lead_rows, codepoints)
    records, single_counts, double_counts = exhaustive_decoder_records()
    model = Cp932Model(
        lead_rows=tuple(lead_rows),
        double_byte_codepoints=codepoints,
        valid_pair_count=len(valid_pairs),
        table_sha256=hashlib.sha256(payload).hexdigest(),
        decoder_sha256=hashlib.sha256(records).hexdigest(),
        single_status_counts=single_counts,
        double_status_counts=double_counts,
    )
    verify_model(model)
    for encoded, expected in ANCHORS.items():
        result = decode_authority(encoded)
        if result.status != STATUS_VALID or result.codepoint != expected:
            raise Cp932GenerationError(
                f"anchor {encoded.hex().upper()} maps to U+{result.codepoint:04X}, "
                f"expected U+{expected:04X}"
            )
    return model


def verify_model(model: Cp932Model) -> None:
    checks = (
        (model.table_sha256, EXPECTED_TABLE_SHA256, "table SHA-256"),
        (model.decoder_sha256, EXPECTED_DECODER_SHA256, "decoder SHA-256"),
        (model.valid_pair_count, EXPECTED_VALID_PAIR_COUNT, "valid pair count"),
        (max((value for value in model.lead_rows if value != INVALID_ROW), default=-1) + 1,
         EXPECTED_COMPACT_ROW_COUNT, "compact row count"),
        (len(model.lead_rows) + len(model.double_byte_codepoints) * 2,
         EXPECTED_TABLE_STORAGE_BYTES, "table storage bytes"),
        (model.single_status_counts, EXPECTED_SINGLE_STATUS_COUNTS, "single status counts"),
        (model.double_status_counts, EXPECTED_DOUBLE_STATUS_COUNTS, "double status counts"),
    )
    for actual, expected, label in checks:
        if actual != expected:
            raise Cp932GenerationError(f"{label} changed: {actual!r}, expected {expected!r}")


def _format_values(values: Sequence[int], width: int, per_line: int) -> tuple[str, ...]:
    rows: list[str] = []
    for offset in range(0, len(values), per_line):
        rows.append(
            "    "
            + ", ".join(
                f"0x{value:0{width}X}u" for value in values[offset : offset + per_line]
            )
            + ","
        )
    return tuple(rows)


def render_header(model: Cp932Model) -> str:
    row_lines = _format_values(model.lead_rows, 2, 12)
    codepoint_lines = _format_values(model.double_byte_codepoints, 4, 12)
    return "\n".join(
        (
            "#pragma once",
            "",
            "// Generated by tools/generate_cp932_compact.py from Python's strict cp932 codec.",
            f"// Policy: {POLICY_VERSION}",
            f"// Table payload SHA-256: {model.table_sha256}",
            f"// Exhaustive decoder SHA-256: {model.decoder_sha256}",
            "// Invalid double-byte entries are U+0000; valid pairs never map to U+0000.",
            "",
            "#include <stddef.h>",
            "#include <stdint.h>",
            "",
            "namespace th08",
            "{",
            "namespace cp932",
            "{",
            "",
            "enum class DecodeStatus : uint8_t",
            "{",
            "    Valid = 0u,",
            "    Invalid = 1u,",
            "    Truncated = 2u,",
            "};",
            "",
            "struct DecodeResult",
            "{",
            "    uint32_t codepoint;",
            "    uint8_t consumed;",
            "    DecodeStatus status;",
            "};",
            "",
            "static constexpr size_t kLeadCount = 60u;",
            "static constexpr size_t kTrailCount = 188u;",
            f"static constexpr size_t kCompactRowCount = {EXPECTED_COMPACT_ROW_COUNT}u;",
            "static constexpr uint8_t kInvalidRow = 0xFFu;",
            "",
            "static constexpr uint8_t kLeadRows[kLeadCount] = {",
            *row_lines,
            "};",
            "",
            "static constexpr uint16_t kDoubleByteCodepoints[kCompactRowCount * kTrailCount] = {",
            *codepoint_lines,
            "};",
            "",
            "constexpr bool IsLeadByte(uint8_t value)",
            "{",
            "    return (value >= 0x81u && value <= 0x9Fu) ||",
            "           (value >= 0xE0u && value <= 0xFCu);",
            "}",
            "",
            "constexpr size_t LeadOrdinal(uint8_t value)",
            "{",
            "    return value <= 0x9Fu ? static_cast<size_t>(value - 0x81u)",
            "                         : static_cast<size_t>(31u + value - 0xE0u);",
            "}",
            "",
            "constexpr int TrailOrdinal(uint8_t value)",
            "{",
            "    return value >= 0x40u && value <= 0x7Eu",
            "               ? static_cast<int>(value - 0x40u)",
            "               : (value >= 0x80u && value <= 0xFCu",
            "                      ? static_cast<int>(63u + value - 0x80u)",
            "                      : -1);",
            "}",
            "",
            "constexpr DecodeResult MakeResult(uint32_t codepoint, uint8_t consumed,",
            "                                  DecodeStatus status)",
            "{",
            "    return DecodeResult{codepoint, consumed, status};",
            "}",
            "",
            "inline DecodeResult DecodeOne(const uint8_t *input, size_t remaining)",
            "{",
            "    if (remaining == 0u)",
            "        return MakeResult(0u, 0u, DecodeStatus::Truncated);",
            "    if (input == NULL)",
            "        return MakeResult(0u, 0u, DecodeStatus::Invalid);",
            "",
            "    const uint8_t first = input[0];",
            "    if (!IsLeadByte(first))",
            "    {",
            "        if (first <= 0x80u)",
            "            return MakeResult(first, 1u, DecodeStatus::Valid);",
            "        if (first == 0xA0u)",
            "            return MakeResult(0xF8F0u, 1u, DecodeStatus::Valid);",
            "        if (first >= 0xA1u && first <= 0xDFu)",
            "            return MakeResult(0xFF61u + first - 0xA1u, 1u, DecodeStatus::Valid);",
            "        if (first >= 0xFDu)",
            "            return MakeResult(0xF8F1u + first - 0xFDu, 1u, DecodeStatus::Valid);",
            "        return MakeResult(0u, 1u, DecodeStatus::Invalid);",
            "    }",
            "",
            "    if (remaining < 2u)",
            "        return MakeResult(0u, 0u, DecodeStatus::Truncated);",
            "    const int trail = TrailOrdinal(input[1]);",
            "    if (trail < 0)",
            "        return MakeResult(0u, 1u, DecodeStatus::Invalid);",
            "    const uint8_t row = kLeadRows[LeadOrdinal(first)];",
            "    if (row == kInvalidRow)",
            "        return MakeResult(0u, 1u, DecodeStatus::Invalid);",
            "    const uint16_t codepoint =",
            "        kDoubleByteCodepoints[static_cast<size_t>(row) * kTrailCount +",
            "                               static_cast<size_t>(trail)];",
            "    return codepoint != 0u",
            "               ? MakeResult(codepoint, 2u, DecodeStatus::Valid)",
            "               : MakeResult(0u, 1u, DecodeStatus::Invalid);",
            "}",
            "",
            "static_assert(sizeof(kLeadRows) + sizeof(kDoubleByteCodepoints) ==",
            f"                  {EXPECTED_TABLE_STORAGE_BYTES}u,",
            '              "CP932 compact table storage changed");',
            'static_assert(kDoubleByteCodepoints[32] == 0xFF5Eu, "CP932 8160 mismatch");',
            'static_assert(kDoubleByteCodepoints[33] == 0x2225u, "CP932 8161 mismatch");',
            'static_assert(kDoubleByteCodepoints[60] == 0xFF0Du, "CP932 817C mismatch");',
            'static_assert(kDoubleByteCodepoints[80] == 0xFFE0u, "CP932 8191 mismatch");',
            'static_assert(kDoubleByteCodepoints[81] == 0xFFE1u, "CP932 8192 mismatch");',
            'static_assert(kDoubleByteCodepoints[137] == 0xFFE2u, "CP932 81CA mismatch");',
            "",
            "} // namespace cp932",
            "} // namespace th08",
            "",
        )
    )


def atomic_write(path: Path, payload: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if temporary.exists():
            temporary.unlink()


def print_stats(model: Cp932Model, header: str, output: Path) -> None:
    print(f"policy={POLICY_VERSION}")
    print(f"output={output}")
    print(f"header_bytes={len(header.encode('utf-8'))}")
    print(f"header_sha256={hashlib.sha256(header.encode('utf-8')).hexdigest()}")
    print(f"table_storage_bytes={EXPECTED_TABLE_STORAGE_BYTES}")
    print(f"valid_pairs={model.valid_pair_count}")
    print(f"table_sha256={model.table_sha256}")
    print(f"decoder_sha256={model.decoder_sha256}")
    print(f"single_status_counts={model.single_status_counts}")
    print(f"double_status_counts={model.double_status_counts}")


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--check", action="store_true", help="verify the tracked header")
    parser.add_argument("--stdout", action="store_true", help="print the generated header")
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    if args.check and args.stdout:
        raise Cp932GenerationError("--check and --stdout are mutually exclusive")
    model = build_model()
    header = render_header(model)
    header_hash = hashlib.sha256(header.encode("utf-8")).hexdigest()
    if EXPECTED_HEADER_SHA256 and header_hash != EXPECTED_HEADER_SHA256:
        raise Cp932GenerationError(
            f"generated header SHA-256 {header_hash}, expected {EXPECTED_HEADER_SHA256}"
        )
    if args.stdout:
        sys.stdout.write(header)
    elif args.check:
        try:
            tracked = args.output.read_text(encoding="utf-8")
        except OSError as exc:
            raise Cp932GenerationError(f"cannot read {args.output}: {exc}") from exc
        if tracked != header:
            raise Cp932GenerationError(f"generated header is out of date: {args.output}")
        print_stats(model, header, args.output)
    else:
        atomic_write(args.output, header)
        print_stats(model, header, args.output)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Cp932GenerationError as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(2)
