#!/usr/bin/env python3
"""
PowerPC Disassembler — disassembles all executable (text) sections of a DOL
using the Capstone engine and outputs annotated assembly.

Output: disasm/<section>.asm with:
  - VMA addresses
  - Raw bytes
  - Mnemonics + operands
  - Branch targets resolved to labels
  - Data references cross-referenced to string table
"""
import sys, os, struct
from dol_parser import load, Dol

try:
    from capstone import Cs, CS_ARCH_PPC, CS_MODE_32, CS_MODE_BIG_ENDIAN
    from capstone.ppc import *
except ImportError:
    print("ERROR: capstone not found. Run: pip install capstone")
    sys.exit(1)


def build_string_map(dol: Dol, min_len=5) -> dict:
    """Returns {vma: string_text} for all readable strings in data sections."""
    result = {}
    for sec in dol.sections:
        if sec.is_executable:
            continue
        i, start = 0, None
        data = sec.data
        for i, b in enumerate(data):
            if 0x20 <= b <= 0x7E:
                if start is None:
                    start = i
            else:
                if start is not None and (i - start) >= min_len:
                    text = data[start:i].decode("ascii", errors="replace")
                    result[sec.vma + start] = text
                start = None
    return result


def disassemble_section(dol: Dol, sec, out_path: str, string_map: dict, label_map: dict):
    md = Cs(CS_ARCH_PPC, CS_MODE_32 | CS_MODE_BIG_ENDIAN)
    md.detail = True
    md.skipdata = True

    insns = list(md.disasm(sec.data, sec.vma))

    # First pass: collect all branch targets → build local labels
    branch_targets = set()
    for insn in insns:
        mnemonic = insn.mnemonic.lower()
        if mnemonic.startswith("b"):
            op_str = insn.op_str.strip()
            try:
                target = int(op_str, 16)
                branch_targets.add(target)
            except ValueError:
                pass

    with open(out_path, "w") as f:
        f.write(f"; ============================================================\n")
        f.write(f"; Section: {sec.name}  VMA=0x{sec.vma:08X}  size=0x{sec.size:X}\n")
        f.write(f"; ============================================================\n\n")

        current_func = None

        for insn in insns:
            addr = insn.address

            # Function boundary: if this address is in the label map, start a new function block
            if addr in label_map:
                fname = label_map[addr]
                f.write(f"\n\n; {'─'*60}\n")
                f.write(f"; FUNCTION: {fname}\n")
                f.write(f"; {'─'*60}\n")
                f.write(f"{fname}:  ; 0x{addr:08X}\n")
                current_func = fname

            # Local branch target label
            if addr in branch_targets and addr not in label_map:
                f.write(f".L_{addr:08X}:\n")

            # Print instruction
            raw_bytes = " ".join(f"{b:02X}" for b in insn.bytes)
            mnemonic  = insn.mnemonic
            op_str    = insn.op_str

            # Annotate branch targets
            comment = ""
            if mnemonic.lower().startswith("b"):
                try:
                    target = int(op_str, 16)
                    if target in label_map:
                        comment = f"  ; -> {label_map[target]}"
                    elif target in branch_targets:
                        comment = f"  ; -> .L_{target:08X}"
                    else:
                        comment = f"  ; -> 0x{target:08X}"
                except ValueError:
                    pass

            # Annotate data references (lis + addi pattern → often a string address)
            if "0x" in op_str:
                try:
                    for word in op_str.split(","):
                        word = word.strip()
                        if word.startswith("0x") or word.startswith("-0x"):
                            val = int(word, 16) & 0xFFFFFFFF
                            if val in string_map:
                                s = string_map[val][:40].replace("\n","\\n")
                                comment += f'  ; "{s}"'
                except Exception:
                    pass

            f.write(f"  {addr:08X}  {raw_bytes:<12}  {mnemonic:<8} {op_str}{comment}\n")

    print(f"  Disassembled {len(insns):6} insns → {out_path}")


def disassemble_all(dol: Dol, out_dir: str, label_map: dict = None):
    if label_map is None:
        label_map = {}
    # Always label the entry point
    label_map[dol.entry] = "main_entry"

    os.makedirs(out_dir, exist_ok=True)
    string_map = build_string_map(dol)
    print(f"Built string map: {len(string_map)} strings")

    for sec in dol.sections:
        if not sec.is_executable:
            continue
        out_path = os.path.join(out_dir, f"{sec.name}.asm")
        print(f"Disassembling {sec.name} ...")
        disassemble_section(dol, sec, out_path, string_map, label_map)


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: ppc_disasm.py <main.dol> <output_dir> [symbol_map.txt]")
        sys.exit(1)

    dol_path = sys.argv[1]
    out_dir  = sys.argv[2]

    label_map = {}
    if len(sys.argv) > 3 and os.path.exists(sys.argv[3]):
        with open(sys.argv[3]) as f:
            for line in f:
                line = line.strip()
                if line.startswith("#") or not line:
                    continue
                parts = line.split()
                if len(parts) >= 2:
                    try:
                        addr = int(parts[0], 16)
                        name = parts[1]
                        label_map[addr] = name
                    except ValueError:
                        pass
        print(f"Loaded {len(label_map)} labels from symbol map")

    dol = load(dol_path)
    dol.print_info()
    print()
    disassemble_all(dol, out_dir, label_map)
    print("\nDone!")
