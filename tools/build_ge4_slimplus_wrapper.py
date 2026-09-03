#!/usr/bin/env python3
"""Derive the TH07-hardware-proven Slim+ GE4 wrapper.

The accepted model-3 wrapper remains the immutable input authority. The
derived wrapper changes only its two-instruction model predicate:

    li    v1, 3
    bnel  v0, v1, reject

becomes:

    nop
    blezl v0, reject

Negative model-query errors and PSP-1000 model 0 still reject. Positive
Slim+ models proceed to the unchanged physical-size, requested-size and
kernel-export gates. This exact derived binary was validated on PSP Go by
the TH07 backend before reuse here.
"""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import tempfile


BASE_SIZE = 2150
BASE_SHA256 = "411e71b3ffb31bd91024cc0221481a787e693276c0899e05da08c3cd91dc1ab8"
CANDIDATE_SHA256 = (
    "3dc5c753497349d6fb0ab5ae2a819b240cc51e8aa412ded10bb52daa540d841d"
)
MODEL_GATE_OFFSET = 0x84
MODEL3_GATE = bytes.fromhex("030003240f004354")
SLIMPLUS_GATE = bytes.fromhex("000000000f004058")


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def derive(base: bytes) -> bytes:
    if len(base) != BASE_SIZE:
        raise ValueError(f"base size {len(base)} != frozen {BASE_SIZE}")
    actual_hash = sha256(base)
    if actual_hash != BASE_SHA256:
        raise ValueError(f"base SHA-256 {actual_hash} != frozen {BASE_SHA256}")
    if base.count(MODEL3_GATE) != 1:
        raise ValueError("frozen model-3 instruction pair is not unique")
    if base[MODEL_GATE_OFFSET : MODEL_GATE_OFFSET + len(MODEL3_GATE)] != MODEL3_GATE:
        raise ValueError("frozen model-3 instruction pair moved")

    candidate = bytearray(base)
    candidate[MODEL_GATE_OFFSET : MODEL_GATE_OFFSET + len(MODEL3_GATE)] = SLIMPLUS_GATE
    result = bytes(candidate)
    if sha256(result) != CANDIDATE_SHA256:
        raise ValueError("derived candidate hash does not match the audited contract")
    if (
        result[:MODEL_GATE_OFFSET] != base[:MODEL_GATE_OFFSET]
        or result[MODEL_GATE_OFFSET + len(MODEL3_GATE) :]
        != base[MODEL_GATE_OFFSET + len(MODEL3_GATE) :]
    ):
        raise ValueError("bytes outside the model gate changed")
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    source = args.input.resolve()
    output = args.output.resolve()
    if source == output:
        parser.error("refusing to overwrite the frozen input wrapper")

    result = derive(source.read_bytes())
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary_name: str | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="wb", prefix=".ge4-slimplus-", dir=output.parent, delete=False
        ) as temporary:
            temporary.write(result)
            temporary.flush()
            os.fsync(temporary.fileno())
            temporary_name = temporary.name
        os.replace(temporary_name, output)
        temporary_name = None
    finally:
        if temporary_name is not None:
            Path(temporary_name).unlink(missing_ok=True)

    print(
        f"GE4 Slim+ wrapper: {output} {len(result)} bytes "
        f"SHA-256 {CANDIDATE_SHA256}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
