#!/usr/bin/env python3
"""Exhaustively compare the generated C++ CP932 decoder with Python cp932."""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Sequence


sys.dont_write_bytecode = True
import generate_cp932_compact as generator  # noqa: E402


REPO_ROOT = Path(__file__).resolve().parents[1]
PROBE_SOURCE = REPO_ROOT / "tools" / "cp932_compact_probe.cpp"
GENERATED_HEADER = REPO_ROOT / "src" / "modern" / "linux" / "cp932_compact.generated.hpp"
RECORD_SIZE = struct.calcsize("<BBHI")


class Cp932TestError(RuntimeError):
    """The generated table, C++ API, or host authority disagreed."""


def find_compiler(requested: str | None) -> str:
    candidates = (requested, os.environ.get("CXX"), "g++", "clang++")
    for candidate in candidates:
        if candidate:
            resolved = shutil.which(candidate)
            if resolved:
                return resolved
    raise Cp932TestError("no host C++ compiler found; pass --compiler")


def compile_and_run_probe(compiler: str) -> bytes:
    with tempfile.TemporaryDirectory(prefix="th08-cp932-test-") as temporary:
        executable = Path(temporary) / "cp932_compact_probe"
        command = (
            compiler,
            "-std=c++17",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(REPO_ROOT),
            str(PROBE_SOURCE),
            "-o",
            str(executable),
        )
        compiled = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if compiled.returncode != 0:
            raise Cp932TestError(
                "host probe compilation failed:\n"
                + compiled.stderr.decode("utf-8", errors="replace")
            )
        executed = subprocess.run(
            (str(executable),), stdout=subprocess.PIPE, stderr=subprocess.PIPE
        )
        if executed.returncode != 0:
            raise Cp932TestError(
                f"host probe exited {executed.returncode}: "
                + executed.stderr.decode("utf-8", errors="replace")
            )
        return executed.stdout


def describe_record(index: int, payload: bytes) -> str:
    status, consumed, reserved, codepoint = struct.unpack("<BBHI", payload)
    if index < 0x100:
        encoded = f"{index:02X} (length 1)"
    else:
        encoded = f"{index - 0x100:04X} (length 2)"
    return (
        f"input={encoded} status={status} consumed={consumed} "
        f"reserved={reserved} codepoint=U+{codepoint:04X}"
    )


def assert_exact_records(expected: bytes, actual: bytes) -> None:
    expected_records = 0x100 + 0x10000
    expected_bytes = expected_records * RECORD_SIZE
    if len(expected) != expected_bytes:
        raise Cp932TestError(f"authority emitted {len(expected)} bytes, expected {expected_bytes}")
    if len(actual) != expected_bytes:
        raise Cp932TestError(f"C++ probe emitted {len(actual)} bytes, expected {expected_bytes}")
    if actual == expected:
        return
    for offset in range(0, expected_bytes, RECORD_SIZE):
        if actual[offset : offset + RECORD_SIZE] != expected[offset : offset + RECORD_SIZE]:
            index = offset // RECORD_SIZE
            raise Cp932TestError(
                "C++ decoder mismatch: expected "
                + describe_record(index, expected[offset : offset + RECORD_SIZE])
                + "; actual "
                + describe_record(index, actual[offset : offset + RECORD_SIZE])
            )
    raise Cp932TestError("C++ decoder output differs without a record-level mismatch")


def result_at(records: bytes, encoded: bytes) -> tuple[int, int, int]:
    if len(encoded) == 1:
        index = encoded[0]
    elif len(encoded) == 2:
        index = 0x100 + ((encoded[0] << 8) | encoded[1])
    else:
        raise Cp932TestError("test lookup accepts one or two bytes")
    status, consumed, reserved, codepoint = struct.unpack_from(
        "<BBHI", records, index * RECORD_SIZE
    )
    if reserved != 0:
        raise Cp932TestError(f"probe reserved field is nonzero at record {index}")
    return status, consumed, codepoint


def verify_anchors(records: bytes) -> None:
    for encoded, expected_codepoint in generator.ANCHORS.items():
        actual = result_at(records, encoded)
        expected = (generator.STATUS_VALID, len(encoded), expected_codepoint)
        if actual != expected:
            raise Cp932TestError(
                f"anchor {encoded.hex().upper()} is {actual}, expected {expected}"
            )
    for lead in generator.LEAD_BYTES:
        actual = result_at(records, bytes((lead,)))
        if actual != (generator.STATUS_TRUNCATED, 0, 0):
            raise Cp932TestError(f"lead {lead:02X} is not distinguished as truncated")
    for invalid in (bytes((0x81, 0x30)), bytes((0x81, 0x7F)), bytes((0x85, 0x40))):
        actual = result_at(records, invalid)
        if actual != (generator.STATUS_INVALID, 1, 0):
            raise Cp932TestError(
                f"invalid sequence {invalid.hex().upper()} is classified as {actual}"
            )


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", help="host C++17 compiler (default: CXX/g++/clang++)")
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    model = generator.build_model()
    rendered = generator.render_header(model)
    try:
        tracked = GENERATED_HEADER.read_text(encoding="utf-8")
    except OSError as exc:
        raise Cp932TestError(f"cannot read {GENERATED_HEADER}: {exc}") from exc
    if tracked != rendered:
        raise Cp932TestError("tracked generated header differs from host regeneration")
    header_hash = hashlib.sha256(tracked.encode("utf-8")).hexdigest()
    if header_hash != generator.EXPECTED_HEADER_SHA256:
        raise Cp932TestError(
            f"header SHA-256 {header_hash}, expected {generator.EXPECTED_HEADER_SHA256}"
        )

    expected, single_counts, double_counts = generator.exhaustive_decoder_records()
    compiler = find_compiler(args.compiler)
    actual = compile_and_run_probe(compiler)
    assert_exact_records(expected, actual)
    verify_anchors(actual)
    decoder_hash = hashlib.sha256(actual).hexdigest()
    if decoder_hash != generator.EXPECTED_DECODER_SHA256:
        raise Cp932TestError(
            f"probe SHA-256 {decoder_hash}, expected {generator.EXPECTED_DECODER_SHA256}"
        )

    print(f"compiler={compiler}")
    print(f"header_sha256={header_hash}")
    print(f"table_sha256={model.table_sha256}")
    print(f"decoder_sha256={decoder_hash}")
    print(f"single_inputs=256 status_counts={single_counts}")
    print(f"double_inputs=65536 status_counts={double_counts}")
    print(f"valid_double_byte_mappings={model.valid_pair_count}")
    print(f"table_storage_bytes={generator.EXPECTED_TABLE_STORAGE_BYTES}")
    print("anchors=passed invalid_vs_truncated=passed exhaustive_cpp_vs_python=passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (Cp932TestError, generator.Cp932GenerationError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(2)
