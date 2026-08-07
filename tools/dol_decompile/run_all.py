#!/usr/bin/env python3
"""
run_all.py — One-shot DOL decompilation pipeline runner.

Steps:
  1. Parse DOL header, print section map
  2. Scan strings, detect SDK & engine references
  3. Disassemble all executable sections (PPC asm)
  4. Lift to C pseudocode
  5. Generate a summary report

Usage:
  python3 run_all.py <main.dol>                   (auto output to ./dol_out/)
  python3 run_all.py <main.dol> <output_dir>
  python3 run_all.py <main.dol> <output_dir> <symbol_map.txt>
"""
import sys, os, subprocess, time

DOL_PATH    = None
OUT_DIR     = None
SYMBOL_MAP  = None

def step(n, desc):
    print(f"\n{'='*60}")
    print(f" Step {n}: {desc}")
    print(f"{'='*60}")

def run():
    global DOL_PATH, OUT_DIR, SYMBOL_MAP
    DOL_PATH   = sys.argv[1] if len(sys.argv) > 1 else None
    OUT_DIR    = sys.argv[2] if len(sys.argv) > 2 else "dol_out"
    SYMBOL_MAP = sys.argv[3] if len(sys.argv) > 3 else None

    if not DOL_PATH or not os.path.exists(DOL_PATH):
        print(f"ERROR: DOL file not found: {DOL_PATH}")
        print(__doc__)
        sys.exit(1)

    os.makedirs(OUT_DIR, exist_ok=True)
    script_dir = os.path.dirname(os.path.abspath(__file__))

    t0 = time.time()

    # ── Step 1: Parse DOL ────────────────────────────────────────────────
    step(1, "Parsing DOL header")
    import importlib.util, sys as _sys

    # Import modules from same directory
    def load_module(name, path):
        spec = importlib.util.spec_from_file_location(name, path)
        mod  = importlib.util.module_from_spec(spec)
        _sys.modules[name] = mod
        spec.loader.exec_module(mod)
        return mod

    dol_parser = load_module("dol_parser", os.path.join(script_dir, "dol_parser.py"))
    dol = dol_parser.load(DOL_PATH)
    dol.print_info()

    # ── Step 2: String scan ───────────────────────────────────────────────
    step(2, "Scanning strings and detecting SDK / engine references")
    string_scanner = load_module("string_scanner", os.path.join(script_dir, "string_scanner.py"))
    strings_dir = os.path.join(OUT_DIR, "strings")
    string_scanner.scan(dol, strings_dir)

    # ── Step 3: Disassembly ───────────────────────────────────────────────
    step(3, "Disassembling PowerPC executable sections")
    ppc_disasm = load_module("ppc_disasm", os.path.join(script_dir, "ppc_disasm.py"))

    label_map = {dol.entry: "main_entry"}
    if SYMBOL_MAP and os.path.exists(SYMBOL_MAP):
        with open(SYMBOL_MAP) as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"): continue
                parts = line.split(None, 1)
                if len(parts) == 2:
                    try:
                        label_map[int(parts[0], 16)] = parts[1]
                    except ValueError:
                        pass
        print(f"Loaded {len(label_map)} labels from {SYMBOL_MAP}")

    asm_dir = os.path.join(OUT_DIR, "asm")
    ppc_disasm.disassemble_all(dol, asm_dir, label_map)

    # ── Step 4: Lift to C ─────────────────────────────────────────────────
    step(4, "Lifting PPC assembly to C pseudocode")
    lift_to_c = load_module("lift_to_c", os.path.join(script_dir, "lift_to_c.py"))
    c_dir = os.path.join(OUT_DIR, "c_pseudocode")
    lift_to_c.lift_to_c(dol, c_dir, label_map, max_funcs=5000)

    # ── Step 5: Summary report ────────────────────────────────────────────
    step(5, "Generating summary report")
    elapsed = time.time() - t0

    report = []
    report.append("# Silent Hill Origins (Wii) — DOL Decompilation Report")
    report.append("")
    report.append(f"- **DOL file:** `{DOL_PATH}`")
    report.append(f"- **File size:** {os.path.getsize(DOL_PATH) / 1024:.1f} KB")
    report.append(f"- **Entry point:** `0x{dol.entry:08X}`")
    report.append(f"- **BSS:** `0x{dol.bss_addr:08X}` ({dol.bss_size // 1024} KB)")
    report.append("")
    report.append("## Sections")
    report.append("")
    report.append("| Section | VMA | Size | Type |")
    report.append("|---------|-----|------|------|")
    for sec in dol.sections:
        flags = "code" if sec.is_executable else "data"
        report.append(f"| `{sec.name}` | `0x{sec.vma:08X}` | {sec.size // 1024} KB | {flags} |")
    report.append("")

    # Count strings
    strings_file = os.path.join(strings_dir, "strings.txt")
    if os.path.exists(strings_file):
        with open(strings_file) as f:
            n_strings = sum(1 for _ in f)
        report.append(f"## Strings\n\n- **Total strings found:** {n_strings}")

        sdk_file = os.path.join(strings_dir, "sdk_hints.txt")
        if os.path.exists(sdk_file):
            with open(sdk_file) as f:
                n_sdk = sum(1 for l in f if not l.startswith("#") and l.strip())
            report.append(f"- **SDK/library references:** {n_sdk}")

        eng_file = os.path.join(strings_dir, "engine_hints.txt")
        if os.path.exists(eng_file):
            with open(eng_file) as f:
                n_eng = sum(1 for l in f if not l.startswith("#") and l.strip())
            report.append(f"- **Engine/game references:** {n_eng}")

    # Count asm
    report.append("\n## Disassembly")
    total_insns = 0
    for fname in os.listdir(asm_dir) if os.path.exists(asm_dir) else []:
        if fname.endswith(".asm"):
            with open(os.path.join(asm_dir, fname)) as f:
                n = sum(1 for l in f if l.strip() and not l.startswith(";") and not l.endswith(":"))
            total_insns += n
            report.append(f"- `{fname}`: {n} instructions")
    report.append(f"\n**Total instructions disassembled:** {total_insns:,}")

    report.append(f"\n## Output Files")
    report.append(f"- `{strings_dir}/strings.txt` — all printable strings")
    report.append(f"- `{strings_dir}/sdk_hints.txt` — SDK references")
    report.append(f"- `{strings_dir}/engine_hints.txt` — game engine strings")
    report.append(f"- `{strings_dir}/cpp_symbols.txt` — C++ mangled symbols (if any)")
    report.append(f"- `{asm_dir}/text*.asm` — annotated PPC assembly")
    report.append(f"- `{c_dir}/decompiled.c` — C pseudocode (starting point)")
    report.append(f"- `{c_dir}/decompiled.h` — function declarations")

    report.append(f"\n## Next Steps")
    report.append("""
1. Open `dol_out/strings/engine_hints.txt` — find Climax class names matching the PS2 viewer
2. Cross-reference function addresses from `sdk_hints.txt` → OS/GX function call sites
3. Open `dol_out/asm/text0.asm` in a text editor — search by function name
4. Open `decompiled.c` — use it as the starting point for manual porting
5. For deeper analysis: **install Ghidra** and import the DOL with the WiiLoader plugin
   - Ghidra's decompiler produces much cleaner output than this script
   - Import our `dol_out/strings/sdk_hints.txt` to bulk-rename functions in Ghidra
""")

    report.append(f"\n---\n*Generated in {elapsed:.1f}s*")

    report_path = os.path.join(OUT_DIR, "REPORT.md")
    with open(report_path, "w") as f:
        f.write("\n".join(report))

    print(f"\n{'='*60}")
    print(f" DONE in {elapsed:.1f}s")
    print(f" Report: {report_path}")
    print(f"{'='*60}")


if __name__ == "__main__":
    run()
