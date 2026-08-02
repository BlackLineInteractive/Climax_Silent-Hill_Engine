# Silent Hill: Shattered Memories (Wii) — Archive and Container Reference

Companion to [SH_FORMAT.md](SH_FORMAT.md), which covers Silent Hill Origins on
PlayStation 2. This document describes the second Climax Engine title, *Silent
Hill: Shattered Memories*, as shipped on Nintendo Wii: the `data.arc` and
`igc.arc` archives, the level containers inside them, and the GameCube/Wii
texture format their dictionaries use.

Everything below was derived by parsing the retail Wii disc image. Where a claim
rests on a measurement, the figure is given. Where something is unresolved, it is
marked as such rather than guessed; §10 lists what remains open.

---

## 1. Notation and method

All offsets are hexadecimal, all sizes decimal unless prefixed `0x`.

**Endianness is not uniform in this game and this is the single most important
thing to get right.** Three different conventions appear in one file:

| Data | Byte order |
|------|-----------|
| Archive header and entry table | little-endian |
| RenderWare chunk headers (`[type][size][version]`) | little-endian |
| Climax section headers, game-object records, floats, native raster fields | **big-endian** |

The archive is little-endian because it is shared with the PS2 and PSP builds.
The payload data is big-endian because the Wii's PowerPC core reads it directly.

The test corpus is the retail disc:

```
files/data.arc   1 500 508 160 bytes   1995 entries
files/igc.arc    1 376 569 344 bytes    289 entries
sys/main.dol       4 565 440 bytes
```

---

## 2. The archive

### 2.1 Header — 16 bytes at offset 0

```
0x00  u32  magic            0x0000FA10  (bytes 10 FA 00 00)
0x04  u32  entryCount       data.arc 1995, igc.arc 289
0x08  u32  firstDataOffset  data.arc 0x8000, igc.arc 0x1800
0x0C  u32  reserved         0 in both files
```

This is a different container from the `"A2.0"` archive of Silent Hill Origins.
It is four bytes shorter, and — the decisive difference — **it has no name
table**. Origins stores every file name in a block at the tail of the archive;
Shattered Memories stores a 32-bit key instead and nothing else (§2.4).

### 2.2 Entry table — `entryCount × 16` bytes, from 0x10

```
0x00  u32  key               32-bit resource identifier, see §2.4
0x04  u32  offset            absolute, from the start of the archive
0x08  u32  compressedSize    bytes actually stored
0x0C  u32  uncompressedSize  0 means the payload is stored, not deflated
```

Verified across both archives:

| Property | data.arc | igc.arc |
|----------|----------|---------|
| Offsets monotonically increasing | yes | yes |
| Overlapping payloads | 0 | 0 |
| Every offset a multiple of 2048 | yes | yes |
| Last payload ends at | file size − 446 | exactly the file size |
| Keys unique | 1995 / 1995 | 289 / 289 |

The alignment is not fixed at 2048: entries are aligned to whatever suits their
size, and the distribution in `data.arc` is 992 at 2 KB, 499 at 4 KB, 252 at
8 KB, 137 at 16 KB and 115 at 32 KB. Treating 2048 as the floor is safe; anything
stricter is not.

The table ends before `firstDataOffset` with a gap (832 bytes in `data.arc`,
1504 in `igc.arc`) that is padding, not data.

### 2.3 Payloads

A payload is a raw zlib stream (`78 DA`) when `uncompressedSize` is non-zero, and
stored verbatim when it is zero. This is the same rule Origins uses.

All 1995 entries of `data.arc` inflate without a single failure, to 2.13 GB from
1.40 GB stored — an overall ratio of 0.66. Eleven of them are stored rather than
deflated, and all eleven are JPEG images, which zlib cannot usefully shrink.

In `igc.arc` **every one of the 289 entries is stored uncompressed.** Its content
is already-compressed audio and pre-packed sub-archives.

### 2.4 The 32-bit key — what it is, and what it is not

The first word of each entry is a unique 32-bit identifier. It is the archive's
only lookup key: there are no names anywhere in the file.

It is **not** a checksum of the payload. Tested on 40 entries against CRC-32 and
Adler-32 of both the stored and the inflated bytes, and against the zlib stream's
own trailing Adler-32: zero matches.

It is **not** a plain hash of the resource name, for any of the following. Two
independent name oracles were used:

* **24 audio banks** carry their own file name in the first 64 bytes of their
  payload (`AFX_BootMusic.xm`, `SH1R_track_07_CarJourney.xm`, …), giving exact
  name/key pairs.
* **2736 resource names** were lifted from the length-prefixed string tables in
  `igc.arc` (`IGC_10_5_DrK.anm`, `AFX_02_1a.snd`, …).

Both sets were tested against FNV-1, FNV-1a, djb2, djb2-xor, sdbm, Jenkins
one-at-a-time, the ELF hash, RenderWare's `rwStringHash`, CRC-32 and Adler-32, in
each case over the name as-is, lower-cased, upper-cased, with and without the
extension, with and without a trailing NUL, and with separators normalised. Zero
matches in every combination.

It is **not** a fold of the resource GUID. The build paths inside the containers
name their outputs `{GUID}.dff` and `{GUID}.txd`, and every 16-byte GUID in the
first 4 KB of 200 containers was tested as four little- and big-endian words —
XOR-folded, summed, first word, last word, CRC-32 and Adler-32. Zero matches.

It is **not a multiplicative rolling hash of the bare name.** `AFX_ITCombat.xm`
and `AFX_DTCombat.xm` are the same length and differ in exactly one character, at
index 4. For any hash of the form `h = h·M + c`, that forces
`h(a) − h(b) = 5·M¹⁰ (mod 2³²)`, which determines `M¹⁰`. Lifting the tenth roots
bit by bit from mod 2 up to mod 2³² yields **no** multiplier at all, for either
candidate key.

A further hard data point: **two entries carry the identical internal name
`AFX_ITCombat.xm` with different keys** (`0x173610EE` and `0x1129EB8A`). Whatever
the key is derived from, it is not that string alone — either it includes a path
the payload does not record, or it is an identifier assigned at build time with
no textual preimage at all.

`main.dol` was searched for the usual give-aways: the FNV prime `0x01000193`,
the FNV basis `0x811C9DC5`, the Murmur constants, and both CRC-32 polynomials.
Only the reversed CRC-32 polynomial and its table are present, and those belong
to the Nintendo RVL SDK (the same binary contains `arc.c`, `ARCInitHandle: bad
archive format` and `HomeMenu%s.arc`, which are the SDK's own unrelated U8
archive code). Nothing FNV- or Murmur-shaped appears anywhere.

And there is **no name table in the executable either**. `main.dol` does carry
archive data, but only more of the same: at 0x419378 it embeds a verbatim copy of
the `data.arc` header and all 1995 entry records, and immediately after it the
same for `igc.arc`, each introduced by the literal `EMBEDED` and the archive's
file name. This is a cached directory so the game need not read the table from
the disc. All 1995 keys are therefore "present in the executable", but only as
that copy — a random control of 1995 values matches 0 times, and no key appears
as a `lis`/`ori` immediate pair anywhere in the code (0 of 1995), so the game
does not name resources by compiled-in constants either.

A third archive is embedded at 0x3F9378: a complete 6-entry archive of boot
resources — three binary tables, two texture containers and one XML config. Five
of its six keys are byte-identical to keys in `data.arc`, which establishes that
**the key is a global resource identifier, stable across archives**, not a
per-archive index or a hash of a per-archive path.

That last archive also yields the cleanest oracle available: its sixth entry
decompresses to `<CONFIG ProductID="RVL-R5WE" …>`, so the resource the executable
calls `$config.txt` has key `0x1586A83D`. Tested against FNV-1, FNV-1a, CRC-32 and
its complement, Adler-32, a plain byte sum, and every `h = h·M + c` for
M ∈ {31, 33, 37, 131, 1313, 65599, 16777619, 2654435761, 0x9E3779B1, 5381, 101,
257, 0x1003F} × six initial values, over eight spellings of the name in four
cases: no match.

**Practical consequence.** Extraction works fine by index, and a key identifies a
resource unambiguously and permanently. Names do not survive anywhere on the
disc; the recoverable naming is per-content, described in §3 and §8.

### 2.5 Nested archives

An entry may itself be a complete archive in this same format. In `igc.arc` the
first ten entries are exactly that. Their sub-tables are self-contained: every
sub-entry offset is relative to the start of the sub-archive, `firstDataOffset`
equals `16 + subEntryCount × 16` in each case, and no sub-payload runs past the
parent entry. Sub-payloads follow the normal rule and *are* zlib-compressed even
though the parent entry is stored.

