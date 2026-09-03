#!/usr/bin/env python3
"""Verify and build the diagnostic TH08 1.00d x87 replay oracle."""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import tempfile


TARGET_SIZE = 840_704
TARGET_SHA256 = "330fbdbf58a710829d65277b4f312cfbb38d5448b3df523e79350b879213d924"


def pe_va_to_offset(image: bytes, va: int) -> int:
    pe_offset = struct.unpack_from("<I", image, 0x3C)[0]
    if image[pe_offset : pe_offset + 4] != b"PE\0\0":
        raise AssertionError("target is not a PE image")
    section_count = struct.unpack_from("<H", image, pe_offset + 6)[0]
    optional_size = struct.unpack_from("<H", image, pe_offset + 20)[0]
    optional = pe_offset + 24
    if struct.unpack_from("<H", image, optional)[0] != 0x10B:
        raise AssertionError("target is not PE32")
    image_base = struct.unpack_from("<I", image, optional + 28)[0]
    rva = va - image_base
    sections = optional + optional_size
    for index in range(section_count):
        header = sections + index * 40
        virtual_size, virtual_address, raw_size, raw_offset = struct.unpack_from(
            "<IIII", image, header + 8
        )
        extent = max(virtual_size, raw_size)
        if virtual_address <= rva < virtual_address + extent:
            delta = rva - virtual_address
            if delta >= raw_size:
                raise AssertionError(f"VA 0x{va:08x} has no raw file bytes")
            return raw_offset + delta
    raise AssertionError(f"VA 0x{va:08x} is outside mapped sections")


def require_bytes(image: bytes, va: int, expected_hex: str, label: str) -> None:
    expected = bytes.fromhex(expected_hex)
    offset = pe_va_to_offset(image, va)
    actual = image[offset : offset + len(expected)]
    if actual != expected:
        raise AssertionError(
            f"{label} mismatch at VA 0x{va:08x}: "
            f"expected={expected.hex()} actual={actual.hex()}"
        )


