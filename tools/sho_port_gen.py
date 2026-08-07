#!/usr/bin/env python3
"""
sho_port_gen.py  –  Silent Hill Origins PS2 → C++ port code generator.

Reads SLES_551.47 and docs/port_class_map.json, then for every class:

  1.  Emits a C++ header (.h) with:
      – struct layout deduced from property field offsets and types
      – HandleAttributes() override with every recovered case in-line
      – factory function declaration (for the registry table)
      – all raw addresses as comments so code reviewers can verify

  2.  Emits a C++ stub (.cpp) with:
      – factory function body (placement-new idiom from the original)
      – HandleAttributes() body with numbered TODOs for unknown stores
      – setter stubs for every called function that has a mangled name
        (we demangle those and emit a stub prototype)

  3.  Emits a single engine/class_registry.cpp that reconstructs the
      global registrar table the game used at run-time.

  4.  Emits engine/rws_types.h with every RWS chunk-ID enum and the
      standard RWS attribute command types that appear in the call graph.

Everything is keyed to the port_class_map.json that attrmap.py produced,
so re-running attrmap.py and then this script keeps the port up to date.

Usage:
    python3 tools/sho_port_gen.py SLES_551.47 \\
        --map  docs/port_class_map.json \\
        --out  port/src
"""
from __future__ import annotations

import argparse
import json
import os
import re
import struct
import sys
import textwrap
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

sys.path.insert(0, str(Path(__file__).parent))
from sles import Sles           # existing loader
from mips import decode         # existing disassembler

# ─────────────────────────────────────────────────────────────────────────────
# Demangling helpers  (same logic as attrmap.py, extended with full method name)
# ─────────────────────────────────────────────────────────────────────────────

def _eat_len_name(body: str, i: int) -> tuple[str, int]:
    j = i
    while j < len(body) and body[j].isdigit():
        j += 1
    if j == i:
        return "", i
    n = int(body[i:j])
    return body[j:j+n], j+n


def demangle_symbol(sym: str) -> tuple[str, str]:
    """Returns (class_name, method_name) from a GCC 2.x mangled symbol.

    Handles both the plain and Q-qualified (nested namespace) forms.
    Returns (sym, sym) if parsing fails.
    """
    if '__' not in sym:
        return sym, sym
    method_raw, body = sym.split('__', 1)
    if body.startswith('C'):
        body = body[1:]

    # Qualified name: Qn<l1name><l2name>…
    if body.startswith('Q'):
        i = 1
        if i < len(body) and body[i] == '_':
            j = body.index('_', i+1)
            count = int(body[i+1:j]); i = j+1
        else:
            count = int(body[i]); i += 1
        parts = []
        for _ in range(count):
            name, i = _eat_len_name(body, i)
            if not name:
                return sym, method_raw
            parts.append(name)
        return parts[-1], method_raw      # innermost is the class

    # Simple: N<name>
    name, _ = _eat_len_name(body, 0)
    return (name or sym), method_raw


def demangle_setter(sym: str) -> str:
    """Best-effort human readable name for a setter symbol."""
    cls, method = demangle_symbol(sym)
    # strip trailing argument types (everything after first non-alpha after method)
    m = re.match(r'^([A-Za-z_][A-Za-z0-9_]*)', method)
    mname = m.group(1) if m else method
    return f"{cls}::{mname}"


# ─────────────────────────────────────────────────────────────────────────────
# C++ type helpers
# ─────────────────────────────────────────────────────────────────────────────

# Observations of the real data, produced by tools/property_observations.py.
# Optional: the generator still works without it, the comments are just poorer.
DECOMPILED: dict = {}

try:
    with open('docs/property_observations.json') as _f:
        OBSERVATIONS = json.load(_f)
except Exception:
    OBSERVATIONS = {}

TYPE_MAP = {
    'float':  'float',
    'int':    'int32_t',
    'short':  'int16_t',
    'byte':   'int8_t',
    'long':   'int64_t',
    'vector': '__attribute__((aligned(16))) int32_t[4]',  # PS2 128-bit
}

ATTR_READER_MAP = {
    'float':  'ReadFloat',
    'int':    'ReadInt32',
    'short':  'ReadInt16',
    'byte':   'ReadInt8',
    'long':   'ReadInt64',
    'vector': 'ReadVector128',
}


