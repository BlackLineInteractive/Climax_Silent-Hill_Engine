"""Pull the game's script and UI files out of SH.ARC.

Silent Hill Origins keeps its entire front end, its puzzles and its cutscene
timing in plain XML inside the archive -- 40 files, none of them compiled. The
executable supplies the widget behaviour; the screens themselves are data.

    python3 tools/extract_scripts.py game-iso/SHO/SH.ARC out/scripts

Writes every entry whose name ends in .xml, .txt or .cfg, plus a summary of
what each one appears to define.
"""
import argparse
import os
import re
import struct
import zlib

# What each file governs, from reading them. Recorded here because the names
# alone do not say which are screens, which are puzzle definitions, and which
# are the developers' own tools left in the retail build.
NOTES = {
    'mainmenu.xml': 'the main menu: buttons, textures, navigation',
    'pausemenu.xml': 'the in-game pause menu',
    'newgame.xml': 'difficulty and new-game options',
    'gameoptions.xml': 'options screen',
    'extraoptions.xml': 'extras / unlockables screen',
    'inventory.xml': 'inventory screen layout',
    'notes.xml': 'notes screen',
    'mapviewer.xml': 'map screen',
    'examine.xml': 'item examine view',
    'ingame_examine.xml': 'examine view during play',
    'note_examine.xml': 'reading a note',
    'controls_norm.xml': 'control diagram, exploration',
    'controls_combat.xml': 'control diagram, combat',
    'credits.xml': 'credits roll',
    'ratings.xml': 'end-of-game rating screen',
    'accolades.xml': 'accolade definitions',
    'hints.xml': 'hint text',
    'endgame.xml': 'ending selection',
    'bootmenu.xml': "the developers' level-warp menu, left in the retail build",
    'bootmenuMS.xml': 'a second warp menu',
    'igcscript.xml': 'cutscene timing and subtitles',
    'anatomypuzzle.xml': 'puzzle definition',
    'bkdropproppuzzle.xml': 'puzzle definition',
    'calenderpuzzle.xml': 'puzzle definition',
    'circuitbrkpuzzle.xml': 'puzzle definition',
    'flaurouspuzzle.xml': 'puzzle definition',
    'ironlungpuzzle.xml': 'puzzle definition',
    'laundrypuzzle.xml': 'puzzle definition',
    'organboxpuzzle.xml': 'puzzle definition',
    'pilldollpuzzle.xml': 'puzzle definition',
    'tillpuzzle.xml': 'puzzle definition',
    'hospitallift.xml': 'lift panel',
    'peephole.xml': 'door peephole view',
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('archive')
    ap.add_argument('outdir')
    args = ap.parse_args()

    d = open(args.archive, 'rb').read()
    _, n, _, ntOff, ntSize = struct.unpack_from('<4sIIII', d, 0)
    nt = d[ntOff:ntOff + ntSize]
    os.makedirs(args.outdir, exist_ok=True)

    written = 0
    for i in range(n):
        no, off, cs, us = struct.unpack_from('<IIII', d, 0x14 + i * 16)
        name = nt[no:nt.index(b'\0', no)].decode('latin1')
        if not name.lower().endswith(('.xml', '.txt', '.cfg')):
            continue
        try:
            blob = zlib.decompress(d[off:off + cs]) if us else d[off:off + cs]
        except zlib.error:
            print(f'  {name}: will not inflate, skipped')
            continue

        open(os.path.join(args.outdir, name), 'wb').write(blob)
        written += 1

        # A one-line description: the note if we have one, otherwise the root
        # element and how many children it has.
        note = NOTES.get(name)
        if not note:
            m = re.search(rb'<([A-Za-z_]+)[ >]', blob)
            root = m.group(1).decode('latin1') if m else '?'
            kids = len(re.findall(rb'<[A-Za-z_]+[ >]', blob))
            note = f'<{root}> with {kids} elements'
        print(f'{len(blob):8d}  {name:<24} {note}')

    print(f'\n{written} files -> {args.outdir}')


if __name__ == '__main__':
    main()