def run(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, check=True, text=True, **kwargs)


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser()
    parser.add_argument("--target", type=Path, default=root / "resources/th08.exe")
    parser.add_argument(
        "--source", type=Path, default=root / "tools/canonical_x87_trig_preload.cpp"
    )
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    args = parser.parse_args()

    image = args.target.read_bytes()
    digest = hashlib.sha256(image).hexdigest()
    if len(image) != TARGET_SIZE or digest != TARGET_SHA256:
        raise AssertionError(
            f"wrong target: size={len(image)} sha256={digest}; "
            f"expected size={TARGET_SIZE} sha256={TARGET_SHA256}"
        )

    require_bytes(
        image,
        0x00408D40,
        "55 8b ec 51 d9 45 08 e8 a4 b1 09 00 d9 55 fc 8b e5 5d c2 04 00",
        "_cosf wrapper",
    )
    require_bytes(
        image,
        0x00409060,
        "55 8b ec 51 d9 45 08 e8 34 af 09 00 d9 55 fc 8b e5 5d c2 04 00",
        "_sinf wrapper",
    )
    require_bytes(
        image,
        0x004A3F0D,
        "52 9b d9 3c 24 74 50 66 81 3c 24 7f 02 74 06 d9 2d 28 da 4b 00 d9 ff",
        "x87 cosine core",
    )
    require_bytes(
        image,
        0x004A3FBD,
        "52 9b d9 3c 24 74 50 66 81 3c 24 7f 02 74 06 d9 2d 28 da 4b 00 d9 fe",
        "x87 sine core",
    )
    require_bytes(
        image,
        0x00419378,
        "8b 85 d0 fd ff ff 50 e8 dc fc fe ff d9 9d cc fd ff ff",
        "RunEcl opcode 32 sinf call/store",
    )
    require_bytes(
        image,
        0x004193E0,
        "8b 95 c8 fd ff ff 52 e8 54 f9 fe ff d9 9d c4 fd ff ff",
        "RunEcl opcode 33 cosf call/store",
    )
    require_bytes(
        image,
        0x0040C26D,
        "8b 55 f4 8b 42 10 50 e8 e7 cd ff ff 8b 4d f4 d8 49 08 "
        "8b 55 f4 d8 42 20 8b 45 f4 d9 58 14",
        "Fantasy Orb sine-X/multiply/add/store boundary",
    )
    require_bytes(
        image,
        0x0040C28B,
        "8b 4d f4 8b 51 10 52 e8 a9 ca ff ff 8b 45 f4 d8 48 08 "
        "8b 4d f4 d8 41 24 8b 55 f4 d9 5a 18",
        "Fantasy Orb cosine-Y/multiply/add/store boundary",
    )
    require_bytes(
        image,
        0x0040CB6E,
        "8b 4d f4 8b 51 10 52 e8 e6 c4 ff ff 8b 45 f4 d8 48 08 "
        "8b 4d f4 d8 41 20 8b 55 f4 d9 5a 14",
        "Fantasy Seal sine-X/multiply/add/store boundary",
    )
    require_bytes(
        image,
        0x0040CB8C,
        "8b 45 f4 8b 48 10 51 e8 a8 c1 ff ff 8b 55 f4 d8 4a 08 "
        "8b 45 f4 d8 40 24 8b 4d f4 d9 59 18",
        "Fantasy Seal cosine-Y/multiply/add/store boundary",
    )

    probe_source = r"""
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
extern "C" float cosf(float);
static float FromBits(std::uint32_t bits) {
    float value; std::memcpy(&value, &bits, sizeof(value)); return value;
}
static std::uint32_t Bits(float value) {
    std::uint32_t bits; std::memcpy(&bits, &value, sizeof(bits)); return bits;
}
int main() {
    using Marker = unsigned int (*)();
    Marker marker = reinterpret_cast<Marker>(
        dlsym(RTLD_DEFAULT, "th08_canonical_x87_trig_oracle_version"));
    float (*volatile dynamic_cosf)(float) = &cosf;
    const std::uint32_t frame4667 =
        Bits(dynamic_cosf(FromBits(0x4006b668U)));
    const std::uint32_t frame4674 =
        Bits(dynamic_cosf(FromBits(0xc033725cU)));
    const float frame5219X = static_cast<float>(
        std::sin(static_cast<double>(FromBits(0x40096474U))) *
            static_cast<double>(FromBits(0x422cccceU)) +
        static_cast<double>(FromBits(0x434e48c8U)));
    const float frame5219Y = static_cast<float>(
        std::cos(static_cast<double>(FromBits(0x40096474U))) *
            static_cast<double>(FromBits(0x422cccceU)) +
        static_cast<double>(FromBits(0x43d50555U)));
    std::printf(
        "marker=%u f4667_cos=%08x f4674_cos=%08x "
        "f5219_sin_x=%08x f5219_cos_y=%08x\n",
        marker ? marker() : 0U, frame4667, frame4674,
        Bits(frame5219X), Bits(frame5219Y));
    return marker && marker() == 1U &&
                   frame4667 == 0xbf025173U &&
                   frame4674 == 0xbf7189a8U &&
                   Bits(frame5219X) == 0x437283cdU &&
                   Bits(frame5219Y) == 0x43c941b1U
               ? 0
               : 1;
}
"""

    with tempfile.TemporaryDirectory(prefix="th08-x87-trig-") as temporary:
        output = Path(temporary)
        shared = output / "libth08-canonical-x87-trig.so"
        self_test = output / "canonical-x87-trig-self-test"
        probe = output / "preload-probe"
        common = [
            args.cxx,
            "-std=c++17",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fno-builtin-sinf",
            "-fno-builtin-cosf",
        ]
        run(common + ["-fPIC", "-shared", str(args.source), "-o", str(shared)])
        run(
            common
            + [
                "-DTH08_CANONICAL_X87_TRIG_SELF_TEST=1",
                str(args.source),
                "-o",
                str(self_test),
            ]
        )
        self_result = run([str(self_test)], capture_output=True)
        run(common + ["-x", "c++", "-", "-ldl", "-o", str(probe)], input=probe_source)
        environment = os.environ.copy()
        environment["LD_PRELOAD"] = str(shared)
        preload_result = run([str(probe)], env=environment, capture_output=True)

    print(self_result.stdout.strip())
    print(f"preload: {preload_result.stdout.strip()}")
    print(
        "canonical-ecl-trig: PASS "
        f"target_size={len(image)} target_sha256={digest} byte_checks=10"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