# ─────────────────────────────────────────────────────────────────────────────
# Data classes
# ─────────────────────────────────────────────────────────────────────────────

@dataclass
class Store:
    offset: Optional[str]
    type:   Optional[str]
    calls:  Optional[str]  # raw mangled symbol


@dataclass
class Property:
    index:  int
    case:   Optional[str]
    stores: list[Store]


@dataclass
class ClassInfo:
    name:       str
    size:       int
    factory:    str
    registry:   str
    gr_address: Optional[str]
    dispatch:   Optional[str]
    properties: list[Property]

    @property
    def has_attrs(self) -> bool:
        return bool(self.properties)

    @property
    def field_name(self) -> str:
        # turn CamelCase into lower_snake for struct members
        s = re.sub(r'([A-Z])', r'_\1', self.name).lstrip('_').lower()
        return s


# ─────────────────────────────────────────────────────────────────────────────
# Factory function analysis
# ─────────────────────────────────────────────────────────────────────────────

def analyse_factory(sles: Sles, factory_va: int, class_size: int) -> dict:
    """Walk the factory function to recover vtable pointer and ctor chain."""
    result = {'vtable': None, 'ctor_calls': [], 'alloc_size': class_size}
    if factory_va == 0:
        return result

    # Walk up to 128 instructions from factory start
    # Look for: lui/addiu pair (vtable ptr), jal (ctor calls)
    lui_vals: dict[int, int] = {}
    for k in range(128):
        va = factory_va + k * 4
        w = sles._word(va)
        if w is None:
            break
        txt, kind, det = decode(w, va)
        op = txt.split()[0]

        if kind == 'lui' and det:
            lui_vals[det[0]] = det[1]
        elif kind == 'imm' and det and det[1] in lui_vals:
            addr = (lui_vals[det[1]] + det[2]) & 0xFFFFFFFF
            # A vtable pointer is written to offset 0 of the new object
            if op in ('addiu', 'ori') and 'sw' not in op:
                # candidate: resolve if it's in .rodata or .data
                if result['vtable'] is None:
                    result['vtable'] = f'0x{addr:08X}'
        elif op == 'jal' and det:
            result['ctor_calls'].append(f'0x{det:08X}')
        elif op == 'jr':
            break

    return result


# ─────────────────────────────────────────────────────────────────────────────
# SLES helper extension — we need _word() not in the original sles.py
# ─────────────────────────────────────────────────────────────────────────────

def _sles_word(self, va: int) -> Optional[int]:
    off = self.va2off(va)
    if off is None:
        return None
    if off + 4 > len(self.d):
        return None
    return struct.unpack_from('<I', self.d, off)[0]

Sles._word = _sles_word  # monkey-patch


# ─────────────────────────────────────────────────────────────────────────────
# Code emitters
# ─────────────────────────────────────────────────────────────────────────────

HEADER_GUARD_RE = re.compile(r'[^A-Z0-9]')

def guard(name: str) -> str:
    return 'PORT_' + HEADER_GUARD_RE.sub('_', name.upper()) + '_H'


