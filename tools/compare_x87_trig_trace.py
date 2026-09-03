#!/usr/bin/env python3
"""Compare a canonical-x87 call trace with pinned PSPDEV float libm."""

from __future__ import annotations

import argparse
import ctypes
from pathlib import Path
import re
import struct


LINE = re.compile(
    r"^completed=(?P<completed>\d+) caller=(?P<caller>[0-9a-f]+) "
    r"op=(?P<op>[cs]) angle=(?P<angle>[0-9a-f]{8}) "
    r"x87=(?P<x87>[0-9a-f]{8})$"
)


def float_from_bits(bits: int) -> float:
    return struct.unpack("<f", struct.pack("<I", bits))[0]


def bits_from_float(value: float) -> int:
    return struct.unpack("<I", struct.pack("<f", value))[0]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("trace", type=Path)
    parser.add_argument("pspdev_float_libm", type=Path)
    parser.add_argument(
        "--caller",
        action="append",
        default=[],
        help="optional hexadecimal caller image offset; may be repeated",
    )
    args = parser.parse_args()

    library = ctypes.CDLL(str(args.pspdev_float_libm.resolve()))
    library.pspdev_fd_sinf.argtypes = [ctypes.c_float]
    library.pspdev_fd_sinf.restype = ctypes.c_float
    library.pspdev_fd_cosf.argtypes = [ctypes.c_float]
    library.pspdev_fd_cosf.restype = ctypes.c_float
    caller_filter = {int(value, 16) for value in args.caller}

    records = 0
    mismatches = 0
    for line_number, line in enumerate(
        args.trace.read_text(encoding="ascii").splitlines(), 1
    ):
        match = LINE.fullmatch(line)
        if match is None:
            raise AssertionError(f"malformed trace line {line_number}: {line}")
        caller = int(match.group("caller"), 16)
        if caller_filter and caller not in caller_filter:
            continue
        records += 1
        angle_bits = int(match.group("angle"), 16)
        x87_bits = int(match.group("x87"), 16)
        operation = match.group("op")
        function = (
            library.pspdev_fd_cosf if operation == "c" else library.pspdev_fd_sinf
        )
        psp_bits = bits_from_float(function(float_from_bits(angle_bits)))
        if psp_bits == x87_bits:
            continue
        mismatches += 1
        print(
            f"completed={match.group('completed')} caller={caller:x} op={operation} "
            f"angle={angle_bits:08x} x87={x87_bits:08x} pspdev={psp_bits:08x}"
        )

    print(f"summary records={records} mismatches={mismatches}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
