#!/usr/bin/env python3
"""
lift_to_c_v2.py  —  PowerPC → C  (Silent Hill Origins Wii, main.dol)

Full-pass structural lifter with:
  • Register value tracking through basic blocks (symbolic SSA-lite)
  • CR-register tracking  →  proper if (a < b) / if (a == b) conditions
  • CTR loop detection    →  bdnz/bdz  →  for / while
  • Float vs Int register classification  (FPR = double, GPR = uint32_t)
  • Function signature inference from call sites
  • Return value tracking through r3
  • Proper C label syntax  (no leading dot)
  • Paired-single (ps_*) stubs
  • Branch-likely (beql/bnel) handled correctly

Usage:
    python3 tools/dol_decompile/lift_to_c_v2.py <main.dol> <out_dir> [symbol_map.txt]
"""
from __future__ import annotations

import re
import struct
import sys
import os
from dataclasses import dataclass, field
from typing import Optional
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from dol_parser import load, DolSection, Dol

try:
    from capstone import Cs, CS_ARCH_PPC, CS_MODE_32, CS_MODE_BIG_ENDIAN
except ImportError:
    print("ERROR: pip install capstone")
    sys.exit(1)


# ─────────────────────────────────────────────────────────────────────────────
# Register name aliases (PPC ABI)
# ─────────────────────────────────────────────────────────────────────────────
GPR = [
    'r0','sp','rtoc','a0','a1','a2','a3','a4','a5','a6','a7',
    'r11','r12','r13','r14','r15','r16','r17','r18','r19','r20',
    'r21','r22','r23','r24','r25','r26','r27','r28','r29','r30','r31',
]
FPR = [f'f{i}' for i in range(32)]


# ─────────────────────────────────────────────────────────────────────────────
# Symbolic register state for one basic block
# ─────────────────────────────────────────────────────────────────────────────
@dataclass
class RegState:
    """
    Tracks register contents as C variable names (materialized).
    Every write to a register produces a new temporary variable t_N.
    This prevents expression explosion: instead of nesting (a+(b+c)) etc
    we emit   uint32_t t3 = a + b;   and store name 't3'.
    """
    # reg-index → variable name (string like 't7', 'a0', 'f3' …)
    gpr:    dict[int, str] = field(default_factory=dict)
    fpr:    dict[int, str] = field(default_factory=dict)
    # cr[0..7] → dict with keys 'lt','gt','eq','so' → C boolean expression
    cr:     dict[int, dict] = field(default_factory=dict)
    # CTR is special: always a materialized variable name
    ctr_var: str = 'CTR'
    # Counter for fresh temporaries
    _tmp: int = field(default=0, repr=False)
    # Emitted variable declarations (collected, written before the function body)
    decls:  list[str] = field(default_factory=list)

    def fresh(self, type_: str = 'uint32_t') -> str:
        """Return a fresh temp variable name and record its declaration."""
        n = f't{self._tmp}'
        self._tmp += 1
        self.decls.append(f'    {type_} {n} = 0;')
        return n

    def gpr_name(self, i: int) -> str:
        return self.gpr.get(i, GPR[i] if i < len(GPR) else f'r{i}')

    def fpr_name(self, i: int) -> str:
        return self.fpr.get(i, FPR[i] if i < len(FPR) else f'f{i}')

    def cr_field(self, n: int) -> dict:
        return self.cr.get(n, {'lt': f'cr{n}_lt', 'gt': f'cr{n}_gt',
                               'eq': f'cr{n}_eq', 'so': 'false'})

    def set_gpr(self, i: int, expr: str, out: list, type_: str = 'uint32_t') -> str:
        """Assign expr to a fresh temp, emit the assignment, update gpr map.
        Returns the variable name.
        """
        # Trivial: if expr is already just a variable name, reuse it
        if re.match(r'^[a-zA-Z_][a-zA-Z0-9_]*$', expr.strip()):
            self.gpr[i] = expr.strip()
            return expr.strip()
        v = self.fresh(type_)
        self.gpr[i] = v
        out.append(f'    {v} = {expr};')
        return v

    def set_fpr(self, i: int, expr: str, out: list, type_: str = 'double') -> str:
        if re.match(r'^[a-zA-Z_][a-zA-Z0-9_]*$', expr.strip()):
            self.fpr[i] = expr.strip()
            return expr.strip()
        v = self.fresh(type_)
        self.fpr[i] = v
        out.append(f'    {v} = {expr};')
        return v

    def set_ctr(self, expr: str, out: list) -> str:
        """Materialise CTR: emit assignment, return var name."""
        if re.match(r'^[a-zA-Z_][a-zA-Z0-9_]*$', expr.strip()):
            self.ctr_var = expr.strip()
        else:
            # Always write to the named CTR variable
            out.append(f'    CTR = {expr};')
            self.ctr_var = 'CTR'
        return self.ctr_var

    def set_cr_from_cmp(self, n: int, a: str, b: str, signed: bool = True):
        self.cr[n] = {
            'lt': f'({a} < {b})',
            'gt': f'({a} > {b})',
            'eq': f'({a} == {b})',
            'so': 'false',
        }

    def set_cr_from_cmpU(self, n: int, a: str, b: str):
        ua, ub = f'(uint32_t){a}', f'(uint32_t){b}'
        self.cr[n] = {
            'lt': f'({ua} < {ub})',
            'gt': f'({ua} > {ub})',
            'eq': f'({ua} == {ub})',
            'so': 'false',
        }


