"""Loader for the Silent Hill Origins PS2 executable.

Wraps the ELF so the analysis scripts do not each re-derive the section table,
and provides the two things that come up constantly: turning an address into a
string, and finding the code that builds a given address out of a lui/addiu
pair (MIPS never stores an address as a word, so a plain pointer search finds
nothing).

Used by docs/EXECUTABLES.md.
"""
import struct

from mips import decode


class Sles:
    def __init__(self, path):
        self.d = open(path, 'rb').read()
        d = self.d
        shoff = struct.unpack_from('<I', d, 0x20)[0]
        shentsize, shnum, shstrndx = struct.unpack_from('<HHH', d, 0x2E)
        nameoff = struct.unpack_from('<8I', d, shoff + shstrndx * shentsize)[4]
        self.sections = {}
        for i in range(shnum):
            nm, st, sf, sa, so, ssz, _, _ = struct.unpack_from(
                '<8I', d, shoff + i * shentsize)
            n = d[nameoff + nm:d.index(b'\0', nameoff + nm)].decode()
            self.sections[n] = (sa, so, ssz)
        self.tsa, self.tso, self.tsz = self.sections['.text']
        self.words = struct.unpack(f'<{self.tsz // 4}I',
                                   d[self.tso:self.tso + self.tsz // 4 * 4])

    # -- addresses ---------------------------------------------------------
    def va2off(self, va):
        for n, (sa, so, ssz) in self.sections.items():
            if sa and sa <= va < sa + ssz and n not in ('.bss', '.sbss'):
                return so + (va - sa)
        return None

    def string(self, va, limit=64):
        for n in ('.rodata', '.data', '.sdata'):
            sa, so, ssz = self.sections[n]
            if sa <= va < sa + ssz:
                o = so + (va - sa)
                e = self.d.find(b'\0', o, o + limit)
                s = self.d[o:e if e > 0 else o + limit]
                if len(s) > 2 and all(32 <= c < 127 for c in s):
                    return s.decode()
        return None

    def find_string(self, text):
        pat = text.encode() + b'\0'
        for n in ('.rodata', '.data', '.sdata'):
            sa, so, ssz = self.sections[n]
            i = self.d.find(pat, so, so + ssz)
            if i >= 0:
                return sa + (i - so)
        return None

    # -- code --------------------------------------------------------------
    def refs_to(self, va):
        """Addresses of the instruction that completes a lui/addiu pair
        producing `va`."""
        hi = ((va + 0x8000) >> 16) & 0xFFFF
        lo = va & 0xFFFF
        out = []
        for i, w in enumerate(self.words):
            if (w >> 26) != 0x0F or (w & 0xFFFF) != hi:
                continue
            rt = (w >> 16) & 0x1F
            for j in range(i + 1, min(i + 12, len(self.words))):
                w2 = self.words[j]
                if ((w2 & 0xFFFF) == lo and (w2 >> 26) in (0x09, 0x0D, 0x23, 0x2B)
                        and ((w2 >> 21) & 0x1F) == rt):
                    out.append(self.tsa + j * 4)
                    break
        return out

    def calls_to(self, target):
        """Addresses of every `jal target`."""
        out = []
        for i, w in enumerate(self.words):
            if (w >> 26) != 3:
                continue
            pc = self.tsa + i * 4
            if (((pc + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)) == target:
                out.append(pc)
        return out

    def args_at(self, call_va, back=40):
        """Best-effort values of $a0..$a3 at a call site, by replaying the
        preceding instructions. The delay slot counts, so it is included."""
        i = (call_va - self.tsa) // 4
        reg = {0: 0}
        for j in range(max(0, i - back), i + 2):
            v = self.words[j]
            op = v >> 26
            rt, rs, imm = (v >> 16) & 0x1F, (v >> 21) & 0x1F, v & 0xFFFF
            s = imm - 0x10000 if imm & 0x8000 else imm
            if op == 0x0F:
                reg[rt] = (imm << 16) & 0xFFFFFFFF
            elif op in (0x09, 0x19):
                reg[rt] = (reg.get(rs, 0) + s) & 0xFFFFFFFF
            elif op == 0x0D:
                reg[rt] = (reg.get(rs, 0) | imm) & 0xFFFFFFFF
            elif op == 0:
                fn, rd = v & 0x3F, (v >> 11) & 0x1F
                if fn in (0x21, 0x25, 0x2D):
                    if rt == 0 and rs in reg:
                        reg[rd] = reg[rs]
                    elif rs == 0 and rt in reg:
                        reg[rd] = reg[rt]
        return [reg.get(4, 0), reg.get(5, 0), reg.get(6, 0), reg.get(7, 0)]

    def dis(self, va, count, back=0):
        """Prints `count` instructions, annotating any address that resolves
        to a string."""
        pc = va - back * 4
        lu = {}
        for _ in range(count):
            off = self.tso + (pc - self.tsa)
            if off < 0 or off + 4 > len(self.d):
                break
            w = struct.unpack_from('<I', self.d, off)[0]
            txt, kind, det = decode(w, pc)
            note = ''
            if kind == 'lui':
                lu[det[0]] = det[1]
            elif kind in ('imm', 'mem') and det and det[1] in lu:
                v = (lu[det[1]] + det[2]) & 0xFFFFFFFF
                s = self.string(v)
                note = f'   ; 0x{v:08X}' + (f' "{s}"' if s else '')
            mark = ' <<<' if pc == va else ''
            print(f'  {pc:08X}  {txt}{note}{mark}')
            pc += 4