def emit_header(cls: ClassInfo, factory_info: dict, out_dir: Path):
    """Emit include/engine/<ClassName>.h"""
    path = out_dir / 'include' / 'engine' / f'{cls.name}.h'
    path.parent.mkdir(parents=True, exist_ok=True)

    lines = []
    g = guard(cls.name)
    lines += [
        f'#pragma once',
        f'#ifndef {g}',
        f'#define {g}',
        '',
        '// Auto-generated by sho_port_gen.py — DO NOT EDIT BY HAND',
        f'// Source: SLES_551.47  factory @ {cls.factory}  size={cls.size}',
        f'// Ghost Rider cross-ref: {cls.gr_address or "n/a"}',
        '',
        '#include <cstdint>',
        '#include <new>',
        '#include "engine/rws_types.h"',
        '#include "engine/CGameObject.h"',
        '',
        f'namespace SHO {{',
        '',
    ]

    # ── struct layout ────────────────────────────────────────────────────────
    lines += [
        f'/// @brief {cls.name}',
        f'/// Instance size: {cls.size} bytes (from SHO class registry)',
    ]
    if cls.dispatch:
        lines.append(f'/// Attribute dispatch: {cls.dispatch}')

    lines += [
        f'struct {cls.name} : public CGameObject {{',
        '',
        f'    // ── Fields recovered from HandleAttributes property stores ──',
    ]

    # What the shipped data says about each property index, keyed by the class's
    # own component. The code gives the offset and the width; only the archive
    # can say whether a slot holds a designer-typed name, a placement matrix, or
    # a value nobody ever changed from its default.
    obs = OBSERVATIONS.get(cls.name, {}).get(cls.name, {})

    seen_offsets: set[str] = set()
    for prop in cls.properties:
        note = ''
        o = obs.get(str(prop.index))
        if o:
            if o.get('constant'):
                top = o['top'][0][0] if o.get('top') else '?'
                note = f"  [{o['kind']}, constant {top} across {o['seen']}]"
            else:
                note = f"  [{o['kind']}, {o['distinct']} distinct over {o['seen']}]"
                if o.get('top'):
                    vals = ', '.join(str(v) for v, _ in o['top'][:3])
                    note += f" e.g. {vals}"
        for store in prop.stores:
            if store.offset and store.offset not in seen_offsets:
                seen_offsets.add(store.offset)
                cpp_type = TYPE_MAP.get(store.type or 'int', 'int32_t')
                off_int  = int(store.offset, 16) if store.offset.startswith('0x') else int(store.offset)
                lines.append(
                    f'    {cpp_type:<30} field_{off_int:04X};'
                    f'  ///< prop {prop.index} @ {store.offset} ({store.type}){note}'
                )

    if not seen_offsets:
        lines.append('    // No field stores recovered — all properties call setters')

    lines += [
        '',
        '    // ── Lifecycle ───────────────────────────────────────────────',
        f'    static {cls.name}* Create();           ///< factory @ {cls.factory}',
        f'    static void         Register();        ///< registry @ {cls.registry}',
        '',
    ]

    # ── HandleAttributes ────────────────────────────────────────────────────
    if cls.has_attrs:
        lines += [
            '    // ── Attribute binding ───────────────────────────────────────',
            '    /// Dispatched from CGameObject::HandleAttributes.',
            f'    /// {len(cls.properties)} properties recovered via {cls.dispatch}.',
            '    void HandleAttributes(RWS::CAttributeCommandIterator& cmd) override;',
            '',
        ]

    # setter forward declarations
    setter_syms: set[str] = set()
    for prop in cls.properties:
        for store in prop.stores:
            if store.calls:
                setter_syms.add(store.calls)

    if setter_syms:
        lines += [
            '    // ── Setters (called from HandleAttributes) ──────────────────',
        ]
        for sym in sorted(setter_syms):
            pretty = demangle_setter(sym)
            lines.append(f'    // {pretty}  (mangled: {sym})')

    lines += [
        '',
        f'}};  // struct {cls.name}',
        '',
        f'}}  // namespace SHO',
        '',
        f'#endif  // {g}',
        '',
    ]

    path.write_text('\n'.join(lines))
    return path