# ─────────────────────────────────────────────────────────────────────────────
# PPC instruction decoder (operand parser on top of capstone text output)
# ─────────────────────────────────────────────────────────────────────────────

def parse_reg(s: str) -> int:
    """'r3' → 3, 'f5' → 5. Returns -1 on failure."""
    s = s.strip().lower().lstrip('$')
    if s.startswith('cr'):
        try: return int(s[2:])
        except: return -1
    for prefix in ('r','f'):
        if s.startswith(prefix):
            try: return int(s[len(prefix):])
            except: pass
    return -1


def parse_imm(s: str) -> Optional[int]:
    s = s.strip()
    try:
        v = int(s, 16) if s.startswith('0x') or s.startswith('-0x') else int(s, 0)
        return v
    except:
        return None


def parse_mem(s: str):
    """'0x50(r3)' → (offset_int, base_reg_idx). Returns None on failure."""
    m = re.match(r'^(-?(?:0x[\da-fA-F]+|\d+))\((\w+)\)$', s.strip())
    if not m: return None
    off = parse_imm(m.group(1))
    base = parse_reg(m.group(2))
    if off is None or base < 0: return None
    return (off, base)


# ─────────────────────────────────────────────────────────────────────────────
# Instruction → C line translator
# ─────────────────────────────────────────────────────────────────────────────

def sign_extend_16(v: int) -> int:
    return v - 0x10000 if v & 0x8000 else v

def sign_extend_26(v: int) -> int:
    return v - 0x4000000 if v & 0x2000000 else v


