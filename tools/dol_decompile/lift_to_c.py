#!/usr/bin/env python3
"""
PPC → C Lifter — converts PowerPC assembly to readable C pseudocode.

This is a pattern-based structural analysis, NOT a full decompiler.
For each function it:
  1. Detects function boundaries (blr / function call graph)
  2. Reconstructs local variables from stack frame offsets
  3. Converts common PPC idioms to C equivalents
  4. Outputs one C file per function with the pseudocode

This is best used as a starting point alongside Ghidra decompiler output.
"""
import sys, os, re
from dataclasses import dataclass, field
from typing import List, Dict, Tuple, Optional
from dol_parser import load, Dol

try:
    from capstone import Cs, CS_ARCH_PPC, CS_MODE_32, CS_MODE_BIG_ENDIAN
    from capstone.ppc import *
except ImportError:
    print("ERROR: capstone not found.")
    sys.exit(1)


# -------------------------------------------------------------------------
# PPC ABI helpers (System V PPC32 / Wii ABI)
# -------------------------------------------------------------------------
# r3-r10  = function arguments / return values
# r0      = scratch / link register copy
# r1      = stack pointer
# r2      = RTOC (read-only small data)
# r3      = return value
# r31,r30 = callee-saved frame registers (non-volatile)
# f1-f8   = float arguments
# CTR, LR = count / link registers

REG_NAMES = {
    0:"r0", 1:"sp", 2:"rtoc",
    3:"a0", 4:"a1", 5:"a2", 6:"a3", 7:"a4", 8:"a5", 9:"a6", 10:"a7",
    11:"r11", 12:"r12",
    13:"r13", 14:"r14", 15:"r15", 16:"r16", 17:"r17", 18:"r18", 19:"r19",
    20:"r20", 21:"r21", 22:"r22", 23:"r23", 24:"r24", 25:"r25",
    26:"r26", 27:"r27", 28:"r28", 29:"r29", 30:"r30", 31:"r31",
}

def reg_name(r: int) -> str:
    return REG_NAMES.get(r, f"r{r}")


@dataclass
class Instruction:
    addr:     int
    mnemonic: str
    op_str:   str
    raw:      bytes


@dataclass
class BasicBlock:
    start:  int
    insns:  List[Instruction] = field(default_factory=list)
    succs:  List[int]         = field(default_factory=list)  # successor VMA addresses


@dataclass
class Function:
    start:   int
    name:    str
    blocks:  List[BasicBlock] = field(default_factory=list)
    stack_size: int = 0
    # Stack offsets → local variable names
    locals:  Dict[int, str]   = field(default_factory=dict)


# -------------------------------------------------------------------------
# Function boundary detection
# -------------------------------------------------------------------------
def find_functions(insns, entry: int, label_map: dict) -> List[Function]:
    if not insns:
        return []
    by_addr = {i.address: i for i in insns}
    all_addrs = sorted(by_addr.keys())
    if not all_addrs:
        return []

    func_starts = set()
    func_starts.add(entry)

    # BL/BLA targets are function calls
    for insn in insns:
        m = insn.mnemonic.lower()
        if m in ("bl", "bla") or m.startswith("bl "):
            try:
                target = int(insn.op_str.strip(), 16)
                if target in by_addr:
                    func_starts.add(target)
            except ValueError:
                pass

    # Any address in label_map is also a function
    for addr in label_map:
        if addr in by_addr:
            func_starts.add(addr)

    func_starts = sorted(func_starts)

    functions = []
    for i, start in enumerate(func_starts):
        end = func_starts[i+1] if i+1 < len(func_starts) else all_addrs[-1] + 4
        name = label_map.get(start, f"func_{start:08X}")
        func_insns = [by_addr[a] for a in all_addrs if start <= a < end and a in by_addr]
        f = Function(start=start, name=name)
        bb = BasicBlock(start=start)
        for insn in func_insns:
            bb.insns.append(insn)
            m = insn.mnemonic.lower()
            # End basic block on branches
            if m.startswith("b") and m not in ("bc ", "bctr"):
                f.blocks.append(bb)
                bb = BasicBlock(start=insn.address + 4)
        if bb.insns:
            f.blocks.append(bb)
        functions.append(f)

    return functions