def emit_source(cls: ClassInfo, factory_info: dict, out_dir: Path):
    """Emit src/engine/<ClassName>.cpp"""
    path = out_dir / 'src' / 'engine' / f'{cls.name}.cpp'
    path.parent.mkdir(parents=True, exist_ok=True)

    lines = []
    lines += [
        f'// Auto-generated by sho_port_gen.py — fill in the TODOs',
        f'// Source: SLES_551.47  factory @ {cls.factory}',
        '',
        f'#include "engine/{cls.name}.h"',
        '#include <new>',
        '#include "engine/CMemory.h"',
        '#include "engine/CGameObjectRegistry.h"',
        '#include "rws/CAttributeCommandIterator.h"',
        '',
        f'namespace SHO {{',
        '',
        '// ─────────────────────────────────────────────────────────────────',
        f'// Factory',
        '// ─────────────────────────────────────────────────────────────────',
        '',
        f'{cls.name}* {cls.name}::Create() {{',
        f'    // Original factory @ {cls.factory}',
    ]

    if factory_info.get('vtable'):
        lines.append(f'    // vtable candidate: {factory_info["vtable"]}')
    for c in factory_info.get('ctor_calls', [])[:4]:
        lines.append(f'    // calls ctor @ {c}')

    lines += [
        f'    void* mem = CMemory::Alloc({cls.size});',
        f'    if (!mem) return nullptr;',
        f'    return new (mem) {cls.name}();',
        '}',
        '',
        f'void {cls.name}::Register() {{',
        f'    // Original registry record @ {cls.registry}',
        # The factory is typed to the concrete class for callers' convenience,
        # so it needs a cast to the registry's CGameObject* signature.
        f'    CGameObjectRegistry::Register("{cls.name}",',
        f'        reinterpret_cast<CGameObjectRegistry::FactoryFn>(Create), {cls.size});',
        '}',
        '',
    ]

    if cls.has_attrs:
        lines += [
            '// ─────────────────────────────────────────────────────────────────',
            '// HandleAttributes',
            '// ─────────────────────────────────────────────────────────────────',
            '',
            f'void {cls.name}::HandleAttributes(RWS::CAttributeCommandIterator& cmd) {{',
            '    // Each case is one attribute record from the container (0x0704).',
            '    // The index comes from the low bits of record[1].',
            '    const uint32_t idx = cmd.GetCommandId();',
            '    switch (idx) {',
        ]

        for prop in cls.properties:
            lines.append(f'    case {prop.index}:  // @ {prop.case}')
            if prop.stores:
                for store in prop.stores:
                    if store.offset:
                        reader = ATTR_READER_MAP.get(store.type or 'int', 'ReadInt32')
                        lines.append(f'        field_{int(store.offset,16) if store.offset.startswith("0x") else int(store.offset):04X}'
                                     f' = cmd.{reader}();')
                    elif store.calls:
                        pretty = demangle_setter(store.calls)
                        lines += [
                            f'        // TODO: call {pretty}',
                            f'        // Mangled: {store.calls}',
                            f'        // {pretty}(cmd.ReadRaw());',
                        ]
            else:
                lines.append(f'        // TODO: no stores recovered — inspect @ {prop.case}')
                lines.append(f'        break;')
            if prop.stores:
                lines.append('        break;')

        lines += [
            '    default:',
            '        CGameObject::HandleAttributes(cmd);',
            '        break;',
            '    }',
            '}',
            '',
        ]

    lines += [
        f'}}  // namespace SHO',
        '',
    ]

    # The original, decompiled from the retail executable. Kept as a comment so
    # the file still compiles: Ghidra's output names functions FUN_xxxxxx and
    # types undefined4, so it is a reference to port against, not code to build.
    original = DECOMPILED.get(cls.name)
    if original:
        lines += [
            '#if 0',
            '// ─────────────────────────────────────────────────────────────────',
            '// Original HandleAttributes, decompiled from SLES_551.47.',
            '// Produced by tools/GhidraDecompile.java at the address',
            f'// tools/sho_attrs.py recovered for {cls.name}.',
            '// ─────────────────────────────────────────────────────────────────',
            original.rstrip(),
            '#endif',
            '',
        ]

    path.write_text('\n'.join(lines))
    return path


def emit_registry(classes: list[ClassInfo], out_dir: Path):
    """Emit src/engine/class_registry.cpp"""
    path = out_dir / 'src' / 'engine' / 'class_registry.cpp'
    path.parent.mkdir(parents=True, exist_ok=True)

    lines = [
        '// Auto-generated by sho_port_gen.py',
        '// Reconstructs the SHO global class registry.',
        '// Original registrar @ 0x001FE298 (SLES_551.47)',
        '',
        '#include "engine/CGameObjectRegistry.h"',
        '',
    ]
    for cls in classes:
        lines.append(f'#include "engine/{cls.name}.h"')

    lines += [
        '',
        'namespace SHO {',
        '',
        'void RegisterAllClasses() {',
    ]
    for cls in classes:
        lines.append(f'    {cls.name}::Register();'
                     f'  // factory @ {cls.factory}  size={cls.size}')
    lines += [
        '}',
        '',
        '}  // namespace SHO',
        '',
    ]

    path.write_text('\n'.join(lines))
    return path