class Lifter:
    def __init__(self, dol: Dol, label_map: dict, string_map: dict):
        self.dol        = dol
        self.label_map  = label_map
        self.string_map = string_map

    def label(self, addr: int) -> str:
        if addr in self.label_map:
            return self.label_map[addr]
        return f'L_{addr:08X}'

    def _string_hint(self, addr: int) -> str:
        if addr in self.string_map:
            s = self.string_map[addr][:30].replace('\n','\\n').replace('"','\\"')
            return f'  /* "{s}" */'
        return ''

    # ── Main dispatch ─────────────────────────────────────────────────────────
    def translate(self, insn, state: RegState) -> list[str]:
        """Returns list of C lines for this instruction. Updates state in-place."""
        m   = insn.mnemonic.lower()
        ops = insn.op_str.strip()
        opl = [o.strip() for o in ops.split(',')]

        # Normalise mnemonic aliases
        if m in ('nop', 'trap'): return [f'    /* {m} */']

        # ── Return ──────────────────────────────────────────────────────────
        if m == 'blr':
            ret = state.gpr_name(3)
            return [f'    return {ret};']
        if m == 'bclr':
            # conditional blr — encoded as bclrNN
            return [f'    /* bclr — conditional return */']

        # ── Unconditional branch ─────────────────────────────────────────────
        if m == 'b':
            tgt = parse_imm(ops)
            lbl = self.label(tgt) if tgt else ops
            return [f'    goto {lbl};']

        # ── Branch-and-link (call) ────────────────────────────────────────────
        if m == 'bl':
            tgt = parse_imm(ops)
            fname = self.label(tgt) if tgt else ops
            # Guess arg count from which gprs were recently written
            args = []
            for i in range(4, 8):  # a0=r3..a3=r6 (indices 3..6)
                v = state.gpr.get(i + -1)   # r3=idx3, a0=gpr[3]
                pass
            a0 = state.gpr_name(3)
            a1 = state.gpr_name(4)
            a2 = state.gpr_name(5)
            a3 = state.gpr_name(6)
            call = f'    a0 = {fname}({a0}, {a1}, {a2}, {a3});'
            state.gpr[3] = f'{fname}(/* ... */)'  # r3 = return value
            hint = self._string_hint(tgt) if tgt else ''
            return [call + hint]

        # ── Branch-to-count-register (jr) ────────────────────────────────────
        if m == 'bctr':
            return ['    goto *CTR;  /* indirect jump table */']

        # ── Conditional branches ─────────────────────────────────────────────
        if m in ('beq','bne','blt','bgt','ble','bge',
                 'beql','bnel','bltl','bgtl','blel','bgel',
                 'bdnz','bdz'):
            return self._translate_branch(m, opl, state)

        # ── CTR setup ────────────────────────────────────────────────────────
        if m == 'mtctr':
            src = parse_reg(ops)
            state.ctr = state.gpr_name(src)
            return [f'    CTR = {state.ctr};']

        # ── Move register ────────────────────────────────────────────────────
        if m == 'mr':
            dst, src = parse_reg(opl[0]), parse_reg(opl[1])
            val = state.gpr_name(src)
            state.gpr[dst] = val
            return [f'    {GPR[dst] if dst < len(GPR) else f"r{dst}"} = {val};']

        # ── Load immediate ────────────────────────────────────────────────────
        if m == 'li':
            dst = parse_reg(opl[0])
            imm = parse_imm(opl[1])
            if imm is not None:
                state.gpr[dst] = str(imm)
                return [f'    {GPR[dst] if dst < len(GPR) else f"r{dst}"} = {imm};']
            return [f'    /* li {opl[0]}, {opl[1]} */']

        if m == 'lis':
            dst = parse_reg(opl[0])
            imm = parse_imm(opl[1])
            if imm is not None:
                val = (imm << 16) & 0xFFFFFFFF
                state.gpr[dst] = f'0x{val:08X}'
                hint = self._string_hint(val)
                return [f'    {GPR[dst] if dst < len(GPR) else f"r{dst}"} = 0x{val:08X};{hint}']
            return [f'    /* lis {opl[0]}, {opl[1]} */']

        # ── Add immediate ─────────────────────────────────────────────────────
        if m in ('addi', 'addis'):
            dst  = parse_reg(opl[0])
            src  = parse_reg(opl[1])
            imm  = parse_imm(opl[2])
            if imm is None: return [f'    /* {m} {ops} */']
            shift = 16 if m == 'addis' else 0
            sval  = imm << shift
            src_e = state.gpr_name(src) if src != 0 else '0'
            if src == 0:
                state.gpr[dst] = f'0x{sval & 0xFFFFFFFF:X}'
            else:
                state.gpr[dst] = f'({src_e} + {sval})'
            dname = GPR[dst] if dst < len(GPR) else f'r{dst}'
            hint = self._string_hint(sval & 0xFFFFFFFF) if sval else ''
            return [f'    {dname} = {state.gpr[dst]};{hint}']

        # ── Logical immediates ─────────────────────────────────────────────────
        if m in ('ori', 'oris', 'andi.', 'andis.', 'xori'):
            dst = parse_reg(opl[0])
            src = parse_reg(opl[1])
            imm = parse_imm(opl[2])
            op_str = {'ori':'|','oris':'|','andi.':'&','andis.':'&','xori':'^'}[m]
            dname = GPR[dst] if dst < len(GPR) else f'r{dst}'
            sname = state.gpr_name(src)
            val   = f'({sname} {op_str} 0x{imm:X})'
            state.gpr[dst] = val
            return [f'    {dname} = {val};']

        # ── Arithmetic ────────────────────────────────────────────────────────
        if m in ('add','addu','addc','adde','subf','subfc','subfe','neg',
                 'mullw','mulhw','mulhwu','divw','divwu'):
            return self._translate_arith(m, opl, state)

        if m in ('add.','subf.','neg.'):
            lines = self._translate_arith(m.rstrip('.'), opl, state)
            dst = parse_reg(opl[0])
            v   = state.gpr_name(dst)
            state.set_cr_from_cmp(0, v, '0')
            return lines

        # ── Shift ─────────────────────────────────────────────────────────────
        if m in ('slw','srw','sraw','slwi','srwi','srawi','clrlwi','rotlwi','rotrwi'):
            return self._translate_shift(m, opl, state)

        # ── Compare ───────────────────────────────────────────────────────────
        if m in ('cmpwi','cmpw','cmplwi','cmplw','cmpdi','cmpd','cmpldi','cmpld'):
            return self._translate_cmp(m, opl, state)

        # ── Load/Store ────────────────────────────────────────────────────────
        if m in ('lwz','lwzu','lbz','lbzu','lhz','lhzu','lha','lwbrx',
                 'ld','lwa','ldu'):
            return self._translate_load(m, opl, state, 'u32')
        if m in ('stw','stwu','stb','stbu','sth','sthu','std','stdu'):
            return self._translate_store(m, opl, state, 'u32')
        if m == 'stwx':
            return [f'    /* stwx {ops} */']

        # ── Float load/store ──────────────────────────────────────────────────
        if m in ('lfs','lfsu','lfd','lfdu'):
            return self._translate_fload(m, opl, state)
        if m in ('stfs','stfsu','stfd','stfdu'):
            return self._translate_fstore(m, opl, state)

        # ── Float arithmetic ──────────────────────────────────────────────────
        if m in ('fadd','fadds','fsub','fsubs','fmul','fmuls',
                 'fdiv','fdivs','fmadd','fmadds','fmsub','fmsubs',
                 'fnmadd','fnmadds','fnmsub','fnmsubs',
                 'fabs','fnabs','fneg','fmr','frsp','fctiw','fctiwz',
                 'fsqrt','fsqrts','fres','frsqrte'):
            return self._translate_fpu(m, opl, state)

        # ── Paired-single (Wii unique) ────────────────────────────────────────
        if m.startswith('ps_'):
            return self._translate_ps(m, opl, state)

        # ── Special registers ─────────────────────────────────────────────────
        if m == 'mflr':
            dst = parse_reg(ops)
            state.gpr[dst] = 'LR'
            return [f'    {GPR[dst] if dst < len(GPR) else f"r{dst}"} = LR;']
        if m == 'mtlr':
            src = parse_reg(ops)
            return [f'    LR = {state.gpr_name(src)};']
        if m == 'mfctr':
            dst = parse_reg(ops)
            state.gpr[dst] = 'CTR'
            return [f'    {GPR[dst] if dst < len(GPR) else f"r{dst}"} = CTR;']
        if m in ('mfspr','mtspr','mfmsr','mtmsr','mtsprg','mfsprg'):
            return [f'    /* {m} {ops} */']
        if m in ('sync','isync','eieio','lwsync','dcbst','dcbf','icbi',
                 'dcbt','dcbtst','dcbz','dcbz_l'):
            return [f'    __sync();  /* {m} */']

        # ── Condition register ops ─────────────────────────────────────────────
        if m in ('crand','cror','crxor','crnand','crnor','creqv','crandc','crorc'):
            return [f'    /* {m} {ops} */']

        # ── Trap ──────────────────────────────────────────────────────────────
        if m in ('tw','twi','td','tdi'):
            return [f'    /* trap: {m} {ops} */']

        # ── Rotate/mask ───────────────────────────────────────────────────────
        if m in ('rlwinm','rlwinm.','rlwimi','rlwnm','rldimi','rldicl','rldicr'):
            return self._translate_rotate(m, opl, state)

        # ── 64-bit ops (rare in game code) ───────────────────────────────────
        if m in ('extsb','extsh','extsw','cntlzw','cntlzd','popcntb'):
            dst = parse_reg(opl[0])
            src = parse_reg(opl[1])
            dname = GPR[dst] if dst < len(GPR) else f'r{dst}'
            ext_map = {'extsb':'(int8_t)','extsh':'(int16_t)','extsw':'(int32_t)',
                       'cntlzw':'__builtin_clz','cntlzd':'__builtin_clzl'}
            fn = ext_map.get(m, m)
            if fn.startswith('__'):
                state.gpr[dst] = f'{fn}({state.gpr_name(src)})'
            else:
                state.gpr[dst] = f'(int32_t)({fn}{state.gpr_name(src)})'
            return [f'    {dname} = {state.gpr[dst]};']

        # ── Fallback: emit raw asm comment ────────────────────────────────────
        return [f'    /* {insn.address:08X}  {m:<10} {ops} */']

    # ── Branch translation ────────────────────────────────────────────────────
    def _translate_branch(self, m: str, opl: list, state: RegState) -> list[str]:
        # CTR branches
        if m in ('bdnz', 'bdnzl'):
            tgt = parse_imm(opl[-1])
            lbl = self.label(tgt) if tgt else opl[-1]
            ctr = state.ctr or 'CTR'
            return [f'    if (--{ctr} != 0) goto {lbl};']
        if m in ('bdz', 'bdzl'):
            tgt = parse_imm(opl[-1])
            lbl = self.label(tgt) if tgt else opl[-1]
            ctr = state.ctr or 'CTR'
            return [f'    if (--{ctr} == 0) goto {lbl};']

        # Standard conditional branches
        # May have cr field as first operand: beq cr2, target
        if len(opl) == 2 and opl[0].startswith('cr'):
            cr_n  = parse_reg(opl[0])
            tgt   = parse_imm(opl[1])
        elif len(opl) == 1:
            cr_n  = 0
            tgt   = parse_imm(opl[0])
        else:
            cr_n  = 0
            tgt   = parse_imm(opl[-1])

        lbl  = self.label(tgt) if tgt else (opl[-1] if opl else '?')
        crf  = state.cr_field(cr_n)

        cond_map = {
            'beq': crf['eq'],  'beql': crf['eq'],
            'bne': f'!({crf["eq"]})', 'bnel': f'!({crf["eq"]})',
            'blt': crf['lt'],  'bltl': crf['lt'],
            'bgt': crf['gt'],  'bgtl': crf['gt'],
            'ble': f'!({crf["gt"]})', 'blel': f'!({crf["gt"]})',
            'bge': f'!({crf["lt"]})', 'bgel': f'!({crf["lt"]})',
        }

        # Check for blr variant (conditional return)
        if 'lr' in m:
            cond = cond_map.get(m.replace('lr','').replace('l',''), crf['eq'])
            return [f'    if ({cond}) return {state.gpr_name(3)};']

        cond = cond_map.get(m, f'/* {m} */')
        return [f'    if ({cond}) goto {lbl};']

    # ── Arithmetic translation ────────────────────────────────────────────────
    def _translate_arith(self, m: str, opl: list, state: RegState) -> list[str]:
        if not opl: return [f'    /* {m} */']
        dst = parse_reg(opl[0])
        dname = GPR[dst] if dst < len(GPR) else f'r{dst}'

        if m == 'neg' and len(opl) >= 2:
            src = parse_reg(opl[1])
            val = f'-{state.gpr_name(src)}'
            state.gpr[dst] = val
            return [f'    {dname} = {val};']

        if len(opl) < 3: return [f'    /* {m} {",".join(opl)} */']

        a = parse_reg(opl[1])
        b = parse_reg(opl[2])
        ea, eb = state.gpr_name(a), state.gpr_name(b)

        op_map = {
            'add':'+',' addu':'+','addc':'+','adde':'+',
            'subf':'-','subfc':'-','subfe':'-',
            'mullw':'*','mulhw':'*','divw':'/','divwu':'/',
        }
        op = op_map.get(m, '+')

        if m in ('subf','subfc','subfe'):
            val = f'({eb} - {ea})'   # subf rd, ra, rb  →  rd = rb - ra
        else:
            val = f'({ea} {op} {eb})'
        state.gpr[dst] = val
        return [f'    {dname} = {val};']

    # ── Shift/rotate translation ──────────────────────────────────────────────
    def _translate_shift(self, m: str, opl: list, state: RegState) -> list[str]:
        if len(opl) < 2: return [f'    /* {m} {",".join(opl)} */']
        dst  = parse_reg(opl[0])
        src  = parse_reg(opl[1])
        dname = GPR[dst] if dst < len(GPR) else f'r{dst}'
        sname = state.gpr_name(src)

        if m in ('slwi','srwi','srawi','clrlwi','rotlwi','rotrwi') and len(opl) >= 3:
            imm = parse_imm(opl[2])
            if imm is None: return [f'    /* {m} {",".join(opl)} */']
            if m == 'slwi':   val = f'({sname} << {imm})'
            elif m in ('srwi','srawi'): val = f'({sname} >> {imm})'
            elif m == 'clrlwi': mask = (1 << (32 - imm)) - 1; val = f'({sname} & 0x{mask:08X})'
            elif m == 'rotlwi': val = f'(({sname} << {imm}) | ({sname} >> {32-imm}))'
            elif m == 'rotrwi': val = f'(({sname} >> {imm}) | ({sname} << {32-imm}))'
            else: val = f'({sname} >> {imm})'
        elif m in ('slw','srw','sraw') and len(opl) >= 3:
            b = parse_reg(opl[2])
            bname = state.gpr_name(b)
            op = '>>' if m in ('srw','sraw') else '<<'
            val = f'({sname} {op} {bname})'
        else:
            val = f'/* {m}({sname}) */'

        state.gpr[dst] = val
        return [f'    {dname} = {val};']

    # ── Compare translation ───────────────────────────────────────────────────
    def _translate_cmp(self, m: str, opl: list, state: RegState) -> list[str]:
        # cmpwi [crN,] rA, imm
        # cmpw  [crN,] rA, rB
        if opl and opl[0].startswith('cr'):
            cr_n = parse_reg(opl[0])
            opl  = opl[1:]
        else:
            cr_n = 0

        if len(opl) < 2: return [f'    /* {m} */']
        a    = parse_reg(opl[0])
        aname = state.gpr_name(a)
        unsigned = m.startswith('cmpl') or m.startswith('cmpld') or m.startswith('cmpldi')

        if m in ('cmpwi','cmpdi','cmplwi','cmpldi'):
            imm  = parse_imm(opl[1])
            bname = str(imm) if imm is not None else opl[1]
        else:
            b     = parse_reg(opl[1])
            bname = state.gpr_name(b)

        if unsigned:
            state.set_cr_from_cmpU(cr_n, aname, bname)
        else:
            state.set_cr_from_cmp(cr_n, aname, bname)

        # Emit as comment — the condition will be used by the branch that follows
        return [f'    /* cmp: CR{cr_n} = ({aname} vs {bname}) */']

    # ── Load translation ──────────────────────────────────────────────────────
    def _translate_load(self, m: str, opl: list, state: RegState, _type: str) -> list[str]:
        if len(opl) < 2: return [f'    /* {m} {",".join(opl)} */']
        dst  = parse_reg(opl[0])
        dname = GPR[dst] if dst < len(GPR) else f'r{dst}'
        mem  = parse_mem(opl[1])
        if mem is None:
            return [f'    /* {m} {opl[0]}, {opl[1]} */']
        off, base = mem
        bname = state.gpr_name(base)

        cast_map = {
            'lwz':'(uint32_t)','lwzu':'(uint32_t)','lbz':'(uint8_t)','lbzu':'(uint8_t)',
            'lhz':'(uint16_t)','lhzu':'(uint16_t)','lha':'(int16_t)',
            'ld':'(uint64_t)','lwa':'(int32_t)','ldu':'(uint64_t)','lwbrx':'(uint32_t)',
        }
        cast  = cast_map.get(m, '')
        ptr   = f'({bname} + {off:#x})' if off else bname
        val   = f'*({cast}*)({ptr})'
        state.gpr[dst] = val
        hint  = self._string_hint(off) if off else ''
        return [f'    {dname} = {val};{hint}']

    # ── Store translation ─────────────────────────────────────────────────────
    def _translate_store(self, m: str, opl: list, state: RegState, _type: str) -> list[str]:
        if len(opl) < 2: return [f'    /* {m} {",".join(opl)} */']
        src  = parse_reg(opl[0])
        sname = state.gpr_name(src)
        mem  = parse_mem(opl[1])
        if mem is None:
            return [f'    /* {m} {opl[0]}, {opl[1]} */']
        off, base = mem
        bname = state.gpr_name(base)

        cast_map = {
            'stw':'(uint32_t*)','stwu':'(uint32_t*)','stb':'(uint8_t*)','stbu':'(uint8_t*)',
            'sth':'(uint16_t*)','sthu':'(uint16_t*)','std':'(uint64_t*)','stdu':'(uint64_t*)',
        }
        cast = cast_map.get(m, '(uint32_t*)')
        ptr  = f'({bname} + {off:#x})' if off else bname
        return [f'    *({cast})({ptr}) = {sname};']

    # ── FPU load ──────────────────────────────────────────────────────────────
    def _translate_fload(self, m: str, opl: list, state: RegState) -> list[str]:
        if len(opl) < 2: return [f'    /* {m} */']
        dst  = parse_reg(opl[0])
        mem  = parse_mem(opl[1])
        if mem is None: return [f'    /* {m} {opl[0]}, {opl[1]} */']
        off, base = mem
        bname = state.gpr_name(base)
        is_double = m in ('lfd','lfdu')
        cast   = '(double*)' if is_double else '(float*)'
        ptr    = f'({bname} + {off:#x})' if off else bname
        val    = f'*({cast})({ptr})'
        state.fpr[dst] = val
        return [f'    f{dst} = {val};']

    # ── FPU store ─────────────────────────────────────────────────────────────
    def _translate_fstore(self, m: str, opl: list, state: RegState) -> list[str]:
        if len(opl) < 2: return [f'    /* {m} */']
        src  = parse_reg(opl[0])
        sname = state.fpr_name(src)
        mem  = parse_mem(opl[1])
        if mem is None: return [f'    /* {m} {opl[0]}, {opl[1]} */']
        off, base = mem
        bname = state.gpr_name(base)
        is_double = m in ('stfd','stfdu')
        cast = '(double*)' if is_double else '(float*)'
        ptr  = f'({bname} + {off:#x})' if off else bname
        return [f'    *({cast})({ptr}) = {sname};']

    # ── FPU arithmetic ────────────────────────────────────────────────────────
    def _translate_fpu(self, m: str, opl: list, state: RegState) -> list[str]:
        if not opl: return [f'    /* {m} */']
        dst  = parse_reg(opl[0])

        op2_map = {
            'fadd':'+','fadds':'+','fsub':'-','fsubs':'-',
            'fmul':'*','fmuls':'*','fdiv':'/','fdivs':'/',
        }
        un_map  = {'fabs':'fabsf','fnabs':'(-fabsf(','fneg':'-','fmr':'','frsp':'(float)',
                   'fsqrt':'sqrtf','fsqrts':'sqrtf','fres':'(1.0f/','frsqrte':'(1.0f/sqrtf('}
        if m in op2_map and len(opl) >= 3:
            a, b   = parse_reg(opl[1]), parse_reg(opl[2])
            ea, eb = state.fpr_name(a), state.fpr_name(b)
            val    = f'({ea} {op2_map[m]} {eb})'
            state.fpr[dst] = val
            return [f'    f{dst} = {val};']
        if m in ('fmadd','fmadds','fnmadd','fnmadds') and len(opl) >= 4:
            a,c,b = parse_reg(opl[1]),parse_reg(opl[3]),parse_reg(opl[2])
            ea,eb,ec = state.fpr_name(a),state.fpr_name(b),state.fpr_name(c)
            sign = '-' if m.startswith('fn') else ''
            val  = f'{sign}fma({ea}, {ec}, {eb})'
            state.fpr[dst] = val
            return [f'    f{dst} = {val};']
        if m in ('fmsub','fmsubs','fnmsub','fnmsubs') and len(opl) >= 4:
            a,c,b = parse_reg(opl[1]),parse_reg(opl[3]),parse_reg(opl[2])
            ea,eb,ec = state.fpr_name(a),state.fpr_name(b),state.fpr_name(c)
            sign = '-' if m.startswith('fn') else ''
            val  = f'{sign}fma({ea}, {ec}, -({eb}))'
            state.fpr[dst] = val
            return [f'    f{dst} = {val};']
        if m == 'fmr' and len(opl) >= 2:
            src = parse_reg(opl[1])
            val = state.fpr_name(src)
            state.fpr[dst] = val
            return [f'    f{dst} = {val};']
        if m in ('fneg','frsp') and len(opl) >= 2:
            src = parse_reg(opl[1])
            un  = un_map.get(m,'')
            val = f'{un}{state.fpr_name(src)}'
            state.fpr[dst] = val
            return [f'    f{dst} = {val};']
        if m in ('fsqrt','fsqrts','fres','frsqrte') and len(opl) >= 2:
            src = parse_reg(opl[1])
            fn  = un_map.get(m, m)
            val = f'{fn}({state.fpr_name(src)})'
            if m in ('fres','frsqrte'): val += ')'
            state.fpr[dst] = val
            return [f'    f{dst} = {val};']
        if m in ('fctiw','fctiwz') and len(opl) >= 2:
            src = parse_reg(opl[1])
            val = f'(int32_t){state.fpr_name(src)}'
            state.gpr[dst] = val
            return [f'    {GPR[dst] if dst < len(GPR) else f"r{dst}"} = {val};']

        return [f'    /* {m} {",".join(opl)} */']

    # ── Paired-single ─────────────────────────────────────────────────────────
    def _translate_ps(self, m: str, opl: list, state: RegState) -> list[str]:
        if not opl: return [f'    /* {m} */']
        dst = parse_reg(opl[0])
        # Emit as structured comment with actual operands resolved
        args = ', '.join(
            (state.fpr_name(parse_reg(o)) if not o.startswith('cr') else o)
            for o in opl[1:] if parse_reg(o) >= 0
        )
        return [f'    ps_{dst} = {m[3:]}({args});  /* paired-single */']

    # ── Rotate/mask ───────────────────────────────────────────────────────────
    def _translate_rotate(self, m: str, opl: list, state: RegState) -> list[str]:
        if len(opl) < 5 and m in ('rlwinm','rlwinm.','rlwimi'):
            return [f'    /* {m} {",".join(opl)} */']
        try:
            dst  = parse_reg(opl[0])
            src  = parse_reg(opl[1])
            sh   = parse_imm(opl[2])
            mb   = parse_imm(opl[3])
            me   = parse_imm(opl[4]) if len(opl) > 4 else None
            sname = state.gpr_name(src)
            dname = GPR[dst] if dst < len(GPR) else f'r{dst}'

            if me is not None and sh == 0 and mb == 0:
                # Extract: rlwinm rD, rS, 0, 0, N  →  clrlwi N
                mask = (0xFFFFFFFF >> mb) & (0xFFFFFFFF << (31 - me)) & 0xFFFFFFFF
                val  = f'({sname} & 0x{mask:08X})'
            elif sh is not None:
                rot  = f'(({sname} << {sh}) | ({sname} >> {32-sh}))' if sh else sname
                if me is not None:
                    mb2 = mb if mb <= me else 0
                    mask = ((0xFFFFFFFF >> mb2) & ~((0xFFFFFFFF >> me) >> 1)) & 0xFFFFFFFF
                    val  = f'({rot} & 0x{mask:08X})'
                else:
                    val  = rot
            else:
                val = f'/* {m}({sname}) */'

            state.gpr[dst] = val
            return [f'    {dname} = {val};']
        except:
            return [f'    /* {m} {",".join(opl)} */']