# -------------------------------------------------------------------------
# Simple PPC → C pattern matching
# -------------------------------------------------------------------------
def ppc_to_c(func: Function, string_map: dict) -> str:
    """
    Convert a function to C pseudocode via pattern matching.
    This is intentionally SIMPLE and readable, not perfect.
    """
    lines = []
    lines.append(f"// ====================================================")
    lines.append(f"// Function: {func.name}")
    lines.append(f"// VMA: 0x{func.start:08X}")
    lines.append(f"// ====================================================")
    lines.append(f"void* {func.name}(void* a0, void* a1, void* a2, void* a3)")
    lines.append("{")

    # Collect all stack offsets used
    stack_offsets = {}
    for block in func.blocks:
        for insn in block.insns:
            # stw/lwz rN, OFFSET(r1)  — stack frame access
            m = re.match(r'(\w+),\s*(-?\d+)\(r1\)', insn.op_str)
            if m and insn.mnemonic.lower() in ("stw","lwz","lfs","stfs","std","ld"):
                off = int(m.group(2))
                if off not in stack_offsets and off < 0:
                    stack_offsets[off] = f"var_{abs(off):02X}"

    # Declare locals
    if stack_offsets:
        lines.append(f"    // Stack frame locals:")
        for off, name in sorted(stack_offsets.items()):
            lines.append(f"    void* {name}; // [sp + {off}]")
        lines.append("")

    # Convert instructions to C
    for block in func.blocks:
        if len(func.blocks) > 1:
            lines.append(f"\n  .L_{block.start:08X}:  // basic block")

        for insn in block.insns:
            m  = insn.mnemonic.lower()
            op = insn.op_str.strip()

            # ---- Common patterns → C ----
            c_line = None

            # Return
            if m == "blr":
                c_line = "return a0;"  # approximate

            # Function call
            elif m == "bl":
                try:
                    target = int(op, 16)
                    # look up in string map neighborhood — often the function name is nearby
                    c_line = f"/* call */ func_{target:08X}(/* args */);"
                except ValueError:
                    c_line = f"/* call */ {op}();"

            # Load word → assignment
            elif m == "lwz":
                parts = op.split(",")
                if len(parts) == 2:
                    dst = parts[0].strip()
                    src = parts[1].strip()
                    # check string reference
                    try:
                        addr_m = re.match(r'-?(\d+)\((\w+)\)', src)
                        if addr_m:
                            c_line = f"    {dst} = *({src});"
                        else:
                            c_line = f"    {dst} = {src};"
                    except:
                        c_line = f"    {dst} = {src};"

            # Store word → assignment
            elif m == "stw":
                parts = op.split(",")
                if len(parts) == 2:
                    c_line = f"    *({parts[1].strip()}) = {parts[0].strip()};"

            # Add immediate → arithmetic
            elif m == "addi":
                parts = [p.strip() for p in op.split(",")]
                if len(parts) == 3:
                    c_line = f"    {parts[0]} = {parts[1]} + {parts[2]};"

            # Move (mr) → assignment
            elif m == "mr":
                parts = [p.strip() for p in op.split(",")]
                if len(parts) == 2:
                    c_line = f"    {parts[0]} = {parts[1]};"

            # Conditional branch
            elif m.startswith("b") and m not in ("blr","bl","b"):
                try:
                    target = int(op.split(",")[-1].strip(), 16)
                    cond_map = {
                        "beq":"if (==)", "bne":"if (!=)",
                        "bgt":"if (>)",  "blt":"if (<)",
                        "bge":"if (>=)", "ble":"if (<=)",
                    }
                    cond = cond_map.get(m, f"if ({m})")
                    c_line = f"    {cond} goto .L_{target:08X};"
                except:
                    c_line = f"    {m} {op};"

            # Comparison
            elif m in ("cmpwi","cmpw","cmplwi"):
                parts = [p.strip() for p in op.split(",")]
                if len(parts) >= 3:
                    c_line = f"    cmp({parts[-2]}, {parts[-1]}); // {m}"

            # Load immediate (lis/li)
            elif m in ("li", "lis"):
                parts = [p.strip() for p in op.split(",")]
                if len(parts) == 2:
                    # Check if this is loading a string pointer
                    try:
                        val = int(parts[1], 16) << 16
                        if val in string_map:
                            s = string_map[val][:30].replace("\n","\\n")
                            c_line = f'    {parts[0]} = "{s}"; // lis pattern'
                        else:
                            c_line = f"    {parts[0]} = {parts[1]}{'0000' if m=='lis' else ''};"
                    except:
                        c_line = f"    {parts[0]} = {parts[1]};"

            # Fallback: raw asm comment
            if c_line is None:
                try:
                    c_line = f"    /* {insn.address:08X}  {m:<8} {op} */"
                except Exception:
                    c_line = "    /* [skipdata] */"

            lines.append(c_line)

    lines.append("}")
    lines.append("")
    return "\n".join(lines)


