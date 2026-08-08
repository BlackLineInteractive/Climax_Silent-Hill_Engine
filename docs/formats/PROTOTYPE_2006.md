# Silent Hill Origins: 2006 Prototype Archival Format (GR.ARC)

The August 7, 2006 PSP prototype uses an early Climax Engine data architecture directly inherited from *Ghost Rider*. The `GR.ARC` format used here is an early iteration of what would eventually become the `A2.0` and `0x0000FA10` (Shattered Memories) formats.

## 1. The archive header

`GR.ARC` opens with a 16-byte header. There is no magic string, unlike the
final `A2.0` format.

```c
struct PrototypeArcHeader {
    u32 fileCount;        // 61
    u32 dataBase;         // 992 -- where entry offsets are measured from
    u32 nameTableOffset;  // 42058144
    u32 nameTableSize;    // 1488
};
```

`nameTableOffset + nameTableSize` equals the file size exactly (42059632),
which is what identifies those two fields: the name table is the last thing in
the archive. `dataBase` is also exactly where the table of contents ends --
16 + 61 * 16 = 992 -- so the payloads begin immediately after the TOC.

## 2. Table of contents

An array of `fileCount` 16-byte entries at offset `0x10`.

```c
struct PrototypeArcEntry {
    u32 nameOffset;       // byte offset into the name table
    u32 offset;           // relative to dataBase, NOT absolute
    u32 size;             // stored size
    u32 uncompressedSize; // 0 throughout: nothing in this build is compressed
};
```

**The entries do carry names.** Field 0 indexes a string table at the end of
the archive, exactly as the retail `A2.0` format does -- the two formats are
closer than they look. Field 1 is relative: reading at `dataBase + offset`
lands on a valid `0x071C` chunk with version `0x1C020065`, and reading at the
raw offset does not.

The fourth field follows the same convention as retail: zero means the payload
is stored as-is. Every entry in this build is zero, so **nothing is compressed
or encrypted** -- containers can be read straight out of the archive.

## 3. Contents

61 entries. The build is a vertical slice with a very different level layout
from the retail game:

| Group | Entries |
|-------|---------|
| Levels | `Asylum_1flr_z1`, `Asylum_2flr_z2`, `Asylum_2flr_z7`, `SilentHill`, plus `.txd` and `.log` companions |
| Zone pairs | `Asylum_1flr_z1-Asylum_2flr_z2`, `Asylum_2flr_z7-SilentHill`, `Hotel_1flr_z1-SilentHill`, `MP_Shopfront-SilentHill` |
| Characters | `CPlayerBehaviour.TravisGrady`, `CEnemyBehaviour.Butcher`, `CEnemyBehaviour.StraightJacket`, `CEnemyBehaviour.AffectedStage1` / `AffectedStage1Alt` |
| Loading art | 13 JPEGs: `ASYLUM_*`, `BUTCHER_*`, `CITY_*` |
| Other | `bootup.dff`, `GlobalStream`, `mainmenu.stream`, `start.stream`, `String.db`, `loading_screen.jpg` |

Notable against the retail game:

* There is a **`SilentHill` level** -- a town space, referenced by four
  different zone-pair bundles, so streaming between the town and the interiors
  already worked.
* The Asylum is split into numbered floor zones (`1flr_z1`, `2flr_z2`,
  `2flr_z7`, `2flr_z8`, `2flr_z3`); none of this survives into retail.
* Travis is `TravisGrady`, his full name, rather than retail's `Travis`.
* `AffectedStage1` is an enemy class that does not exist in the final game.
  `Butcher` and `StraightJacket` do.
* The zone-pair naming (`A-B.txd`) is the same convention retail uses for
  shared texture dictionaries, so that streaming design was settled early.
* The loading-screen filenames preserve working titles and two slips:
  `ASYLUM_ELECTRO_Fpsd.jpg` kept a Photoshop suffix, and `CITY_MIANSTREET_F.jpg`
  misspells "main".

## 4. Container format

The containers are ordinary RenderWare 3.7.0.2. `Asylum_1flr_z1` and
`SilentHill` both open with a `0x071C` chunk carrying version `0x1C020065`,
the same type directory and the same version word the retail PS2 game uses,
and `SilentHill.txd` opens with `0x0016` (texture dictionary). A scan of the
`SilentHill` payload finds 2246 chunks carrying `0x1C020065` and no other
version word.

This means the prototype's containers are readable with the same parser as
retail; only the archive header differs. **Implemented** -- `CArchive` now
recognises the format as `ArcFormat::GR_PROTO`, detected by arithmetic rather
than a magic word (the name table must end at EOF *and* the payloads must start
exactly where the table of contents ends). Mounting `GR.ARC` lists all 61
entries, and the loader pairs each level with its texture dictionaries
automatically.

