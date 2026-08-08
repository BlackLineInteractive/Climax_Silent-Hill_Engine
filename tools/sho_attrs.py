"""Recovers property tables from Origins' own stripped executable.

`attrmap.py` works on Ghost Rider, where every `HandleAttributes` has a symbol.
Origins is stripped, so 63 of its 126 classes -- the ones Climax wrote for
Silent Hill and that have no Ghost Rider counterpart -- have no name to look up:
`CIGCCamera`, `CIGCCharacter`, `CPickupItem`, `CInventoryItemDef`, `CMapItem`,
`CFogConfig`, `CCalibanBehaviour`, `AnatomyPuzzleTrigger` and the rest.

They can still be reached, because the class registry gives every class a
factory address, and the path from there is fixed:

    factory  --jal-->  constructor  --stores-->  vtable  --slot 7-->  HandleAttributes

Slot 7 is not a guess. In Ghost Rider, where the vtables *are* named, 35 of the
36 classes that have both a primary vtable and a `HandleAttributes` symbol carry
it at word 7; the one exception, `CSimpleAnimation`, is not an attribute handler
at all. GCC 2.x vtable entries are eight bytes, so the function pointers land on
odd words -- 3 is the destructor, 5 `PostCreate`, 7 `HandleAttributes`.

Once the address is known the dispatch is read exactly as in Ghost Rider, since
both binaries come from the same compiler and the same engine.

    python3 tools/sho_attrs.py game-iso/SHO/SLES_551.47 \\
        --map docs/generated/port_class_map.json --json docs/generated/sho_attribute_map.json
"""
import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from attrmap import Elf, find_table, find_case_chain, object_register, read_case
from mips import decode

VTABLE_SLOT = 7


def constructor_of(elf, factory, limit=32):
    """The constructor a factory hands its fresh allocation to.

    A factory is always `new(size, 2)` then a call with the result in $a0, so
    the constructor is the second `jal` -- the first one is the allocator.
    """
    calls = []
    for k in range(limit):
        w = elf.insn(factory + k * 4)
        if w is None:
            break
        txt, kind, det = decode(w, factory + k * 4)
        if txt.startswith('jal'):
            calls.append(det)
        if txt.startswith('jr'):
            break
    return calls[1] if len(calls) > 1 else None


def vtable_of(elf, ctor, limit=64):
    """Addresses the constructor materialises that look like vtables.

    A vtable is built with lui/addiu into the data segment and then stored into
    the object, so track register values and keep the ones that point at a word
    which is itself a plausible code address.
    """
    regs = {}
    out = []
    for k in range(limit):
        va = ctor + k * 4
        w = elf.insn(va)
        if w is None:
            break
        txt, kind, det = decode(w, va)
        if kind == 'lui':
            regs[det[0]] = det[1]
        elif kind == 'imm' and det[1] in regs:
            val = (regs[det[1]] + det[2]) & 0xFFFFFFFF
            regs[det[0]] = val
            probe = elf.word(val + VTABLE_SLOT * 4)
            if probe and elf.tsa <= probe < elf.tsa + elf.tsz:
                out.append(val)
        if txt.startswith('jr'):
            break
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('elf')
    ap.add_argument('--map', default='docs/generated/port_class_map.json')
    ap.add_argument('--json')
    args = ap.parse_args()

    elf = Elf(args.elf)
    classes = json.load(open(args.map))['classes']

    result = {}
    hit = miss = 0
    for name, info in sorted(classes.items()):
        factory = int(info['factory'], 16)
        ctor = constructor_of(elf, factory)
        if not ctor:
            miss += 1
            continue

        handler = None
        for vt in vtable_of(elf, ctor):
            cand = elf.word(vt + VTABLE_SLOT * 4)
            if not cand:
                continue
            count, table = find_table(elf, cand, 0x400)
            objreg = object_register(elf, cand, 0x400)
            if count and table:
                props = []
                for i in range(count):
                    tgt = elf.word(table + i * 4)
                    props.append({'index': i,
                                  'case': f'0x{tgt:08X}' if tgt else None,
                                  'stores': read_case(elf, tgt, objreg) if tgt else []})
                handler = {'address': f'0x{cand:08X}', 'vtable': f'0x{vt:08X}',
                           'count': count, 'properties': props,
                           'dispatch': 'jump table'}
                break
            cases = find_case_chain(elf, cand, 0x400)
            if cases:
                handler = {'address': f'0x{cand:08X}', 'vtable': f'0x{vt:08X}',
                           'count': len(cases),
                           'properties': [{'index': i, 'case': f'0x{cases[i]:08X}',
                                           'stores': read_case(elf, cases[i], objreg)}
                                          for i in sorted(cases)],
                           'dispatch': 'compare chain'}
                break

        if handler:
            result[name] = handler
            hit += 1
        else:
            miss += 1

    # A handler reached from several classes is an inherited one, not that
    # class's own: the walk found a base vtable. Those are worse than useless
    # in a port, because they look like data. Checked against Ghost Rider, every
    # such case reports exactly one property while the real class has many.
    from collections import Counter
    shared = {a for a, c in Counter(v['address'] for v in result.values()).items()
              if c > 1}
    for name in list(result):
        if result[name]['address'] in shared:
            result[name]['inherited'] = True

    own = sum(1 for v in result.values() if not v.get('inherited'))
    print(f'{len(classes)} classes: {hit} handlers found, '
          f'{own} of them the class\'s own, {hit - own} inherited from a base')
    for name in sorted(result)[:20]:
        r = result[name]
        print(f'  {name:26s} {r["dispatch"]:14s} {r["count"]:2d} props  '
              f'handler={r["address"]}')

    if args.json:
        json.dump(result, open(args.json, 'w'), indent=1)
        print(f'\nwrote {args.json}')


if __name__ == '__main__':
    main()
