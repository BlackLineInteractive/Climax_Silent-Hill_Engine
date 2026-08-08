"""Convert the game's `.PSS` movies to MP4, at the aspect they were meant for.

`.PSS` is a Sony MPEG-2 program stream. Every one of the 58 is stored **512x512
with a square sample aspect**, and that is a lie the PS2 corrects on output: the
`W` files are widescreen and the `N` files are 4:3, and nothing in the container
says so. Play one in an ordinary player and it comes out squeezed -- which is
exactly what it looks like in the game until the stretch is applied.

So the conversion has to state the aspect, and state it once. Scaling the pixels
and *also* setting a sample aspect ratio cancels out: a first pass here produced
910x512 with SAR 256:455, whose display aspect is back to 1:1 and just as
squeezed as the original. `setsar=1` after the scale is what makes it stick.

    python3 tools/convert_movies.py game-iso/SHO/MOVIES SHO-port/MOVIES

Names encode the variant: <NAME><W|N><lang>.PSS, W widescreen and N 4:3, with
no language suffix meaning English and F/G/I/S the four translations.
"""
import argparse
import os
import re
import subprocess
import sys

# 512 tall at each aspect, rounded to an even width as H.264 requires.
TARGET = {
    'W': (910, 512),   # 16:9
    'N': (682, 512),   # 4:3
}


def variant(stem):
    """W or N, from the letter after the base name.

    The letter sits before the optional language suffix, so it is the last W or
    N in the stem for every name the disc uses: MENUW, GOMOVNF, SCN01WS.
    """
    m = re.match(r'^(.*?)([WN])([FGIS]?)$', stem)
    return m.group(2) if m else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('src', help='MOVIES directory from the disc')
    ap.add_argument('dst', help='where the MP4s go')
    ap.add_argument('--crf', type=int, default=18,
                    help='quality, lower is better (default 18, visually lossless)')
    ap.add_argument('--only', help='convert just the files whose name contains this')
    ap.add_argument('--force', action='store_true',
                    help='re-encode even when the output already exists')
    args = ap.parse_args()

    os.makedirs(args.dst, exist_ok=True)

    files = []
    for root, _, names in os.walk(args.src):
        for n in names:
            if n.lower().endswith('.pss'):
                files.append(os.path.join(root, n))
    files.sort()
    if args.only:
        files = [f for f in files if args.only.lower() in os.path.basename(f).lower()]

    if not files:
        print('no .PSS files found', file=sys.stderr)
        return 1

    done = skipped = failed = 0
    for path in files:
        stem = os.path.splitext(os.path.basename(path))[0]
        v = variant(stem)
        if v is None:
            print(f'  {stem}: cannot tell the aspect from the name, skipped')
            skipped += 1
            continue

        out = os.path.join(args.dst, stem + '.mp4')
        if os.path.exists(out) and not args.force:
            # Only skip if it already has the right display aspect.
            probe = subprocess.run(
                ['ffprobe', '-v', 'error', '-select_streams', 'v',
                 '-show_entries', 'stream=width,height,sample_aspect_ratio',
                 '-of', 'csv=p=0', out],
                capture_output=True, text=True)
            fields = probe.stdout.strip().split(',')
            w, h = TARGET[v]
            if len(fields) >= 2 and fields[0] == str(w) and fields[1] == str(h) \
                    and (len(fields) < 3 or fields[2] in ('1:1', 'N/A', '')):
                skipped += 1
                continue

        w, h = TARGET[v]
        cmd = [
            'ffmpeg', '-y', '-v', 'error', '-i', path,
            # scale then setsar: without the second, ffmpeg keeps a compensating
            # sample aspect and the stretch is undone.
            '-vf', f'scale={w}:{h},setsar=1',
            '-c:v', 'libx264', '-crf', str(args.crf), '-preset', 'medium',
            '-pix_fmt', 'yuv420p',
            '-c:a', 'aac', '-b:a', '192k',
            '-movflags', '+faststart',
            out,
        ]
        print(f'  {stem:<12} {v}  -> {w}x{h}', flush=True)
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print(f'     failed: {r.stderr.strip()[:200]}', file=sys.stderr)
            failed += 1
        else:
            done += 1

    print(f'\n{done} converted, {skipped} already correct, {failed} failed')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
