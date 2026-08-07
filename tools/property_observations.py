"""Describes each class property from the shipped data, not from the code.

The static side -- `attrmap.py` and `sho_attrs.py` -- recovers where a property
is stored and whether the store is a word or a float. That gives field offsets
and sizes, which is why the generated port headers are full of `field_0044`.

What it cannot say is what a property *means*. The archive can: every level
carries real instances, so the payloads themselves show which indices hold a
designer-typed name, which hold a placement matrix, which are booleans that are
always 0, and which were genuinely tuned per instance.

That distinction is what turns a field list into something portable. A property
whose value never changes across 400 objects is a default nobody touched; one
with a wide spread is a real knob.

    python3 tools/property_observations.py game-iso/SHO/SH.ARC \\
        --json docs/property_observations.json
"""
import argparse
import json
import struct
import zlib
from collections import defaultdict

RW_VER = 0x1C020065


def looks_like_name(pay):
    """Printable text with a terminator -- the same test the loader uses."""
    k = 0
    while k < len(pay) and 32 <= pay[k] < 127:
        k += 1
    return k >= 3 and k < len(pay) and pay[k] in (0x00, 0xBF)


def classify(pay):
    if len(pay) == 64:
        return 'matrix'
    if looks_like_name(pay):
        return 'name'
    if len(pay) == 16:
        return 'guid'
    if len(pay) == 4:
        f = struct.unpack_from('<f', pay)[0]
        i = struct.unpack_from('<i', pay)[0]
        # A float that is a small round number reads sensibly both ways; the
        # integer reading of a real float is enormous, which is the tell.
        if i in (0, 1, -1) or abs(i) < 4096:
            return 'int'
        if f == f and abs(f) < 1e6:
            return 'float'
        return 'int'
    return f'raw{len(pay)}'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('archive')
    ap.add_argument('--json')
    args = ap.parse_args()

    d = open(args.archive, 'rb').read()
    magic, n, first, ntOff, ntSize = struct.unpack_from('<4sIIII', d, 0)
    nt = d[ntOff:ntOff + ntSize]

    # class -> component -> index -> observations
    obs = defaultdict(lambda: defaultdict(lambda: defaultdict(
        lambda: {'n': 0, 'kinds': defaultdict(int), 'values': defaultdict(int)})))
    tag = struct.pack('<I', 0x0704)
    ver = struct.pack('<I', RW_VER)

    for i in range(n):
        no, off, cs, us = struct.unpack_from('<IIII', d, 0x14 + i * 16)
        name = nt[no:nt.index(b'\0', no)].decode('latin1')
        if name.endswith('.txd'):
            continue
        try:
            c = zlib.decompress(d[off:off + cs]) if us else d[off:off + cs]
        except Exception:
            continue

        pos = 0
        while True:
            o = c.find(tag, pos)
            if o < 0:
                break
            pos = o + 1
            if o + 12 > len(c) or c[o + 8:o + 12] != ver:
                continue
            size = struct.unpack_from('<I', c, o + 4)[0]
            if size == 0 or o + 12 + size > len(c):
                continue

            p, end = o + 16, o + 12 + size
            cls = comp = None
            while p + 8 <= end:
                rs, rid = struct.unpack_from('<II', c, p)
                if rs < 8 or p + rs > end:
                    break
                kind, idx = rid >> 24, rid & 0xFFFFFF
                pay = c[p + 8:p + rs]
                if kind == 0x20 and cls is None:
                    cls = pay.split(b'\0')[0].decode('latin1', 'replace')
                elif kind == 0x80:
                    comp = pay.split(b'\0')[0].decode('latin1', 'replace')
                elif kind == 0x00 and cls and comp:
                    e = obs[cls][comp][idx]
                    e['n'] += 1
                    k = classify(pay)
                    e['kinds'][k] += 1
                    if k == 'name':
                        e['values'][pay.split(b'\0')[0].decode('latin1', 'replace')] += 1
                    elif k in ('int', 'float') and len(pay) == 4:
                        v = (struct.unpack_from('<f', pay)[0] if k == 'float'
                             else struct.unpack_from('<i', pay)[0])
                        e['values'][round(v, 4) if k == 'float' else v] += 1
                p += rs
            pos = o + 12 + size

    out = {}
    for cls, comps in obs.items():
        out[cls] = {}
        for comp, idxs in comps.items():
            out[cls][comp] = {}
            for idx, e in sorted(idxs.items()):
                kinds = sorted(e['kinds'].items(), key=lambda x: -x[1])
                vals = sorted(e['values'].items(), key=lambda x: -x[1])[:6]
                out[cls][comp][str(idx)] = {
                    'seen': e['n'],
                    'kind': kinds[0][0],
                    'distinct': len(e['values']),
                    'constant': len(e['values']) == 1,
                    'top': [[str(v), c] for v, c in vals],
                }

    total = sum(len(v) for comps in out.values() for v in comps.values())
    print(f'{len(out)} classes, {total} property slots observed')
    for cls in sorted(out)[:6]:
        for comp, idxs in out[cls].items():
            varying = [i for i, e in idxs.items() if not e['constant']]
            print(f'  {cls}.{comp}: {len(idxs)} props, {len(varying)} actually vary')

    if args.json:
        json.dump(out, open(args.json, 'w'), indent=1)
        print(f'\nwrote {args.json}')


if __name__ == '__main__':
    main()