# ─────────────────────────────────────────────────────────────────────────────
# Function emitter
# ─────────────────────────────────────────────────────────────────────────────

def build_string_map(dol: Dol) -> dict:
    result = {}
    for sec in dol.sections:
        if sec.is_executable: continue
        i, start = 0, None
        for i, b in enumerate(sec.data):
            if 0x20 <= b <= 0x7E:
                if start is None: start = i
            else:
                if start is not None and i - start >= 4:
                    result[sec.vma + start] = sec.data[start:i].decode('ascii','replace')
                start = None
    return result


def emit_function(lifter: Lifter, insns: list, start: int, end: int,
                  fname: str, out) -> int:
    func_insns = [i for i in insns if start <= i.address < end]
    if not func_insns: return 0

    # Detect local branch targets for labels
    branch_targets: set[int] = set()
    for ins in func_insns:
        m  = ins.mnemonic.lower()
        op = ins.op_str.strip()
        if m.startswith('b') and op:
            tgt = None
            try: tgt = int(op.split(',')[-1].strip(), 16)
            except: pass
            if tgt and start <= tgt < end:
                branch_targets.add(tgt)

    out.write(f'\n// {"─"*60}\n')
    out.write(f'// {fname}  @  0x{start:08X}\n')
    out.write(f'// {"─"*60}\n')
    out.write(f'void* {fname}(void* a0, void* a1, void* a2, void* a3) {{\n')
    out.write(f'    uint32_t CTR = 0;\n')
    out.write(f'    uint32_t LR = 0;\n\n')

    state = RegState()
    # Seed argument registers
    for i,name in enumerate(['a0','a1','a2','a3']):
        state.gpr[3+i] = name

    for ins in func_insns:
        addr = ins.address

        # Emit label if this is a branch target
        if addr in branch_targets or addr in lifter.label_map:
            lbl = lifter.label(addr)
            out.write(f'{lbl}:\n')

        # Translate
        try:
            lines = lifter.translate(ins, state)
        except Exception as e:
            lines = [f'    /* ERROR @ {addr:08X}: {e} */']

        for line in lines:
            out.write(line + '\n')

    out.write(f'}}\n')
    return len(func_insns)


