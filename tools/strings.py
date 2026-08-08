"""Read and rebuild the game's UI string tables (`Strings.Eng` and friends).

Format, worked out from the six shipped languages:

    u32   version         2 in every retail file
    u32   count           2115 in every retail file
    count x {
        u32 hash          the id the game looks a string up by
        u32 charOffset    offset into the blob, in UTF-16 units, not bytes
    }
    UTF-16LE blob, each string NUL-terminated

**All six languages carry the same hashes in the same order.** That is what
makes a new translation a drop-in: keep the table, replace the text, recompute
the offsets. Nothing needs to know what the hash function is.

Two things live inside the text itself:

* every string starts with `\\x01\\x01`, kept verbatim -- it is not part of the
  visible text and dropping it shifts everything;
* `\\x03` followed by one byte is a glyph placeholder -- `\\x03\\x05` is a face
  button, `\\x03\\x10` the platform trademark. They are written as `{03:05}` in
  the dump so a translator can move them without a hex editor.

    python3 tools/strings.py dump  game-iso/SHO/SH.ARC Strings.Eng out.tsv
    python3 tools/strings.py build out.tsv Strings.Ukr
"""
import argparse
import struct
import sys
import zlib


def read_entry(archive, want):
    d = open(archive, 'rb').read()
    _, n, _, ntOff, ntSize = struct.unpack_from('<4sIIII', d, 0)
    nt = d[ntOff:ntOff + ntSize]
    for i in range(n):
        no, off, cs, us = struct.unpack_from('<IIII', d, 0x14 + i * 16)
        if nt[no:nt.index(b'\0', no)].decode('latin1') == want:
            return zlib.decompress(d[off:off + cs]) if us else d[off:off + cs]
    raise SystemExit(f'no entry named {want}')


def escape(s):
    """Text -> one safe TSV field.

    Single pass, because the glyph placeholder swallows the byte after it: a
    two-pass version paired the marker with the wrong character whenever that
    byte was itself a newline or a tab, and quietly corrupted 80 of the 2115
    strings.
    """
    out = []
    i = 0
    while i < len(s):
        if s[i] == '\x03' and i + 1 < len(s):
            out.append('{03:%02x}' % ord(s[i + 1]))
            i += 2
            continue
        c = s[i]
        if c == '\\':
            out.append('\\\\')
        elif c == '\n':
            out.append('\\n')
        elif c == '\t':
            out.append('\\t')
        elif ord(c) < 0x20:
            # Everything else that could break a line or a column. The item
            # names carry a bare \r -- "a \x01\rportable TV" -- and Python's
            # universal newlines split the row there, silently dropping 74
            # strings on the way back in.
            out.append('\\x%02x' % ord(c))
        else:
            out.append(c)
        i += 1
    return ''.join(out)


def unescape(s):
    out = []
    i = 0
    while i < len(s):
        if s.startswith('{03:', i) and i + 7 <= len(s) and s[i + 6] == '}':
            out.append('\x03')
            out.append(chr(int(s[i + 4:i + 6], 16)))
            i += 7
            continue
        if s[i] == '\\' and i + 1 < len(s):
            c = s[i + 1]
            if c == 'x' and i + 4 <= len(s):
                out.append(chr(int(s[i + 2:i + 4], 16)))
                i += 4
                continue
            out.append('\n' if c == 'n' else '\t' if c == 't' else c)
            i += 2
            continue
        out.append(s[i])
        i += 1
    return ''.join(out)


def parse(blob):
    ver, count = struct.unpack_from('<II', blob, 0)
    table = 8
    text = table + count * 8
    rows = []
    for i in range(count):
        h, o = struct.unpack_from('<II', blob, table + i * 8)
        start = text + o * 2
        end = blob.find(b'\x00\x00', start)
        if (end - start) % 2:
            end += 1
        rows.append((h, blob[start:end].decode('utf-16-le', 'replace')))
    return ver, rows


def cmd_dump(args):
    ver, rows = parse(read_entry(args.archive, args.entry))
    with open(args.out, 'w', encoding='utf-8', newline='\n') as f:
        f.write(f'# version\t{ver}\n')
        f.write('# hash\tprefix\ttext\n')
        for h, s in rows:
            prefix, body = s[:2], s[2:]
            f.write(f'{h:08x}\t{escape(prefix)}\t{escape(body)}\n')
    print(f'{len(rows)} strings -> {args.out}')


def cmd_build(args):
    rows = []
    ver = 2
    for line in open(args.tsv, encoding='utf-8', newline='\n'):
        line = line.rstrip('\n')
        if line.startswith('# version'):
            ver = int(line.split('\t')[1])
            continue
        if not line or line.startswith('#'):
            continue
        parts = line.split('\t')
        if len(parts) < 3:
            print(f'skipping malformed line: {line[:60]}', file=sys.stderr)
            continue
        rows.append((int(parts[0], 16), unescape(parts[1]) + unescape(parts[2])))

    # Identical strings share one copy, exactly as the retail files do.
    blob = bytearray()
    seen = {}
    table = bytearray()
    for h, s in rows:
        if s not in seen:
            seen[s] = len(blob) // 2
            blob += s.encode('utf-16-le') + b'\x00\x00'
        table += struct.pack('<II', h, seen[s])

    out = struct.pack('<II', ver, len(rows)) + bytes(table) + bytes(blob)
    open(args.out, 'wb').write(out)
    print(f'{len(rows)} strings, {len(seen)} unique -> {args.out} ({len(out)} bytes)')


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)

    p = sub.add_parser('dump', help='archive entry -> editable TSV')
    p.add_argument('archive')
    p.add_argument('entry')
    p.add_argument('out')
    p.set_defaults(func=cmd_dump)

    p = sub.add_parser('build', help='edited TSV -> string table')
    p.add_argument('tsv')
    p.add_argument('out')
    p.set_defaults(func=cmd_build)

    args = ap.parse_args()
    args.func(args)


if __name__ == '__main__':
    main()