# -------------------------------------------------------------------------
# Main
# -------------------------------------------------------------------------
def lift_to_c(dol: Dol, out_dir: str, label_map: dict, max_funcs: int = 500):
    os.makedirs(out_dir, exist_ok=True)

    # Build string map
    string_map = {}
    for sec in dol.sections:
        if sec.is_executable:
            continue
        i, start = 0, None
        for i, b in enumerate(sec.data):
            if 0x20 <= b <= 0x7E:
                if start is None: start = i
            else:
                if start is not None and (i-start) >= 4:
                    text = sec.data[start:i].decode("ascii", errors="replace")
                    string_map[sec.vma + start] = text
                start = None

    md = Cs(CS_ARCH_PPC, CS_MODE_32 | CS_MODE_BIG_ENDIAN)
    md.detail = False
    md.skipdata = True

    all_functions = []
    for sec in dol.sections:
        if not sec.is_executable:
            continue
        print(f"Lifting {sec.name} ...")
        insns = list(md.disasm(sec.data, sec.vma))
        funcs = find_functions(insns, dol.entry, label_map)
        print(f"  Found {len(funcs)} functions in {sec.name}")
        all_functions.extend(funcs)

    # Write one C file per section (grouped)
    # Also write a combined header
    header_path = os.path.join(out_dir, "decompiled.h")
    with open(header_path, "w") as hf:
        hf.write("#pragma once\n")
        hf.write("// Auto-generated function declarations from main.dol\n")
        hf.write("// DO NOT EDIT — regenerate with lift_to_c.py\n\n")
        hf.write('#include <stdint.h>\n\n')
        for func in all_functions[:max_funcs]:
            hf.write(f"void* {func.name}(void* a0, void* a1, void* a2, void* a3);\n")
    print(f"Wrote declarations → {header_path}")

    # Write C pseudocode
    c_path = os.path.join(out_dir, "decompiled.c")
    with open(c_path, "w") as cf:
        cf.write('#include "decompiled.h"\n\n')
        cf.write("// ============================================================\n")
        cf.write("// Silent Hill Origins (Wii) — main.dol decompilation\n")
        cf.write("// Generated by lift_to_c.py\n")
        cf.write("// This is pseudocode — NOT compilable directly.\n")
        cf.write("// Use as a starting point for manual porting.\n")
        cf.write("// ============================================================\n\n")
        written = 0
        for func in all_functions:
            if written >= max_funcs:
                break
            c_code = ppc_to_c(func, string_map)
            cf.write(c_code + "\n")
            written += 1

    print(f"Wrote {written} function pseudocode → {c_path}")


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: lift_to_c.py <main.dol> <output_dir> [symbol_map.txt] [max_funcs]")
        sys.exit(1)

    dol_path  = sys.argv[1]
    out_dir   = sys.argv[2]
    label_map = {}
    max_funcs = 2000

    if len(sys.argv) > 3 and os.path.exists(sys.argv[3]):
        with open(sys.argv[3]) as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"): continue
                parts = line.split(None, 1)
                if len(parts) == 2:
                    try:
                        label_map[int(parts[0], 16)] = parts[1]
                    except ValueError:
                        pass
        print(f"Loaded {len(label_map)} labels")

    if len(sys.argv) > 4:
        max_funcs = int(sys.argv[4])

    dol = load(dol_path)
    dol.print_info()
    print()
    lift_to_c(dol, out_dir, label_map, max_funcs)
    print("\nDone!")
