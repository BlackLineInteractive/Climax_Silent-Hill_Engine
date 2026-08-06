"""Recovers the property table of every game-object class from the executable.

A container's 0x0704 record is `[u32 size][u32 id][payload]`, and the engine
dispatches on the low bits of `id`:

    id = record[1];
    if (id >= N) return;
    goto jumpTable[id];          // one case per property

Each case stores the payload into a field of the object, and the store tells us
the type: `swc1` is a float, `sw` an int, `sb` a byte. Walking the table turns
"property 3 of CStaticCamera" from a guess into a field offset and a type.

Run against Ghost Rider: it ships unstripped, so every HandleAttributes has an
exact name, address and size, and the classes are shared with Silent Hill.

    python3 tools/attrmap.py game/GR/SLES_543.17 [--json out.json]
"""
import json
import struct
import sys

sys.path.insert(0, __file__.rsplit('/', 1)[0])
from mips import decode


class Elf:
    def __init__(self, path):
        self.d = open(path, 'rb').read()
        d = self.d
        shoff = struct.unpack_from('<I', d, 0x20)[0]
        shentsize, shnum, shstrndx = struct.unpack_from('<HHH', d, 0x2E)
        nameoff = struct.unpack_from('<8I', d, shoff + shstrndx * shentsize)[4]
        self.sec = {}
        for i in range(shnum):
            nm, st, sf, sa, so, ssz, lk, inf = struct.unpack_from(
                '<8I', d, shoff + i * shentsize)
            n = d[nameoff + nm:d.index(b'\0', nameoff + nm)].decode()
            self.sec[n] = (sa, so, ssz)
        self.tsa, self.tso, self.tsz = self.sec['.text']

    def word(self, va):
        for n, (sa, so, ssz) in self.sec.items():
            if sa and sa <= va < sa + ssz and n not in ('.bss', '.sbss'):
                return struct.unpack_from('<I', self.d, so + (va - sa))[0]
        return None

    def insn(self, va):
        off = self.tso + (va - self.tsa)
        if va < self.tsa or off + 4 > len(self.d):
            return None
        return struct.unpack_from('<I', self.d, off)[0]

    def symbols(self):
        if '.symtab' not in self.sec:
            return {}
        sa, so, ssz = self.sec['.symtab']
        stro = self.sec['.strtab'][1]
        out = {}
        for i in range(ssz // 16):
            nmoff, val, size, info, other, shndx = struct.unpack_from(
                '<IIIBBH', self.d, so + i * 16)
            n = self.d[stro + nmoff:self.d.index(b'\0', stro + nmoff)].decode('latin1')
            if n:
                out[n] = (val, size)
        return out


def demangle_class(sym):
    """HandleAttributes__14CPhysicsObjectRCQ23RWS16CAttributePacket -> CPhysicsObject"""
    body = sym.split('__', 1)[1] if '__' in sym else sym
    if body.startswith('C'):
        body = body[1:]                       # leading 'C' = const method
    i = 0
    while i < len(body) and body[i].isdigit():
        i += 1
    if i == 0:
        return sym
    n = int(body[:i])
    return body[i:i + n]


def find_table(elf, start, size):
    """Locates the `sltiu` bound and the jump-table base.

    The dispatch the compiler emits is always the same shape:

        sll   $idx, $id, 2
        lui   $b, HI ; addiu $b, $b, LO      <- table base
        addu  $idx, $idx, $b
        lw    $t, 0($idx)
        jr    $t

    so the base is found by walking back from the `jr`, rather than by guessing
    which tracked register looks biggest.
    """
    regs = {}
    shifted = set()
    count = None
    last_lw = None                  # (destReg, baseReg)
    last_addu = None                # (destReg, srcA, srcB)

    for va in range(start, start + size, 4):
        w = elf.insn(va)
        if w is None:
            break
        txt, kind, det = decode(w, va)
        op = txt.split()[0]
        rt, rs = (w >> 16) & 0x1F, (w >> 21) & 0x1F
        rd = (w >> 11) & 0x1F

        if kind == 'lui':
            regs[det[0]] = det[1]
        elif kind == 'imm' and det[1] in regs:
            regs[det[0]] = (regs[det[1]] + det[2]) & 0xFFFFFFFF
        elif op == 'sll':
            shifted.add(rd)
        elif op == 'addu':
            last_addu = (rd, rs, rt)
        elif op == 'lw':
            last_lw = (rt, rs)
        elif op == 'sltiu':
            try:
                count = int(txt.rsplit(',', 1)[1].strip(), 16)
            except ValueError:
                count = None
        elif op == 'jr' and count and last_lw and last_addu:
            dst, base = last_lw
            if last_addu[0] == base:
                a, b = last_addu[1], last_addu[2]
                cand = b if a in shifted else a
                if cand in regs and 2 <= count <= 256:
                    return count, regs[cand]
    return None, None


def object_register(elf, start, size):
    """The register the object pointer is parked in (`move $sX, $a0`)."""
    for va in range(start, min(start + 40, start + size), 4):
        w = elf.insn(va)
        if w is None:
            break
        txt, kind, det = decode(w, va)
        if txt.startswith('move') and txt.endswith('$a0'):
            return txt.split()[1].rstrip(',')
    return '$s1'


TYPES = {'swc1': 'float', 'sw': 'int', 'sh': 'short', 'sb': 'byte',
         'sd': 'long', 'sq': 'vector'}


def read_case(elf, va, objreg, limit=24):
    """Stores this case makes into the object."""
    out = []
    for k in range(limit):
        addr = va + k * 4
        w = elf.insn(addr)
        if w is None:
            break
        txt, kind, det = decode(w, addr)
        op = txt.split()[0]
        if op in TYPES and txt.endswith(f'({objreg})'):
            off = txt.split(',')[1].split('(')[0].strip()
            out.append({'offset': off, 'type': TYPES[op]})
        if op in ('jr', 'j'):
            break
        if op in ('beq', 'b') and '$zero, $zero' in txt:
            # the store often sits in the delay slot, so take one more
            nxt = va + (k + 1) * 4
            t2, _, _ = decode(elf.insn(nxt), nxt)
            o2 = t2.split()[0]
            if o2 in TYPES and t2.endswith(f'({objreg})'):
                off = t2.split(',')[1].split('(')[0].strip()
                out.append({'offset': off, 'type': TYPES[o2]})
            break
    return out


def main():
    path = sys.argv[1]
    elf = Elf(path)
    syms = elf.symbols()
    handlers = {n: v for n, v in syms.items() if n.startswith('HandleAttributes__')}
    if not handlers:
        print('no HandleAttributes symbols; this binary is stripped')
        return

    result = {}
    for sym, (va, size) in sorted(handlers.items(), key=lambda kv: kv[1][0]):
        if not size:
            continue
        cls = demangle_class(sym)
        count, table = find_table(elf, va, size)
        if not count or not table:
            result[cls] = {'address': f'0x{va:08X}', 'properties': None,
                           'note': 'no dispatch table found'}
            continue
        objreg = object_register(elf, va, size)
        props = []
        for i in range(count):
            tgt = elf.word(table + i * 4)
            props.append({'index': i,
                          'case': f'0x{tgt:08X}' if tgt else None,
                          'stores': read_case(elf, tgt, objreg) if tgt else []})
        result[cls] = {'address': f'0x{va:08X}', 'table': f'0x{table:08X}',
                       'count': count, 'objectRegister': objreg,
                       'properties': props}

    named = sum(1 for v in result.values() if v.get('properties'))
    print(f'{len(result)} classes, {named} with a recovered table\n')
    for cls, v in sorted(result.items()):
        if not v.get('properties'):
            print(f'{cls:34s} {v["address"]}  -- {v.get("note","")}')
            continue
        print(f'{cls:34s} {v["address"]}  {v["count"]} properties')
        for p in v['properties']:
            s = ', '.join(f'+{x["offset"]} {x["type"]}' for x in p['stores'])
            print(f'    {p["index"]:3d}  {s or "(no direct store)"}')

    if '--json' in sys.argv:
        out = sys.argv[sys.argv.index('--json') + 1]
        json.dump(result, open(out, 'w'), indent=1)
        print(f'\nwrote {out}')


if __name__ == '__main__':
    main()
