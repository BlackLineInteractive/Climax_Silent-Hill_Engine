#!/usr/bin/env python3
"""
DOL Parser — Wii/GameCube Executable Format
Parses the DOL header and extracts all sections with their VMA/file offsets.

DOL Header layout (all big-endian):
  0x000: text_offset[7]   — file offsets of 7 text sections
  0x01C: data_offset[11]  — file offsets of 11 data sections
  0x048: text_addr[7]     — load addresses
  0x064: data_addr[11]
  0x090: text_size[7]
  0x0AC: data_size[11]
  0x0D8: bss_addr
  0x0DC: bss_size
  0x0E0: entry_point
"""
import struct, sys, os
from dataclasses import dataclass, field
from typing import List, Optional

@dataclass
class DolSection:
    name:      str
    file_off:  int
    vma:       int
    size:      int
    data:      bytes = field(default=b"", repr=False)

    @property
    def vma_end(self): return self.vma + self.size
    @property
    def is_executable(self): return self.name.startswith("text")

@dataclass
class Dol:
    path:        str
    entry:       int
    bss_addr:    int
    bss_size:    int
    sections:    List[DolSection]
    raw:         bytes = field(default=b"", repr=False)

    def section_at_vma(self, addr: int) -> Optional[DolSection]:
        for s in self.sections:
            if s.vma <= addr < s.vma_end:
                return s
        return None

    def read_vma(self, addr: int, size: int) -> bytes:
        s = self.section_at_vma(addr)
        if s is None:
            return b""
        off = addr - s.vma
        return s.data[off:off+size]

    def vma_to_file(self, addr: int) -> int:
        s = self.section_at_vma(addr)
        if s is None:
            return -1
        return s.file_off + (addr - s.vma)

    def print_info(self):
        print(f"DOL:       {self.path}")
        print(f"Entry:     0x{self.entry:08X}")
        print(f"BSS:       0x{self.bss_addr:08X}  ({self.bss_size:#x} bytes)")
        print()
        print(f"{'Section':<12} {'FileOff':>10} {'VMA':>10} {'Size':>10}  Flags")
        print("-" * 60)
        for s in self.sections:
            flags = "rx" if s.is_executable else "rw"
            print(f"{s.name:<12} {s.file_off:#010x} {s.vma:#010x} {s.size:#010x}  {flags}")
        total = sum(s.size for s in self.sections) + self.bss_size
        print(f"\nTotal mapped: {total:#x} bytes ({total/1024/1024:.2f} MB)")


def load(path: str) -> Dol:
    with open(path, "rb") as f:
        raw = f.read()

    u32 = lambda off: struct.unpack_from(">I", raw, off)[0]

    text_off  = [u32(0x000 + i*4) for i in range(7)]
    data_off  = [u32(0x01C + i*4) for i in range(11)]
    text_addr = [u32(0x048 + i*4) for i in range(7)]
    data_addr = [u32(0x064 + i*4) for i in range(11)]
    text_size = [u32(0x090 + i*4) for i in range(7)]
    data_size = [u32(0x0AC + i*4) for i in range(11)]
    bss_addr  = u32(0x0D8)
    bss_size  = u32(0x0DC)
    entry     = u32(0x0E0)

    sections = []
    for i in range(7):
        if text_size[i] > 0:
            data = raw[text_off[i] : text_off[i] + text_size[i]]
            sections.append(DolSection(f"text{i}", text_off[i], text_addr[i], text_size[i], data))
    for i in range(11):
        if data_size[i] > 0:
            data = raw[data_off[i] : data_off[i] + data_size[i]]
            sections.append(DolSection(f"data{i}", data_off[i], data_addr[i], data_size[i], data))

    return Dol(path=path, entry=entry, bss_addr=bss_addr, bss_size=bss_size,
               sections=sections, raw=raw)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: dol_parser.py <main.dol>")
        sys.exit(1)
    dol = load(sys.argv[1])
    dol.print_info()