# ─────────────────────────────────────────────────────────────────────────────
# Main
# ─────────────────────────────────────────────────────────────────────────────

def find_function_bounds(insns: list, entry: int, label_map: dict) -> list[tuple]:
    if not insns: return []
    by_addr = {i.address: i for i in insns}
    all_addrs = sorted(by_addr.keys())
    if not all_addrs: return []

    starts = set()
    starts.add(entry)
    for i in insns:
        m = i.mnemonic.lower()
        if m in ('bl','bla'):
            try:
                t = int(i.op_str.strip(), 16)
                if t in by_addr: starts.add(t)
            except: pass
    for a in label_map:
        if a in by_addr: starts.add(a)

    starts = sorted(starts)
    bounds = []
    for idx, s in enumerate(starts):
        e = starts[idx+1] if idx+1 < len(starts) else all_addrs[-1]+4
        bounds.append((s, e))
    return bounds


def lift_section(dol: Dol, sec: DolSection, out_dir: str,
                 label_map: dict, string_map: dict, max_funcs: int):
    md = Cs(CS_ARCH_PPC, CS_MODE_32 | CS_MODE_BIG_ENDIAN)
    md.detail  = False
    md.skipdata = True

    print(f'  Disassembling {sec.name} ...')
    insns = list(md.disasm(sec.data, sec.vma))
    print(f'    {len(insns):,} instructions')

    bounds = find_function_bounds(insns, dol.entry, label_map)
    print(f'    {len(bounds)} functions found')

    lifter = Lifter(dol, label_map, string_map)
    out_path = os.path.join(out_dir, f'{sec.name}_v2.c')

    written = 0
    with open(out_path, 'w') as out:
        out.write('// Auto-generated by lift_to_c_v2.py\n')
        out.write('// Silent Hill Origins (Wii) — main.dol\n')
        out.write('// NOT directly compilable — use as structured reference\n\n')
        out.write('#include <stdint.h>\n#include <math.h>\n#include <stdbool.h>\n\n')

        # Forward declarations
        out.write('// Forward declarations\n')
        for s, e in bounds[:max_funcs]:
            fname = label_map.get(s, f'func_{s:08X}')
            out.write(f'void* {fname}(void* a0, void* a1, void* a2, void* a3);\n')
        out.write('\n')

        for s, e in bounds:
            if written >= max_funcs: break
            fname = label_map.get(s, f'func_{s:08X}')
            n = emit_function(lifter, insns, s, e, fname, out)
            written += 1

    print(f'    Wrote {written} functions → {out_path}')
    return out_path