```
entry 0    3 sub-entries    864 bytes   CSV tables
entry 1-6  5 sub-entries    ~391 KB     speech and text per language
entry 7    2 sub-entries    9 568 bytes
entry 8    2 sub-entries   11 520 bytes  psychology-profile CSVs
entry 9   98 sub-entries   661 440 bytes
```

Nesting is one level deep in the retail data, but a reader should recurse: the
test is simply whether the payload begins with `10 FA 00 00`.

---

## 3. Content census

### 3.1 `data.arc` — 1995 entries, 2.13 GB inflated

| Count | Content |
|-------|---------|
| 1857 | level / model containers, beginning with a `0x0716` chunk (§4) |
| 84 | JPEG images (73 deflated, 11 stored) |
| 24 | audio banks — a 64-byte name field, then a FastTracker II module |
| 18 | binary tables beginning with `u32 = 2` |
| 6 | CSV configuration tables |
| 1 | XML (`<BOOTMENU>` …) |
| 5 | other single-purpose blobs |

The audio banks are the one class that names itself:

```
0x00  char name[64]     "AFX_BootMusic.xm", NUL-padded
0x40  u32  size
0x44  u32  count
0x48  ...               "Extended Module: …"  (standard XM from here)
```

The CSV tables are readable configuration and are worth reading directly — the
mixer table, the reverb preset table (`ExploreInteriorSmall`, `CombatLarge`, …
with full EAX parameters for the PS2 and PSP presets side by side), the streaming
budget table, and the psychology-profiling tables that drive the game's central
mechanic (`Criteria Id,OPEN,CONSC,EXTRA,AGREE,…`).

### 3.2 `igc.arc` — 289 entries, 1.28 GB, all stored

| Count | Content |
|-------|---------|
| 10 | nested archives (§2.5) |
| 279 | cutscene audio streams with an embedded name and resource table |

The 279 streams are the bulk of the disc. Each begins with a small header whose
second word is the payload length, followed by a name and a table of
length-prefixed resource names:

```
0x00  u32   header/tag word
0x04  u32   payload size
0x08  u32   1
0x0C  u32   1
0x10  u32   0
0x14  char  name[]        "AFX_Balkan_Michelle", "AFX_EndCredits_Wii_PSP_PS2", …
...         [u16 len][char name[len]] records:
            "IGC_10_5_DrK.anm", "IGC_10_5_DrK.face.dma", "IGCState", …
```

The header word varies (`0x0F01`, `0x0360`, `0x036A`, `0x0118`, `0x0109`, …) and
for most entries it equals `entrySize − declaredSize`, i.e. it is the length of
the header block. The 19 entries tagged `0x0F01` break that pattern with a
constant 20-byte gap instead. This header is **not fully resolved** — see §10.

---

## 4. The level container

### 4.1 Relationship to Silent Hill Origins

The container is recognisably the same format, with two differences that break a
naive reuse of the Origins parser.

**There is no `0x071C` type directory.** In Origins every container opens with a
directory listing each game-object class and its instance count. Shattered
Memories has none: 0 of 120 containers examined contain a `0x071C` chunk
anywhere. The file starts directly with the first `0x0716` section.

**The section and object payloads are big-endian.** The chunk headers are not.

**The version word does not identify the flavour.** Of the 1857 containers in
`data.arc`, 1698 stamp their first chunk with the same RW 3.7.0.2 build 0x0065
that Origins uses and only 159 carry Climax's `0x1802FFFF`. The reliable test is
the byte order itself: read the section header big-endian and check that the tag
comes out as an `rw*` string. That succeeds on 1857 of 1857 Shattered Memories
containers and fails on an Origins one, whose little-endian lengths read as
implausible numbers when taken the other way round.

Everything else carries over. Over 250 containers the top level holds 2171
`0x0716` sections and 1902 `0x0704` game objects, and nothing else. A container
carries between 2 and 417 sections, 17.3 on average.

### 4.2 Section header

```
[u32 LE type = 0x0716][u32 LE size][u32 LE version]
  +0x00  u32 BE  headerSize
  +0x04  u32 BE  tagLen
  +0x08  char    tag[tagLen]        usually 4 bytes of 0xBF padding
  ...    byte    guid[16]
  ...    u32 BE  nameLen
  ...    char    name[nameLen]      "rwID_TEXDICTIONARY", NUL then 0xBF padded
  ...    build-path strings
```

As in Origins, the payload is reachable without walking the strings:

```
dataStart = inner + 4 + headerSize      -> u32 BE payload length
chunk     = dataStart + 4               -> the RenderWare chunk (LE header)
```

Worked example, the first entry of `data.arc`: `headerSize = 188`, `tagLen = 4`,
name `rwID_TEXDICTIONARY`, `dataStart = 204`, payload length 1 262 840 big-endian
(the same bytes read little-endian give 4 165 210 880, which is the quickest way
to catch an endianness mistake).

### 4.3 Section tags

Counted over all 1857 containers in `data.arc`, which parse to 21 761 sections
and 19 999 game objects without a single walk failure:

| Count | Tag | Also in Origins |
|-------|-----|-----------------|
| 4827 | `rwID_TEXDICTIONARY` | yes |
| 4146 | `rwID_HANIMANIMATION` | as a type, not as a section |
| 1587 | `rwID_CLUMP` | yes |
| 1184 | `rwID_RWS` | yes |
| 646 | `rwID_WORLD` | yes |
| 337 | `rwID_DMORPHANMSTREAM` | new |
| 323 | `rwID_CBSP` | yes |
| 310 | `rwID_AUDIODATA` | replaces `rwaID_WAVEDICT` |
| 156 | `rwID_AINAVMESH` | yes |
| 147 | `rwID_SPLINE` | new |
| 64 | `rwID_DMORPHANIMATION` | new |
| 20 | `rwID_FUSESTATE` | new |
| 9 | `rwID_ZONEINFO` | new |
| 9 | `rwpID_BODYDEF` | new |
| 2 | `rwID_KFONT` | new |
| 1 | `rwID_MTEFFECTDICT` | new |

`rwID_HANIMANIMATION` being the most common section by a wide margin is the
headline: Shattered Memories ships its skeletal animation as first-class sections
rather than burying it, which makes it the better corpus for working out the
animation format described in [ANIMATION_SPEC.md](ANIMATION_SPEC.md).

### 4.4 Game objects (`0x0704`)

Identical in structure to Origins and **big-endian**:

```
[u32 BE size][u32 BE id][payload]
```

with the top byte of `id` selecting the record kind — `0x20` class name, `0x40`
GUID, `0x80` instance name, `0x00` indexed property — and names padded with
`0xBF`. Floats are big-endian too.

A real record, the opening bytes of a `cReflector`:

```
00 00 00 00  00 00 00 14  20 00 00 00  "cReflector" 00 BF
             size 20      id 0x20000000 = class name
00 00 00 18  40 00 00 00  DA 94 07 66 4C CE 88 AD BB 75 7F AC B2 CC 7B 98
             size 24      id 0x40000000 = GUID
00 00 00 14  80 00 00 00  "cReflector" 00 BF
             size 20      id 0x80000000 = instance name
00 00 00 0C  00 00 00 00  00 00 00 01        property 0 = 1
00 00 00 0C  00 00 00 01  3F 4C CC CD        property 1 = 0.8f
```

### 4.5 RenderWare versions

The version word of every chunk, decoded with the standard library-ID unpack:

| Raw | Meaning | Used by |
|-----|---------|---------|
| `0x1802FFFF` | RW 3.6.0.2, build 0xFFFF | Climax's own `0x0716` / `0x0704` chunks |
| `0x1C020065` | RW 3.7.0.2, build 0x0065 | RenderWare data — same build as Origins |
| `0x1C020055` | RW 3.7.0.2, build 0x0055 | some older assets |
| `0x00030002` | RW 3.0.0.3 | a few legacy chunks |

The RenderWare build being byte-identical to the Origins one is the concrete fact
behind "the containers are compatible"; the *packaging* around them is not.

---

## 5. Wii texture dictionaries

A `rwID_TEXDICTIONARY` section's payload is a normal RenderWare texture
dictionary — chunk `0x0016`, a `Struct` giving the texture count, then one
`0x0015 TextureNative` per texture.

```
0x0016  TextureDictionary
  0x0001  Struct, 4 bytes:  u16 LE numTextures, u16 LE deviceId (3)
  0x0015  TextureNative, one per texture
    0x0001  Struct
```

The dictionary's own `Struct` is **little-endian**, unlike the raster struct one
level below it. This is not an assumption: read little-endian the first
dictionary of `data.arc` declares 5 textures and contains exactly 5
`TextureNative` chunks, while big-endian would read 1280.

### 5.1 The native raster struct

All fields big-endian. The header is 108 bytes, then the pixel data:

```
+0x00  u32   platform          6 = GameCube / Wii
+0x04  u32   filterAddressing  e.g. 0x00001102
+0x08  u32   0
+0x0C  u32   1
+0x10  u32   1
+0x14  u32   0
+0x18  char  name[32]          "CH_AdultCheryl_Jacket"
+0x38  char  mask[32]
+0x58  u32   rasterFormat      0x8004 CMPR, 0x8304 RGB5A3, 0x8504 RGBA8
+0x5C  u16   width
+0x5E  u16   height
+0x60  u8    bitsPerPixel      4 / 16 / 32, matching the GX format
+0x61  u8    mipCount
+0x62  u8    gxFormat          the GX texture format, see below
+0x63  u8    0xFF
+0x64  u32   hasAlpha          0 or 1
+0x68  u32   dataSize          total bytes of the whole mip chain
+0x6C  ...   pixel data
```

### 5.2 GX formats and the mip chain

`gxFormat` is the Hollywood GPU's own texture-format enum:

| Value | Format | Bits/texel | Tile |
|-------|--------|-----------|------|
| 0 | I4 | 4 | 8×8 |
| 1 | I8 | 8 | 8×4 |
| 2 | IA4 | 8 | 8×4 |
| 3 | IA8 | 16 | 4×4 |
| 4 | RGB565 | 16 | 4×4 |
| 5 | RGB5A3 | 16 | 4×4 |
| 6 | RGBA8 | 32 | 4×4 |
| 8 | C4 | 4 | 8×8 |
| 9 | C8 | 8 | 8×4 |
| 14 | CMPR | 4 | 8×8 |

Each level is stored at its tile-rounded size, so the chain is

```
levelBytes(fmt, w, h) = ceilToTile(w) * ceilToTile(h) * bitsPerTexel / 8
dataSize = sum over mipCount levels, halving w and h each time, minimum 1
```

**Validation.** Over 150 containers holding 1217 textures, the `dataSize` field
equals the value this formula produces for 1197 of them — every CMPR, RGBA8,
RGB5A3 and I4 texture without exception. The 20 that differ are all `C8`, which
carries a separate palette the size field does not account for; that case is
unresolved (§10).

The decoder in this repository reads all 4827 dictionaries of `data.arc` and
produces 20 030 images with no failures, and its output has been checked by eye:
a CMPR character atlas, an RGB5A3 hair sheet with soft alpha and an RGBA8
eyelash sheet all come out correct in colour, orientation and transparency,
which is what rules out a wrong tile order or a swapped channel.

Worked example: `CH_AdultCheryl_Jacket`, CMPR, 512×512, 4 mip levels.
`131072 + 32768 + 8192 + 2048 = 174080`, which is exactly the `dataSize` field,
and `108 + 174080 = 174188`, exactly the struct size.

### 5.3 Tile layouts

Each format stores whole tiles in reading order:

* **CMPR** — an 8x8 tile is four 8-byte DXT1 sub-blocks covering (0,0), (4,0),
  (0,4) and (4,4). Endpoint colours are big-endian, and the 2-bit indices run
  from the *most* significant pair of each byte, the reverse of DXT1 elsewhere.
  When `c0 <= c1` the fourth index is fully transparent.
* **RGBA8** — a 4x4 tile is 64 bytes: 32 bytes of interleaved alpha/red first,
  then 32 of green/blue.
* **RGB5A3** — the top bit picks the encoding: set means 5 bits per channel and
  full opacity, clear means 3 bits of alpha and 4 bits per channel.
* **RGB565, IA8** — 4x4 tiles, 16 bits per texel, big-endian.
* **I8, IA4** — 8x4 tiles, one byte per texel.
* **I4** — 8x8 tiles, high nibble first.

### 5.4 What the corpus looks like

Format distribution over those 1217 textures: CMPR 1036, RGBA8 83, RGB5A3 70,
C8 20, I4 8. Mip counts are 4 for 1188 of them and 1 for the remaining 29.
Common sizes are 512×512 (350), 256×256 (289), 128×128 (130) and 256×512 (96).

The pixel data is tiled in the GameCube's block order and has to be untwiddled
before use — the same class of problem as the PS2 swizzle in
[SH_FORMAT.md](SH_FORMAT.md) §6, but a different layout.

---

## 6. Origins and Shattered Memories side by side

