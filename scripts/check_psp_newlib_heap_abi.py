#!/usr/bin/env python3
"""Reject PSP ELFs that do not match the audited newlib 4.5 dlmalloc ABI."""

import argparse
import re
import subprocess
import sys


def run(command: list[str]) -> str:
    completed = subprocess.run(command, check=False, text=True, capture_output=True)
    if completed.returncode != 0:
        detail = completed.stderr.strip() or completed.stdout.strip()
        raise RuntimeError(f"{' '.join(command)} failed: {detail}")
    return completed.stdout


def read_symbols(nm: str, elf: str) -> dict[str, tuple[int, str]]:
    symbols: dict[str, tuple[int, str]] = {}
    for line in run([nm, "-S", elf]).splitlines():
        fields = line.split()
        if len(fields) != 4:
            continue
        _, size_hex, symbol_type, name = fields
        try:
            size = int(size_hex, 16)
        except ValueError:
            continue
        symbols[name] = (size, symbol_type)
    return symbols


def require_symbol(symbols: dict[str, tuple[int, str]], name: str, size: int,
                   allowed_types: str) -> None:
    actual = symbols.get(name)
    if actual is None:
        raise RuntimeError(f"missing required ELF symbol {name}")
    actual_size, actual_type = actual
    if actual_size != size or actual_type not in allowed_types:
        raise RuntimeError(
            f"{name}: expected size=0x{size:x} type={allowed_types}, "
            f"got size=0x{actual_size:x} type={actual_type}"
        )


def require_pattern(text: str, pattern: str, description: str) -> None:
    if re.search(pattern, text, re.MULTILINE) is None:
        raise RuntimeError(f"ELF does not exhibit audited {description}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("elf")
    parser.add_argument("--nm", default="psp-nm")
    parser.add_argument("--objdump", default="psp-objdump")
    args = parser.parse_args()

    try:
        file_header = run([args.objdump, "-f", args.elf])
        require_pattern(file_header, r"file format elf32-littlemips", "ELF format")
        require_pattern(file_header, r"architecture: mips:allegrex", "CPU architecture")

        symbols = read_symbols(args.nm, args.elf)
        require_symbol(symbols, "__malloc_av_", 0x408, "Dd")
        require_symbol(symbols, "__malloc_sbrk_base", 0x4, "Dd")
        require_symbol(symbols, "__malloc_current_mallinfo", 0x28, "Bb")
        require_symbol(symbols, "__malloc_lock", 0xC, "Tt")
        require_symbol(symbols, "__malloc_unlock", 0xC, "Tt")
        if "_malloc_r" not in symbols:
            raise RuntimeError("missing required ELF symbol _malloc_r")

        malloc_code = run([args.objdump, "-dr", "--disassemble=_malloc_r", args.elf])
        require_pattern(malloc_code, r"\baddiu\s+v0,a1,19\b", "request + 19 rounding")
        require_pattern(malloc_code, r"\bins\s+a2,zero,0x0,0x4\b",
                        "16-byte request alignment")
        require_pattern(malloc_code, r"\bsltiu?\s+\w+,\w+,16\b", "16-byte MINSIZE")

        lock_code = run([args.objdump, "-dr", "--disassemble=__malloc_lock", args.elf])
        unlock_code = run([args.objdump, "-dr", "--disassemble=__malloc_unlock", args.elf])
        require_pattern(lock_code, r"__retarget_lock_acquire_recursive", "recursive malloc lock")
        require_pattern(unlock_code, r"__retarget_lock_release_recursive", "recursive malloc unlock")
    except RuntimeError as error:
        print(f"PSP newlib heap ABI check failed: {error}", file=sys.stderr)
        return 1

    print("PSP newlib heap ABI: PASS "
          "(newlib 4.5 classic dlmalloc, av=0x408, align=16, MINSIZE=16)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