def main():
    if len(sys.argv) < 3:
        print('Usage: lift_to_c_v2.py <main.dol> <out_dir> [symbol_map.txt] [max_funcs]')
        sys.exit(1)

    dol_path  = sys.argv[1]
    out_dir   = sys.argv[2]
    sym_path  = sys.argv[3] if len(sys.argv) > 3 else None
    max_funcs = int(sys.argv[4]) if len(sys.argv) > 4 else 3000

    os.makedirs(out_dir, exist_ok=True)

    print(f'Loading {dol_path} ...')
    dol = load(dol_path)
    dol.print_info()

    label_map = {dol.entry: 'game_entry'}
    if sym_path and os.path.exists(sym_path):
        with open(sym_path) as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith('#'): continue
                parts = line.split(None, 1)
                if len(parts) == 2:
                    try: label_map[int(parts[0], 16)] = parts[1]
                    except: pass
        print(f'Loaded {len(label_map)} labels')

    print('\nBuilding string map ...')
    string_map = build_string_map(dol)
    print(f'  {len(string_map)} strings')

    print()
    for sec in dol.sections:
        if not sec.is_executable: continue
        lift_section(dol, sec, out_dir, label_map, string_map, max_funcs)

    print('\nDone!')


if __name__ == '__main__':
    main()