def emit_rws_types(out_dir: Path):
    """Emit include/engine/rws_types.h with standard RWS enums."""
    path = out_dir / 'include' / 'engine' / 'rws_types.h'
    path.parent.mkdir(parents=True, exist_ok=True)
    content = textwrap.dedent("""\
        #pragma once
        // RenderWare chunk IDs and attribute types used by the Climax engine.
        // Values from RW SDK 3.6 / Silent Hill Origins container format.
        #include <cstdint>

        namespace RWS {

        // ── RW chunk type IDs ────────────────────────────────────────────────
        enum ChunkId : uint32_t {
            rwID_NAOBJECT       = 0x00000000,
            rwID_STRUCT         = 0x00000001,
            rwID_STRING         = 0x00000002,
            rwID_EXTENSION      = 0x00000003,
            rwID_CAMERA         = 0x00000005,
            rwID_TEXTURE        = 0x00000006,
            rwID_MATERIAL       = 0x00000007,
            rwID_MATLIST        = 0x00000008,
            rwID_FRAMELIST      = 0x0000000E,
            rwID_GEOMETRY       = 0x0000000F,
            rwID_CLUMP          = 0x00000010,
            rwID_LIGHT          = 0x00000012,
            rwID_WORLD          = 0x0000001B,
            rwID_GEOMETRYLIST   = 0x0000001F,
            rwID_CBSP           = 0x0000002D,
            rwID_POLYAREA       = 0x00000120,
            // Climax/RWS Audio extensions
            rwaID_WAVEDICT      = 0x080B0000,
            rwaID_SOUND         = 0x080C0000,
            rwaID_STREAM        = 0x080D0000,
            // SHO container
            rwID_SHO_GAMEOBJ    = 0x00000704,
            rwID_SHO_TYPEBLOCK  = 0x00000705,
        };

        // ── Attribute command iterator (forward declaration) ─────────────────
        class CAttributeCommandIterator {
        public:
            uint32_t GetCommandId()  const;
            float    ReadFloat()     const;
            int32_t  ReadInt32()     const;
            int16_t  ReadInt16()     const;
            int8_t   ReadInt8()      const;
            int64_t  ReadInt64()     const;
            void*    ReadRaw()       const;
            bool     MoveNext();
        };

        // ── Event handler (used by ReplaceRegisteredMsg / ReplaceLinkedMsg) ──
        class CEventHandler {};
        class CEventId {};

        }  // namespace RWS
        """)
    path.write_text(content)
    return path


def load_decompiled(path: Optional[str]) -> dict:
    """Ghidra output for the handlers, keyed by class name.

    The files are what tools/GhidraDecompile.java writes: real decompiled C for
    the very functions sho_attrs.py located. It cannot be dropped in as code --
    the names are FUN_xxxxxx and the types are undefined -- but having the
    original logic beside the generated skeleton is the difference between
    filling in a switch from guesswork and reading what the engine did.
    """
    out = {}
    if not path:
        return out
    d = Path(path)
    if not d.is_dir():
        return out
    for f in d.glob('*_HandleAttributes.c'):
        out[f.stem.replace('_HandleAttributes', '')] = f.read_text()
    return out


def emit_support_headers(out_dir: Path):
    """Emit the headers the generated code includes but nothing provided.

    Without these the output does not compile at all -- the class files include
    engine/CMemory.h, engine/CGameObjectRegistry.h and
    rws/CAttributeCommandIterator.h, and the base header includes rws_types.h by
    a path that only resolves from inside engine/. They are stubs on purpose:
    the point is that the tree builds, so the recovered per-class work can be
    filled in against something that compiles rather than against nothing.
    """
    inc = out_dir / 'include'
    (inc / 'engine').mkdir(parents=True, exist_ok=True)
    (inc / 'rws').mkdir(parents=True, exist_ok=True)

    # The class files include "rws_types.h" unqualified as well as
    # "engine/rws_types.h"; a forwarding header costs nothing and removes a
    # whole class of build breakage.
    (inc / 'rws_types.h').write_text(
        '#pragma once\n'
        '// Forwards to the real header so both include spellings resolve.\n'
        '#include "engine/rws_types.h"\n')

    (inc / 'engine' / 'CMemory.h').write_text(textwrap.dedent("""\
        #pragma once
        // Allocator stub.
        //
        // The original factories call the engine's own pooled allocator, which
        // takes a size and an alignment class (always 2 in the factories seen).
        // Nothing about the port depends on reproducing the pool yet, so this
        // forwards to the system allocator and keeps the call shape.
        #include <cstddef>
        #include <cstdlib>

        namespace SHO {

        struct CMemory {
            static void* Alloc(std::size_t size, int alignClass = 2) {
                (void)alignClass;
                return std::calloc(1, size);
            }
            static void Free(void* p) { std::free(p); }
        };

        }  // namespace SHO
        """))

    # CGameObject.h already declares the registry; a second definition here
    # is a redefinition error, so this path forwards to it.
    (inc / 'engine' / 'CGameObjectRegistry.h').write_text(
        '#pragma once\n'
        '// Declared in engine/CGameObject.h; this path forwards, because the\n'
        '// generated class files include it by this name.\n'
        '#include "engine/CGameObject.h"\n')

    # rws_types.h already declares the iterator; defining it again here is a
    # redefinition error, so this header only forwards.
    (inc / 'rws' / 'CAttributeCommandIterator.h').write_text(
        '#pragma once\n'
        '// The iterator is declared in engine/rws_types.h; this path just\n'
        '// forwards, because the generated class files include both spellings.\n'
        '#include "engine/rws_types.h"\n')

    return inc


