#!/usr/bin/env python3
"""
String Scanner — finds all ASCII/UTF-8 strings inside a DOL, then tries to
match them against well-known Wii SDK and Climax engine patterns.

Outputs:
  - strings.txt     : all strings with their VMA
  - sdk_hints.txt   : matched SDK function names / file paths
  - engine_hints.txt: matched game engine strings
"""
import sys, os, re
from dol_parser import load, Dol

# -------------------------------------------------------------------------
# Known Wii SDK / OS strings that identify library functions or modules
# -------------------------------------------------------------------------
SDK_PATTERNS = [
    # RVL / Broadway OS
    (r"OSInit",              "Wii OS init — entry point region"),
    (r"OSReport",            "printf-like debug output"),
    (r"OSFatal",             "fatal error handler"),
    (r"GXInit",              "GX graphics subsystem init"),
    (r"GXBegin",             "GX draw call"),
    (r"DVDOpen",             "DVD file open"),
    (r"DVDRead",             "DVD async read"),
    (r"PADInit",             "GamePad init"),
    (r"WPADInit",            "Wii Remote init"),
    (r"AIInit",              "Audio Interface init"),
    (r"AXInit",              "AX audio mixer init"),
    (r"AXQuit",              "AX mixer shutdown"),
    (r"NANDOpen",            "NAND flash open"),
    (r"SCInit",              "System Config init"),
    (r"VIInit",              "Video Interface init"),
    (r"VISetNextFrameBuffer","Video flip"),
    (r"__start",             "CRT entry point"),
    (r"__init_cpp",          "C++ global ctor runner"),
    (r"_ZN",                 "C++ mangled symbol (has name info)"),
    # devkitPPC / newlib
    (r"malloc",              "heap alloc"),
    (r"free",                "heap free"),
    (r"printf",              "formatted print"),
    (r"memcpy",              "memory copy"),
    (r"memset",              "memory set"),
    (r"strlen",              "string length"),
    # Climax Engine patterns (same engine as PS2 version)
    (r"CArchive",            "Climax archive system"),
    (r"CPlayer",             "Player entity"),
    (r"CShadow",             "Shadow / Travis"),
    (r"CCamera",             "Camera system"),
    (r"CCollision",          "Collision"),
    (r"rwaID_",              "RWS audio chunk type"),
    (r"rwID_",               "RenderWare chunk type"),
    (r"\.arc",               "Archive file reference"),
    (r"\.txd",               "Texture dict reference"),
    (r"Silent Hill",         "Game title string"),
    (r"Climax",              "Studio name"),
    (r"ASSERT",              "Debug assertion"),
    (r"assert",              "C assert macro"),
    (r"TODO",                "Developer note"),
    (r"FIXME",               "Developer note"),
    (r"Error",               "Error string"),
]
SDK_RE = [(re.compile(p, re.IGNORECASE), label) for p, label in SDK_PATTERNS]

def extract_strings(data: bytes, vma_base: int, min_len: int = 5):
    """Yield (vma, string) for every printable ASCII run >= min_len chars."""
    i, start = 0, None
    for i, b in enumerate(data):
        printable = (0x20 <= b <= 0x7E) or b in (9, 10, 13)
        if printable:
            if start is None:
                start = i
        else:
            if start is not None:
                s = data[start:i]
                text = s.decode("ascii", errors="replace")
                text = text.strip()
                if len(text) >= min_len:
                    yield (vma_base + start, text)
                start = None
    if start is not None and i - start >= min_len:
        yield (vma_base + start, data[start:].decode("ascii", errors="replace").strip())


def scan(dol: Dol, out_dir: str):
    os.makedirs(out_dir, exist_ok=True)

    all_strings = []
    for sec in dol.sections:
        for vma, text in extract_strings(sec.data, sec.vma):
            all_strings.append((vma, sec.name, text))

    # Write all strings
    strings_path = os.path.join(out_dir, "strings.txt")
    with open(strings_path, "w") as f:
        for vma, sec, text in all_strings:
            f.write(f"0x{vma:08X}  [{sec}]  {text}\n")
    print(f"Wrote {len(all_strings)} strings → {strings_path}")

    # Match against known patterns
    sdk_hits     = []
    engine_hits  = []
    cpp_symbols  = []

    for vma, sec, text in all_strings:
        for rx, label in SDK_RE:
            if rx.search(text):
                entry = (vma, sec, text, label)
                if "climax" in label.lower() or "rwa" in label.lower() or \
                   "archive" in label.lower() or "player" in label.lower() or \
                   "camera" in label.lower() or "collision" in label.lower() or \
                   "shadow" in label.lower():
                    engine_hits.append(entry)
                else:
                    sdk_hits.append(entry)
                break

        # C++ demangled symbol detection: _ZN prefix
        if text.startswith("_ZN") or text.startswith("_Z"):
            cpp_symbols.append((vma, sec, text))

    # SDK hits
    sdk_path = os.path.join(out_dir, "sdk_hints.txt")
    with open(sdk_path, "w") as f:
        f.write("# Wii SDK / system library references\n")
        f.write(f"# {'VMA':>10}  {'Section':<10}  {'Label':<40}  String\n")
        f.write("-" * 100 + "\n")
        for vma, sec, text, label in sdk_hits:
            f.write(f"0x{vma:08X}  {sec:<10}  {label:<40}  {text[:80]}\n")
    print(f"Wrote {len(sdk_hits)} SDK hints → {sdk_path}")

    # Engine hints
    eng_path = os.path.join(out_dir, "engine_hints.txt")
    with open(eng_path, "w") as f:
        f.write("# Climax Engine / Silent Hill game strings\n")
        f.write(f"# {'VMA':>10}  {'Section':<10}  {'Label':<40}  String\n")
        f.write("-" * 100 + "\n")
        for vma, sec, text, label in engine_hits:
            f.write(f"0x{vma:08X}  {sec:<10}  {label:<40}  {text[:80]}\n")
    print(f"Wrote {len(engine_hits)} engine hints → {eng_path}")

    # C++ symbols (mangled)
    if cpp_symbols:
        cpp_path = os.path.join(out_dir, "cpp_symbols.txt")
        with open(cpp_path, "w") as f:
            f.write("# C++ mangled symbols — these are goldmines for naming functions!\n")
            f.write("# Run: c++filt <symbol> to demangle each one\n\n")
            for vma, sec, text in cpp_symbols:
                # Try to demangle inline via subprocess
                import subprocess
                try:
                    demangled = subprocess.check_output(
                        ["c++filt", text.split()[0]], text=True, timeout=2
                    ).strip()
                except Exception:
                    demangled = text
                f.write(f"0x{vma:08X}  {sec:<10}  {text.split()[0]:<50}  => {demangled}\n")
        print(f"Wrote {len(cpp_symbols)} C++ symbols → {cpp_path}")


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: string_scanner.py <main.dol> <output_dir>")
        sys.exit(1)
    dol = load(sys.argv[1])
    dol.print_info()
    print()
    scan(dol, sys.argv[2])
