"""Minimal MIPS / R5900 disassembler for reading the PS2 executable.

capstone is not usable here. In CS_MODE_MIPS32 it decodes the R5900 as
MIPS-SIMD and invents instructions that do not exist (`aver_u.h`, `addu.qb`
where the real one is a 128-bit load), and in every mode it aborts the whole
stream at the first instruction it does not recognise -- which on the PS2 means
the first `lq`/`sq`, and those are everywhere in compiler output.

This covers the subset needed to follow control flow and reconstruct arguments,
and it never stops early: anything unknown decodes to a placeholder and the
walk continues.

Used by docs/EXECUTABLES.md.
"""

R = ['zero', 'at', 'v0', 'v1', 'a0', 'a1', 'a2', 'a3', 't0', 't1', 't2', 't3',
     't4', 't5', 't6', 't7', 's0', 's1', 's2', 's3', 's4', 's5', 's6', 's7',
     't8', 't9', 'k0', 'k1', 'gp', 'sp', 'fp', 'ra']

I_OPS = {0x08: 'addi', 0x09: 'addiu', 0x0A: 'slti', 0x0B: 'sltiu',
         0x0C: 'andi', 0x0D: 'ori', 0x0E: 'xori', 0x18: 'daddi',
         0x19: 'daddiu'}

M_OPS = {0x20: 'lb', 0x21: 'lh', 0x23: 'lw', 0x24: 'lbu', 0x25: 'lhu',
         0x27: 'lwu', 0x28: 'sb', 0x29: 'sh', 0x2B: 'sw', 0x37: 'ld',
         0x3F: 'sd', 0x1E: 'lq', 0x1F: 'sq',
         # Unaligned access. The compiler leans on these constantly -- copying a
         # 16-byte GUID is two ldl/ldr pairs and two sdl/sdr pairs -- so leaving
         # them undecoded makes ordinary code unreadable.
         0x22: 'lwl', 0x26: 'lwr', 0x2A: 'swl', 0x2E: 'swr',
         0x1A: 'ldl', 0x1B: 'ldr', 0x2C: 'sdl', 0x2D: 'sdr'}

SPECIAL = {0x20: 'add', 0x21: 'addu', 0x22: 'sub', 0x23: 'subu', 0x24: 'and',
           0x25: 'or', 0x26: 'xor', 0x27: 'nor', 0x2A: 'slt', 0x2B: 'sltu',
           0x2C: 'dadd', 0x2D: 'daddu', 0x00: 'sll', 0x02: 'srl', 0x03: 'sra',
           0x04: 'sllv', 0x06: 'srlv', 0x07: 'srav', 0x18: 'mult',
           0x19: 'multu', 0x1A: 'div', 0x1B: 'divu', 0x10: 'mfhi',
           0x12: 'mflo', 0x08: 'jr', 0x09: 'jalr', 0x0C: 'syscall'}


def signed(v):
    return v - 0x10000 if v & 0x8000 else v


def decode(w, pc):
    """(text, kind, detail).

    kind is 'lui', 'imm', 'mem', 'branch', 'jump', 'reg' or 'other'; detail
    carries the operands a caller needs to track register values.
    """
    op = w >> 26
    rs, rt = (w >> 21) & 0x1F, (w >> 16) & 0x1F
    rd, sa, fn = (w >> 11) & 0x1F, (w >> 6) & 0x1F, w & 0x3F
    imm = w & 0xFFFF
    s = signed(imm)

    if w == 0:
        return 'nop', 'other', None
    if op == 0x0F:
        return f'lui     ${R[rt]}, 0x{imm:04x}', 'lui', (rt, imm << 16)
    if op in I_OPS:
        # andi/ori/xori zero-extend their immediate; the arithmetic ones sign-
        # extend it. Printing a mask of 0xffff as -0x1 makes register masking
        # unreadable, which matters because the engine masks constantly.
        v = imm if op in (0x0C, 0x0D, 0x0E) else s
        return f'{I_OPS[op]:<7s} ${R[rt]}, ${R[rs]}, {v:#x}', 'imm', (rt, rs, v)
    if op in M_OPS:
        return f'{M_OPS[op]:<7s} ${R[rt]}, {s:#x}(${R[rs]})', 'mem', (rt, rs, s)
    if op in (0x04, 0x05, 0x14, 0x15):
        # 0x14/0x15 are the "likely" forms. Leaving them undecoded hid a whole
        # attribute dispatch in Camera::CBaseCamera::HandleAttributes, where the
        # property index is tested with BEQL and the FOV case is only reachable
        # through one of them.
        t = pc + 4 + s * 4
        n = {0x04: 'beq', 0x05: 'bne', 0x14: 'beql', 0x15: 'bnel'}[op]
        return f'{n:<7s} ${R[rs]}, ${R[rt]}, 0x{t:08x}', 'branch', t
    if op in (0x06, 0x07, 0x16, 0x17):
        t = pc + 4 + s * 4
        n = {0x06: 'blez', 0x07: 'bgtz', 0x16: 'blezl', 0x17: 'bgtzl'}[op]
        return f'{n:<7s} ${R[rs]}, 0x{t:08x}', 'branch', t
    if op == 0x01:
        t = pc + 4 + s * 4
        n = {0: 'bltz', 1: 'bgez', 16: 'bltzal', 17: 'bgezal'}.get(rt, f'regimm{rt}')
        return f'{n:<7s} ${R[rs]}, 0x{t:08x}', 'branch', t
    if op in (0x02, 0x03):
        t = ((pc + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
        n = 'j' if op == 0x02 else 'jal'
        return f'{n:<7s} 0x{t:08x}', 'jump', t
    if op == 0x00:
        if fn == 0x08:
            return f'jr      ${R[rs]}', 'jump', None
        if fn == 0x09:
            return f'jalr    ${R[rd]}, ${R[rs]}', 'jump', None
        if fn in (0x00, 0x02, 0x03):
            return f'{SPECIAL[fn]:<7s} ${R[rd]}, ${R[rt]}, {sa}', 'reg', (rd, rt, sa)
        if fn in (0x10, 0x12):
            return f'{SPECIAL[fn]:<7s} ${R[rd]}', 'reg', None
        if fn in SPECIAL:
            if fn in (0x21, 0x25, 0x2D) and rt == 0:
                return f'move    ${R[rd]}, ${R[rs]}', 'reg', (rd, rs, 0)
            return f'{SPECIAL[fn]:<7s} ${R[rd]}, ${R[rs]}, ${R[rt]}', 'reg', (rd, rs, rt)
        return f'special.{fn:#04x}', 'other', None
    if op == 0x11:
        # COP1. The moves matter: a float constant is built in a GPR and handed
        # to a coprocessor register with mtc1, so rendering the whole of COP1 as
        # an opaque `cop1.0x04` loses which register received the value -- and
        # with it every constructor default.
        if rs == 0x00:
            return f'mfc1   ${R[rt]}, $f{rd}', 'fpmove', (rt, rd)
        if rs == 0x04:
            return f'mtc1   ${R[rt]}, $f{rd}', 'mtc1', (rd, rt)
        return f'cop1.{rs:#04x}', 'other', None
    if op in (0x31, 0x39):
        n = 'lwc1' if op == 0x31 else 'swc1'
        return f'{n:<7s} $f{rt}, {s:#x}(${R[rs]})', 'mem', None
    if op == 0x1C:
        return f'mmi.{fn:#04x}', 'other', None
    return f'op{op:#04x}.{w:08x}', 'other', None