| | Origins (`SH.ARC`, PS2) | Shattered Memories (`data.arc`, Wii) |
|---|---|---|
| Archive magic | `"A2.0"` | `0x0000FA10` |
| Archive header | 20 bytes | 16 bytes |
| File names | full name table at the tail | **none** — a 32-bit key only |
| Entry record | name offset, offset, sizes | key, offset, sizes |
| Compression | zlib, stored when `uncompressedSize == 0` | identical rule |
| Nested archives | no | yes, 10 in `igc.arc` |
| Type directory `0x071C` | present in every container | absent |
| Sections `0x0716` | yes, little-endian fields | yes, **big-endian** fields |
| Game objects `0x0704` | yes, little-endian | yes, **big-endian** |
| RenderWare build | 3.7.0.2 build 0x0065 | the same, plus 0x0055 |
| Level sound bank | `rwaID_WAVEDICT` | `rwID_AUDIODATA` |
| Animation sections | none as sections | `rwID_HANIMANIMATION`, most common tag |
| Textures | PS2 PSMT4 / PSMT8 / PSMCT32 | GX CMPR / RGBA8 / RGB5A3 / C8 / I4 |
| Music | `MUSIC/*.RWS`, VAG ADPCM | XM modules inside the archive |
| Cutscene audio | `IGC.ARC/*.IGCStream`, ADS | 279 named streams in `igc.arc` |

---

## 7. Reading an entry

```c
struct ShsmArcHeader { uint32_t magic, entryCount, firstDataOffset, reserved; };
struct ShsmArcEntry  { uint32_t key, offset, compressedSize, uncompressedSize; };

// magic == 0x0000FA10; header and table are little-endian on every platform.
// A payload is zlib when uncompressedSize != 0, and stored verbatim when it
// is 0. A stored payload beginning with 10 FA 00 00 is itself an archive:
// recurse, with sub-entry offsets relative to the sub-archive.
```

Everything past that point is big-endian except the RenderWare chunk headers.

---

## 8. Recovering names

The archive gives none. Four sources inside the payloads do:

1. **Audio banks** state their own file name in the first 64 bytes (§3.1).
2. **Cutscene streams** in `igc.arc` carry a name plus a table of the resources
   they use — 2736 distinct names, in the game's own naming (`IGC_10_5_DrK.anm`,
   `AFX_02_1a.snd`).
3. **Containers** embed the artist's build paths, which name the asset and its
   place in the project tree:
   ```
   z:\SH1R\Content\RVL\Characters\Adult_Cheryl\Textures
   z:\SH1R\Design\IGC\Adult_Cheryl\Adult_Cheryl\Build Output\RVL\Target Resources\{…}.dff
   ```
   `SH1R` is the project's internal name and `RVL` is Nintendo's code for the
   Wii, so the platform an asset was built for is readable from its path.
4. **Texture dictionaries** name every texture in the raster header (§5.1),
   e.g. `CH_AdultCheryl_Jacket`.

A practical extractor should therefore name output files from their content and
fall back to the index, which is what makes the missing key derivation tolerable
in practice.

---

## 9. Sources

Figures come from parsing the retail Wii release. The RenderWare version unpack
follows the standard library-ID formula; the GX texture formats and their tile
sizes follow the Nintendo GX specification.

---

## 10. Open questions

* **The 32-bit key.** §2.4 records what it is not, across a wide and explicit
  search, including a known name/key pair. The evidence now points at an
  identifier minted by the asset pipeline rather than derived from a name, in
  which case no preimage exists on the disc at all. The remaining leads are to
  disassemble the PowerPC resource-request path in `main.dol` and see what a
  caller actually passes, or to find a build-time manifest outside the disc.
* **`C8` texture data size.** The 20 paletted textures do not match the mip-chain
  formula; their palette is presumably counted differently, or stored separately.
* **The `igc.arc` stream header.** The leading word equals the header length for
  most entries but not for the 19 tagged `0x0F01`. The rest of the block — the
  resource-name table and what follows it — is only partly mapped.
* **The GX tile order** is stated here from the GX specification, not verified
  against a decoded image, because the viewer does not yet decode Wii textures.
* **The new section tags** — `rwID_SPLINE`, `rwID_FUSESTATE`, `rwpID_BODYDEF`,
  `rwID_ZONEINFO`, `rwID_DMORPHANIMATION` — are catalogued but not parsed.
* **`rwID_AUDIODATA`** is assumed to be the counterpart of the Origins
  `rwaID_WAVEDICT` sound bank on the strength of its name and its 39 occurrences.
  Its payload has not been decoded.