def emit_base_game_object(out_dir: Path):
    """Emit include/engine/CGameObject.h — minimal base class stub."""
    path = out_dir / 'include' / 'engine' / 'CGameObject.h'
    path.parent.mkdir(parents=True, exist_ok=True)
    content = textwrap.dedent("""\
        #pragma once
        // Minimal CGameObject base stub — fill in from binary analysis.
        #include <cstdint>
        #include "rws_types.h"

        namespace SHO {

        class CGameObject {
        public:
            virtual ~CGameObject() = default;
            virtual void HandleAttributes(RWS::CAttributeCommandIterator& cmd);
            virtual void Update(float dt);
            virtual void Render();

            // vtable slot 0 in the PS2 binary is always the destructor.
            // Slots 1+ vary per class.

        protected:
            uint32_t m_classId;   // low 16 bits = type index in registrar
            uint32_t m_flags;
        };

        class CGameObjectRegistry {
        public:
            using FactoryFn = CGameObject* (*)();
            static void Register(const char* name, FactoryFn fn, uint32_t size);
            static CGameObject* Create(const char* name);
        };

        }  // namespace SHO
        """)
    path.write_text(content)
    return path


def emit_summary_md(classes: list[ClassInfo], out_dir: Path, sles_path: str):
    """Emit docs/PORT_CLASSES.md — human-readable port coverage table."""
    path = out_dir / 'docs' / 'PORT_CLASSES.md'
    path.parent.mkdir(parents=True, exist_ok=True)

    total    = len(classes)
    with_gr  = sum(1 for c in classes if c.gr_address)
    with_attr= sum(1 for c in classes if c.has_attrs)
    fields   = sum(sum(1 for s in p.stores if s.offset)
                   for c in classes for p in c.properties)
    setters  = sum(sum(1 for s in p.stores if s.calls)
                   for c in classes for p in c.properties)

    lines = [
        '# Silent Hill Origins — Port Class Coverage',
        '',
        f'Generated from `{sles_path}` + `docs/port_class_map.json`.',
        '',
        '## Summary',
        '',
        f'| Metric | Value |',
        f'|--------|-------|',
        f'| Total classes | {total} |',
        f'| Cross-ref with Ghost Rider | {with_gr} |',
        f'| With recovered attribute table | {with_attr} |',
        f'| Field stores recovered | {fields} |',
        f'| Setter calls recovered | {setters} |',
        '',
        '## Per-Class Coverage',
        '',
        '| Class | Size | Factory | GR? | Props | Notes |',
        '|-------|------|---------|-----|-------|-------|',
    ]

    for cls in sorted(classes, key=lambda c: c.name):
        gr  = '✓' if cls.gr_address else '–'
        pr  = str(len(cls.properties)) if cls.has_attrs else '–'
        note= ''
        if not cls.gr_address:
            note = 'SHO-only (no GR cross-ref)'
        elif not cls.has_attrs:
            note = 'GR matched, no attr table'
        lines.append(
            f'| `{cls.name}` | {cls.size} | `{cls.factory}` | {gr} | {pr} | {note} |'
        )

    path.write_text('\n'.join(lines) + '\n')
    return path


# ─────────────────────────────────────────────────────────────────────────────
# Main
# ─────────────────────────────────────────────────────────────────────────────