**But the platform data inside is not PS2.** Loading `SilentHill` registers 12
objects and finds 524 material names, yet produces **zero meshes and zero
textures**: the section walk, the object graph and the material lists all
decode, and then the native geometry and raster blocks do not, because they are
not the formats the toolkit knows.

The texture dictionary device id is **9**, against **6** in the retail PS2
build, so the rasters are in the PSP's own form. Both halves were then looked
at properly, and they turn out to be very different problems.

### Geometry: not a PSP problem at all

There is **no `0x0510` NativeDataPLG anywhere** in these containers, only
BinMesh. The geometry flags have the native bit clear:

    flags=0x0001003F   tristrip, positions, UVs, vertex colours, normals

That is ordinary RenderWare geometry -- plain arrays in the Struct, no display
lists of any kind. The decoder required both a BinMesh *and* a native block and
returned early otherwise, which is why levels that parsed every section,
material, clump and skeleton still produced zero meshes.

A plain-geometry path is now implemented, and it is worth recording the one
trap: the triangle record is

    { u16 vertex2; u16 vertex1; u16 materialId; u16 vertex3; }

The material id sits *between* the vertices and vertex 3 comes last. Read as
three consecutive indices it yields a mesh that looks nearly right and is wrong
everywhere. `SilentHill` and `Asylum_1flr_z1` now build meshes (10 and 4
respectively); the retail PS2 path is unaffected.

### Textures: a real PSP format, decoded and verified

The raster header is 172 bytes and starts with standard RenderWare format bits,
then width and height as two `u16`:

    0x4500 = PAL4|8888   32x32   ->  512 indices + 64 palette + 172 = 748
    0x0300 = 4444       128x128  ->  32768 + 172 = 32940
    0x0200 = 565         64x64   ->   8192 + 172 = 8364

Three different formats and sizes all agreeing on a 172-byte header is what
confirms it. The pixels are **swizzled in 16-byte by 8-row blocks**, and the
component order is PSP's own (`R` in the low nibble, `A` in the high). Decoding
a 4444 raster with both corrections produces a clean, coherent surface; without
the unswizzle it comes out as horizontal stripes.

This is documented but **not implemented** in the toolkit -- prototype levels
currently render untextured.

## 5. Game Executable (`EBOOT.BIN`)
The game engine logic is contained entirely within `/PSP_GAME/SYSDIR/EBOOT.BIN`. As is standard for PSP games, this file is an encrypted PRX/ELF, but remarkably, the prototype's `EBOOT.BIN` is a **raw, unencrypted ELF** (`\x7FELF`).

Due to the early stage of development, this executable still carries the core `Ghost Rider` logic and property parsing maps (which `attrmap.py` decodes), as well as the `0x071C` Climax container dispatcher. The binary is stripped of its symbol table, meaning debugging symbols like `HandleAttributes` are absent.

**Lore / Identity Crisis:**
A string analysis of the `.rodata` and `.data` sections reveals a hilarious mix of assets, proving that Climax LA simply dropped the Silent Hill protagonist into a fully functioning Ghost Rider build:
* **Silent Hill References:** `TravisGrady`, `CZone`, `Asylum_1flr_z1`
* **Ghost Rider Leftovers:** `CHellBike.MPGhostRider`, `CBikeBossBehaviour`, `CDemonEssenceManager`, `CLilithBehaviour`, `character_classic_ghost_rider_unlock_seq`

## 6. Audio Files (`DATA/MUSIC/*.RWS`)
Unlike the final game which uses custom Climax Audio Banks, the prototype relies directly on **RenderWare Audio Streams (`.RWS`)**.
- Example files: `MUSTITLE.RWS`, `MUSCOMBA.RWS`, `MUSFREEZ.RWS`.
- This confirms that audio middleware was entirely RenderWare-based at this stage before Climax transitioned to their bespoke sound engine for the final SHO release.

## 7. PlayStation Portable Modules (`.prx`)
The `USRDIR/module` and `USRDIR/kmodule` folders contain standard Sony SCE utility modules. These are not unique to Silent Hill but were used by the Ghost Rider engine to handle basic PSP hardware interfacing.
- **Audio/Video:** `audiocodec.prx`, `videocodec.prx`, `mpeg.prx`, `libpsmfplayer.prx` (suggests video playback was implemented).
- **Crypto/Hash:** `libmd5.prx`, `libsha1.prx`, `libbase64.prx`
- **Other:** `libfont.prx` (PSP system font library), `libatrac3plus.prx` (Sony's proprietary audio codec, likely decoding the `.RWS` audio streams).
