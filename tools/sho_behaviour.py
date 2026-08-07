"""Extracts what each class actually *does*, so the executable can be put down.

The property tables say what a class stores. They say nothing about whether it
has behaviour -- whether it ticks, draws, reacts to messages -- and that is the
difference between a class a port can generate and one somebody has to write.

Both answers are in the vtable. A class that overrides nothing beyond the
constructor and `HandleAttributes` is pure data: create it, apply its
properties, done. A class that overrides `Update` or `HandleEvents` carries
logic, and the size of that function is a fair proxy for how much.

Two things come out of here:

  * **defaults** -- the constants a constructor writes into its own fields
    before any property is applied. Without them an object starts at zero
    instead of whatever the engine considered sensible, which is the kind of
    difference that makes a port compile and misbehave.
  * **overrides** -- which vtable slots this class fills itself rather than
    inheriting, with the size of each, which is the work list.

    python3 tools/sho_behaviour.py game-iso/SHO/SLES_551.47 \\
        --map docs/port_class_map.json --json docs/sho_behaviour.json
"""
import argparse
import json
import struct
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from attrmap import Elf
from mips import decode, R
from sho_attrs import constructor_of, vtable_of

# Enough slots to cover every override seen; entries are eight bytes, so the
# function pointers sit on the odd words.
VTABLE_WORDS = 40


def read_vtable(elf, vt, words=VTABLE_WORDS):
    """Function pointers from a GCC 2.x vtable, by entry index."""
    out = {}
    for i in range(1, words, 2):
        w = elf.word(vt + i * 4)
        if w and elf.tsa <= w < elf.tsa + elf.tsz:
            out[i] = w
    return out


def constructor_defaults(elf, ctor, limit=240):
    """Constants a constructor writes into its own object.

    Integer constants live in general registers; float constants are built in
    `$at` with lui/ori and handed to a coprocessor register by `mtc1`, so both
    files have to be tracked -- and the coprocessor destination matters, since
    several constants are staged before any of them is stored. Following only
    the last `$at` value makes every float come out the same, which is exactly
    what it did before `mtc1` was decoded.
    """
    gpr = {}
    fpr = {}
    out = []
    seen = set()
    for k in range(limit):
        va = ctor + k * 4
        w = elf.insn(va)
        if w is None:
            break
        txt, kind, det = decode(w, va)
        op = txt.split()[0]

        if kind == 'lui':
            gpr[det[0]] = det[1]
        elif kind == 'imm' and det[1] in gpr:
            gpr[det[0]] = (gpr[det[1]] + det[2]) & 0xFFFFFFFF
        elif op == 'addiu' and det and det[1] == 0:
            gpr[det[0]] = det[2] & 0xFFFFFFFF
        elif op == 'ori' and det and det[1] in gpr:
            gpr[det[0]] = (gpr[det[1]] | (det[2] & 0xFFFF)) & 0xFFFFFFFF
        elif kind == 'mtc1':
            fdst, gsrc = det
            if gsrc in gpr:
                fpr[fdst] = gpr[gsrc]

        if op in ('sw', 'swc1') and '($s0)' in txt:
            off_text = txt.split(',')[1].split('(')[0].strip()
            try:
                off = int(off_text, 16) if off_text.startswith('0x') else int(off_text)
            except ValueError:
                continue
            if off in seen:
                continue
            src = txt.split()[1].rstrip(',')
            if op == 'swc1':
                reg = int(src.lstrip('$f'))
                if reg in fpr:
                    val = struct.unpack('<f', struct.pack('<I', fpr[reg]))[0]
                    out.append({'offset': f'0x{off:x}', 'type': 'float',
                                'value': round(val, 6)})
                    seen.add(off)
            else:
                name = src.lstrip('$')
                if name == 'zero':
                    out.append({'offset': f'0x{off:x}', 'type': 'int', 'value': 0})
                    seen.add(off)
                else:
                    idx = None
                    for i, n in enumerate(R):
                        if n == name:
                            idx = i
                            break
                    if idx is not None and idx in gpr:
                        val = gpr[idx]
                        # A store of something that lands inside the loaded
                        # image is a pointer -- the vtable, a string, another
                        # object -- not a value a port should initialise a field
                        # with. CFogConfig writes its vtable at 0x693AB0, which
                        # read as a default is the meaningless integer 6896304.
                        if elf.tsa <= val < 0x00800000:
                            continue
                        out.append({'offset': f'0x{off:x}', 'type': 'int',
                                    'value': val})
                        seen.add(off)
        if op == 'jr':
            break
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('elf')
    ap.add_argument('--map', default='docs/port_class_map.json')
    ap.add_argument('--json')
    args = ap.parse_args()

    elf = Elf(args.elf)
    classes = json.load(open(args.map))['classes']

    raw = {}
    for name, info in sorted(classes.items()):
        factory = int(info['factory'], 16)
        ctor = constructor_of(elf, factory)
        if not ctor:
            continue
        vts = vtable_of(elf, ctor)
        if not vts:
            continue
        vt = vts[0]
        raw[name] = {
            'constructor': f'0x{ctor:08X}',
            'vtable': f'0x{vt:08X}',
            'slots': read_vtable(elf, vt),
            'defaults': constructor_defaults(elf, ctor),
        }

    # A function that fills the same slot in many classes is inherited, not
    # written for any of them. Counting is what separates the two without
    # needing to know the base class.
    per_slot = {}
    for v in raw.values():
        for slot, addr in v['slots'].items():
            per_slot.setdefault(slot, Counter())[addr] += 1

    out = {}
    for name, v in raw.items():
        own = {}
        for slot, addr in v['slots'].items():
            shared = per_slot[slot][addr]
            if shared == 1:
                own[str(slot)] = f'0x{addr:08X}'
        out[name] = {
            'constructor': v['constructor'],
            'vtable': v['vtable'],
            'overrides': own,
            'pure_data': len(own) == 0,
            'defaults': v['defaults'],
        }

    pure = sum(1 for v in out.values() if v['pure_data'])
    withdef = sum(1 for v in out.values() if v['defaults'])
    print(f'{len(out)} classes analysed')
    print(f'  pure data (no behaviour of their own): {pure}')
    print(f'  with behaviour to port               : {len(out) - pure}')
    print(f'  with constructor defaults recovered  : {withdef}')

    ranked = sorted(((len(v['overrides']), n) for n, v in out.items()), reverse=True)
    print('\nmost behaviour, by number of own vtable slots:')
    for c, n in ranked[:12]:
        if c:
            print(f'   {n:30s} {c} overrides, {len(out[n]["defaults"])} defaults')

    if args.json:
        json.dump(out, open(args.json, 'w'), indent=1)
        print(f'\nwrote {args.json}')


if __name__ == '__main__':
    main()