def parse_class_map(map_path: str) -> list[ClassInfo]:
    raw = json.loads(Path(map_path).read_text())
    classes = []
    for name, data in raw.get('classes', {}).items():
        props = []
        for p in data.get('properties') or []:
            stores = []
            for s in p.get('stores', []):
                stores.append(Store(
                    offset=s.get('offset'),
                    type=s.get('type'),
                    calls=s.get('calls'),
                ))
            # The merged map keeps both binaries' case addresses under
            # separate keys. Origins' own is the one that matters -- it is the
            # address in the executable being ported -- with Ghost Rider's as a
            # fallback for classes only it has.
            props.append(Property(
                index=p['index'],
                case=p.get('case_sho') or p.get('case_gr') or p.get('case'),
                stores=stores,
            ))
        classes.append(ClassInfo(
            name=name,
            size=data.get('size', 0),
            factory=data.get('factory', '0x00000000'),
            registry=data.get('registry', '0x00000000'),
            gr_address=data.get('gr_address'),
            dispatch=data.get('dispatch'),
            properties=props,
        ))
    return classes


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('sles',         help='Path to SLES_551.47')
    ap.add_argument('--decomp', help='directory of Ghidra-decompiled handlers')
    ap.add_argument('--map',  '-m', default='docs/port_class_map.json')
    ap.add_argument('--out',  '-o', default='port')
    ap.add_argument('--no-factory-analysis', action='store_true',
                    help='Skip slow factory function disassembly')
    args = ap.parse_args()

    print(f'[1/6] Loading {args.sles} ...')
    sles = Sles(args.sles)
    print(f'      .text @ 0x{sles.tsa:08X}  ({sles.tsz // 1024} KB)')

    print(f'[2/6] Parsing {args.map} ...')
    classes = parse_class_map(args.map)
    print(f'      {len(classes)} classes loaded')

    out = Path(args.out)
    global DECOMPILED
    DECOMPILED = load_decompiled(args.decomp)
    if DECOMPILED:
        print(f'      {len(DECOMPILED)} decompiled handlers available')

    print(f'[3/6] Emitting RWS types header ...')
    emit_support_headers(out)
    emit_rws_types(out)
    emit_base_game_object(out)

    print(f'[4/6] Analysing factories and emitting class files ...')
    written_h = written_cpp = 0
    for cls in classes:
        factory_va = int(cls.factory, 16) if cls.factory != '0x00000000' else 0
        if args.no_factory_analysis or factory_va == 0:
            factory_info = {'vtable': None, 'ctor_calls': [], 'alloc_size': cls.size}
        else:
            factory_info = analyse_factory(sles, factory_va, cls.size)

        emit_header(cls, factory_info, out)
        emit_source(cls, factory_info, out)
        written_h   += 1
        written_cpp += 1

    print(f'      {written_h} headers, {written_cpp} source files written')

    print(f'[5/6] Emitting class registry ...')
    emit_registry(classes, out)

    print(f'[6/6] Emitting PORT_CLASSES.md ...')
    emit_summary_md(classes, out, args.sles)

    # ── Print quick summary ──────────────────────────────────────────────────
    total     = len(classes)
    with_gr   = sum(1 for c in classes if c.gr_address)
    with_attr = sum(1 for c in classes if c.has_attrs)
    sho_only  = total - with_gr
    fields    = sum(sum(1 for s in p.stores if s.offset)
                    for c in classes for p in c.properties)
    setters   = sum(sum(1 for s in p.stores if s.calls)
                    for c in classes for p in c.properties)

    print(f"""
╔══════════════════════════════════════════════════════╗
║          SHO Port Code Generator — Done              ║
╠══════════════════════════════════════════════════════╣
║  Classes total          : {total:<5}                      ║
║  GR cross-ref           : {with_gr:<5}                      ║
║  With attribute table   : {with_attr:<5}                      ║
║  SHO-only (no GR data)  : {sho_only:<5}                      ║
║  Field stores in headers: {fields:<5}                      ║
║  Setter calls documented: {setters:<5}                      ║
╠══════════════════════════════════════════════════════╣
║  Output directory: {str(out):<33}║
╚══════════════════════════════════════════════════════╝
""")


if __name__ == '__main__':
    main()
