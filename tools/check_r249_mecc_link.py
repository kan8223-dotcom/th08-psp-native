#!/usr/bin/env python3
"""Reject stale MECC import stubs in the actual linked PSP ELF/embedded PRX."""
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[1]


class Elf32:
    def __init__(self, path):
        self.data = path if isinstance(path, bytes) else path.read_bytes()
        assert self.data[:6] == b'\x7fELF\x01\x01', path
        offset = struct.unpack_from('<I', self.data, 32)[0]
        size, count, names = struct.unpack_from('<HHH', self.data, 46)
        self.sections = [struct.unpack_from('<10I', self.data, offset + i * size)
                         for i in range(count)]
        strings = self.section_data(self.sections[names])
        self.named = {strings[s[0]:].split(b'\0', 1)[0].decode(): s
                      for s in self.sections}

    def section_data(self, section):
        return self.data[section[4]:section[4] + section[5]]

    def at(self, address, count):
        for s in self.sections:
            if s[1] == 1 and s[3] <= address and address + count <= s[3] + s[5]:
                offset = s[4] + address - s[3]
                return self.data[offset:offset + count]
        raise AssertionError(f'unmapped ELF address: {address:08x}+{count}')

    def libraries(self, section):
        data = self.section_data(self.named[section])
        result = {}
        offset = 0
        while offset < len(data):
            name_address, version, attributes, words, variables, functions, nids = \
                struct.unpack_from('<IHHBBHI', data, offset)
            assert words >= 4, (section, offset)
            if name_address:
                name = bytearray()
                while self.at(name_address + len(name), 1) != b'\0':
                    name.extend(self.at(name_address + len(name), 1))
                result[name.decode()] = struct.unpack(
                    '<' + 'I' * (variables + functions), self.at(nids, 4 * (variables + functions)))
            offset += words * 4
        return result


def main():
    elf = Elf32(ROOT / 'TH08PSP.elf')
    prx = Elf32(ROOT / 'build/psp-generated/mecc/kernel/kcall.prx')
    imports = elf.libraries('.lib.stub')
    exports = prx.libraries('.lib.ent')
    name = 'th08audio_kcall'
    assert name in imports and name in exports, (imports.keys(), exports.keys())
    assert 'kcall' not in imports and 'kcall' not in exports, 'stale MECC library'
    assert set(imports[name]) == set(exports[name]) and len(imports[name]) == 3
    # Check the local ELF and the stripped ELF actually packed into the PBP.
    assert prx.data in elf.data, 'linked embedded PRX differs from generated PRX'
    pbp = (ROOT / 'EBOOT.PBP').read_bytes()
    assert pbp[:4] == b'\0PBP'
    begin, end = struct.unpack_from('<II', pbp, 32)
    packed = Elf32(pbp[begin:end])
    assert packed.libraries('.lib.stub') == imports, 'packed EBOOT imports differ'
    assert prx.data in packed.data, 'packed EBOOT embedded PRX differs'
    print('MECC link PASS: ELF/PBP th08audio_kcall import/export NIDs match; exact PRX embedded')


if __name__ == '__main__':
    main()
