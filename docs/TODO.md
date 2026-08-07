# Work queue

Ordered by value, with what is already known about each so the next attempt
does not start from nothing. Format references: [SH_FORMAT.md](SH_FORMAT.md)
for Origins, [SHSM_ARC_FORMAT.md](SHSM_ARC_FORMAT.md) for Shattered Memories.

---

## Shattered Memories (Wii)

### 1. Paletted textures render black

32 of the 4053 textures in `data.arc` are `C8`, and they are skipped rather than
decoded, so their surfaces bind texture 0 and come out black. Affects a handful
of effect sheets and the ice-transition wipes (`FX_EN_Glow01`,
`EN_TL_ICETRANSITION_3`).

What is established:

* the sizes fit a 16-bit TLUT of 16 (C4) or 256 (C8) entries between the header
  and the pixels — `FX_EN_Glow01` is 108 + 512 + 5440 = 6060 and
  `EN_TL_ICETRANSITION_3` is 108 + 512 + 65536 = 66156, both exactly their
  struct size;
* of the six offset/format combinations tried, a palette at +0x68 read as
  RGB565 is much the smoothest (mean neighbour difference 33.9 against 95–158);
* **it is still wrong** — `EN_TL_ICETRANSITION_3` decodes to rainbow noise, so
  either the palette is elsewhere or it is not a plain 16-bit TLUT.

`DecodePaletted` in `src/Rendering/WiiTexture.cpp` is written and ready; only
the palette location and format are missing. Next lead: find the same texture
in the PS2 or PSP build, where the palette layout is already understood, and
compare.

### 2. Skinning

Characters render in their rest pose. The `Skin PLG (0x0116)` is present on
every character geometry and is not read at all. The Origins path already has a
bone-matrix uniform and a skinning branch in the shader, so this is mostly a
matter of decoding the plugin and filling `Vertex::boneIds` / `boneWeights`.

### 3. Animation

`rwID_HANIMANIMATION` is the most common section in the game — 4146 of them
across `data.arc`, against 1587 clumps. Nothing reads them yet. This is the
better corpus for the animation work than the PS2 game, and it shares the
format design; see [ANIMATION_SPEC.md](ANIMATION_SPEC.md).

Also unread: `rwID_DMORPHANIMATION` and `rwID_DMORPHANMSTREAM` (facial
animation, 401 sections), and the `0x0122` delta-morph targets on the head
geometry.

### 4. Sound

`rwID_AUDIODATA` — 310 sections, presumed to be the counterpart of the Origins
`rwaID_WAVEDICT` sound bank purely from its name and its position in the
container. Payload not decoded. The 279 named streams in `igc.arc` are also not
decodable by the current parsers, which expect the PS2 ADS and RWS layouts.

### 5. Ice and water shading

Currently a deliberate approximation: a Fresnel rim and a specular highlight,
switched by a hand-maintained name rule, with a tooltip saying so.

The real look comes from the GX TEV stages, and those are **not in the
container**. (The comparison this once drew to the PS2 blend mode no longer
holds: that one *is* in the asset, in the `0x0A01` material extension.) What the
asset does carry is the `0x0129` material extension: a single
alternate texture, which in 20 of 1747 world materials is the same surface in
its frozen state (`EN_SC_BK_ceiling` → `EN_SC_BK_ceiling_Frozen`). That is
implemented as the **Frozen variant** toggle.

To do this properly: capture a Dolphin FIFO log in an ice room and read back the
actual TEV stage setup and which textures it binds.

### 6. Game objects

The `0x0704` records are counted but not parsed — 19 999 of them across the
archive. They are the same format as Origins but big-endian, so the property
semantics (which index is the placement matrix) need re-checking for the Wii
build. Without them there are no object markers, no level cameras, and clump
sections have to be drawn at their own origin.

### 7. Unparsed section types

`rwID_SPLINE`, `rwID_FUSESTATE`, `rwpID_BODYDEF`, `rwID_ZONEINFO`,
`rwID_KFONT`, `rwID_MTEFFECTDICT` — catalogued, contents unknown.

### 8. The archive key

`docs/SHSM_ARC_FORMAT.md` §2.4 records an extensive search that ruled out every
common hash of every recoverable name, plus a known name/key pair
(`$config.txt` → `0x1586A83D`) that also fails. Names are recovered from payload
contents instead, which works well enough that this is low priority. The one
remaining lead is disassembling the resource-request path in `main.dol`.

---

## Origins (PS2)

### 1. Characters have no face — cause found, fix landed

The face texture decoded correctly and the mesh was there, but the face never
appeared. **We were discarding it.**

The material's `0x0A01` extension is three words, and the middle one is the
blend mode. Every character head and body declares `0x00010003`, and the engine
masks it to 16 bits exactly as we do — `ClimaxT1PipeSetupWorldMaterialPipes`
does `andi $s3, $v0, 0xffff` on the value the moment it reads it. So the mode is
**3**, and Ghost Rider says what 3 is:

* the alpha-op dispatch table in `.data` at `0x003AA298` is
  `NUL, STD, ADD, SUB, NONE, FIXA, FIXB`;
* `hasBlendOpsCB` indexes it with `blendMode + 1`, which lines the file's 0/1/2
  up with STD/ADD/SUB and makes **3 = NONE**;
* `ClimaxT1AtomicSetAlphaOpNONE` sets `SRCBLEND = rwBLENDONE` and
  `DESTBLEND = rwBLENDZERO`.

NONE means the surface is written straight out and **its alpha is not a coverage
channel at all**. Character textures rely on that completely:

    nurse_head      256x256   1% opaque texels,  33% below our discard threshold
    Ariel_head_cm   128x128  12% opaque texels,  23% below our discard threshold

So `if (tex.a < 0.02) discard;` threw away a third of every face. The fix routes
mode 3 into the opaque pass, sets `GL_ONE, GL_ZERO`, skips the alpha test and
forces the output alpha to 1.

**Still unexplained:** the high word. It is `0x0001` on characters and weapons,
and `Travis_Transparency` carries `0x00010000` — the same flag with mode 0 — so
it is an independent bit, not part of the mode. What it selects is unknown.

What was ruled out earlier and stays ruled out: the retracted UV-tile-offset
explanation (addressing is WRAP, so an offset of a whole unit changes nothing),
and MatFX (character materials carry no `0x0120`). Material `0x011F` UserData is
still unused and still names a second texture (`n1` = `Env02`, an environment
map) on some materials, so dual-layer characters remain undrawn.

### 2. Animation — clips now decode

See [ANIMATION_SPEC.md](ANIMATION_SPEC.md). Two of the four pieces are done.

**Clips are read.** `CPlayerBehaviour.Travis` yields **147 clips**, and there
are **3029** across the archive. Two traps had to be cleared first:

* Clips are `0x1B` chunks sitting **directly in the stream**, not inside a
  `0x716` shell, so the section walk never reached them and the existing decoder
  never ran. They are also **not word aligned** — the same pair of traps the UV
  animations sprang.
* The layout is confirmed by arithmetic over all 3029: every clip satisfies
  `chunkSize == 20 + 24 + records*20` — the `RtAnimAnimation` header, six floats
  of translation offset and scale, then one 20-byte record per keyframe.

**Track roots are marked by a saved pointer, not by a zero.** Each record holds
a byte offset back to the previous keyframe of its track, but the *first* record
of each track keeps the runtime pointer the file was written with. Those read as
large negatives stepping by exactly the 20-byte stride. Testing `prevOffset == 0`
for a root — which the old code did — finds nothing at all. The rule that works
is "anything that does not resolve to an earlier record in this clip".

**The counts cross-check.** Travis's clips carry 34–53 tracks; his skeleton has
54 frames. The one-frame difference is the clump root, which HAnim does not
animate. The 53 also matches the number of pointer-valued records counted
independently in the raw data.

One decoding bug fixed on the way: `glm::quat` takes the scalar **first**, and
the old code passed `(x, y, z, w)`, which put `-qx` into `w` and `qw` into `z` —
a different rotation entirely.

**Still to do:** map tracks to bones through the HAnim PLG (`0x011E`) bone
table, read `Skin PLG (0x0116)` for the inverse bind matrices and per-vertex
weights, then evaluate and skin. The player UI already has the transport and a
timeline; it drives nothing until those two land.

### 3. Fire — done, with two loose ends

Flames showed as flat slabs with hard polygon borders and did not animate. Both
are fixed; what remains is listed at the end of this entry.

**The hard border.** The shader dropped the vertex colour entirely on additive
materials, to stop baked room lighting from driving effect sheets to black. That
also threw away the vertex *alpha*, which on the flame meshes runs the full
0..1 across the mesh and is the artist's fade — the thing that keeps a flame
from ending in a straight polygon edge. The fix takes the alpha and leaves the
RGB alone: with `SRC_ALPHA/ONE` it scales the additive contribution to nothing
at the edges without darkening the sheet.

**The animation** is implemented; the format is below. Everything else in the
material path was measured and ruled out:

* **Blend mode is correct.** Every fire material declares mode 1 — additive —
  and the runtime receives it (`[scene] blend modes: 0=322 1=150
  (FX_fire_Dahlia)`).
* **The pass was wrong and is now fixed**, but was not the cause: fire already
  reached the transparent pass through its `FX_` prefix.
* **The palette decodes correctly.** `FX_fire_Dahlia` is 8-bit; its CLUT is the
  full 1024 bytes so the GS reorder runs, and the sheet is dark red on black at
  alpha 128 — authored so that additive blending drops the background out. It is
  99% fully opaque, which is why no alpha test can shape it.
* **Addressing is WRAP**, not CLAMP, so a stretched edge texel is not it.
* **It is not a frame atlas.** Mesh UVs span `u 0..4, v -1..1` — the sheet is
  tiled, not indexed.

The cause is **UV animation, which we do not run**, and the format is now fully
recovered. Fire is a pair of tiled flame sheets scrolled past each other; frozen
at one instant a scrolling sheet is exactly a still rectangular patch, which is
what we draw.

**The material side.** Every fire material carries a `0x0135` UV-animation
plugin (45, 31, 112, 27 and 22 occurrences across the Dahlia sheets) holding a
slot mask and the name `FX_Generic_Torches_FX_Hub6_Torch`. The one fire texture
without the plugin, `FX_fire_Dahlia_e`, is also the only one with a real alpha
cutout — a plain sprite, and it needs no animation.

The material's `0x011F` Climax UserData decodes cleanly and gives **two layers**:

    k0 float 0.0   l0 int 0   f0 int 0   t0 int 3   n0 "FX_fire_Dahlia"
    k1 float 0.0   l1 int 0   f1 int 0   t1 int 3   n1 "FX_fire_Dahlia_f"

which is why the accessor in the executable is `ClimaxT1MaterialGetUVMatrices`,
plural. We draw only `n0`.

**Where the animations live.** An earlier search for a `0x2B` chunk found
nothing because it looked for a RenderWare chunk carrying the version word.
`0x2B` is not a chunk — it is an **RWS section type**, confirmed by Ghost Rider:

    GetTypeID__Q214ResourceLoader24CUVAnimationStreamLoader -> 0x2B
    GetTypeID__Q214ResourceLoader27CHAnimAnimationStreamLoader -> 0x1B

`DH_1_Exterior` holds **four `0x2B` sections** (and sixteen `0x1B`), which the
loader currently walks past.

**Layout**, read out of the two sections in `DH_1_Exterior`:

    0x2B section
      0x0001 Struct, 4 bytes -> u32 animation count (12 in the first section)
      per animation: a 0x001B chunk whose payload is a standard RtAnimAnimation
        +0x00  u32   version   = 0x100
        +0x04  u32   typeID    = 0x1C1   (Climax keyframe scheme)
        +0x08  u32   numFrames = 16
        +0x0C  u32   flags     = 0
        +0x10  f32   duration  = 28.0 s
        +0x14  u32   0
        +0x18  char  name[32]  "FX_Generic_Torches_FX_Hub6_Torch"
        then the keyframes

The second section's first animation is `JDMaterialNode13`, 2 frames, 12.0 s.

The header is **88 bytes**: the 20 above plus a 68-byte `RpUVAnimCustomData`
(one int, the 32-byte name, then saved runtime pointers). Ghost Rider confirms
the whole thing is stock RenderWare — it exports `RpUVAnimLinearKeyFrame*`,
`RpUVAnimParamKeyFrame*`, `_rwStreamReadSingleUVAnim` and
`RtDictSchemaStreamReadDict`.

**A keyframe is 32 bytes on disk** — an `RpUVAnimLinearKeyFrame`, which is
`{prevFrame, time, matrix[6]}`. (`ClimaxT1KeyFrameStreamGetSizeCB` returns
`numFrames * 0x18`; that 24 is the size in memory, where the pointer replaces
two of the streamed words.) Every animation in the game uses typeID `0x1C1`,
the linear scheme:

    +0x00 f32 time        +0x14 f32 uOffset
    +0x04 f32 (unidentified, always -0.0)
    +0x08 f32 uScale      +0x18 f32 vOffset
    +0x0C f32 vScale      +0x1C u32 previous keyframe index
    +0x10 f32 (always 0)

Following the previous-frame links splits the frames into one chain per texture
layer. The torch clip has two, and they are exactly the dual-sheet setup the
UserData describes:

    layer 0: scale ( 1.0,  1.0)  offset scrolls U at -1.0/s, V at -0.25/s
    layer 1: scale ( 0.8, -0.8)  offset scrolls U at -0.5/s, V at -0.25/s

The offsets reach whole texture units at the end of the clip (-28.0 after 28 s),
so under WRAP addressing the loop closes seamlessly.

One trap worth recording: **the sections are not word aligned** —
`DH_1_Exterior` has one at offset 1053589 — so a scan with a stride of 4 finds
nothing at all.

**Still open on fire:**

* Only the first layer is drawn. The material's `n1` sheet needs a second draw
  with its own UV transform before the flame looks like the game's.
* Names in the file are truncated to 31 characters, so clips whose names share a
  prefix collide in the lookup table — 74 animations collapse to 21 entries in
  `DH_1_Exterior`. The material's reference is truncated identically, so the
  match still succeeds, but a fire can pick up a same-prefixed neighbour's clip.
  Whether the engine disambiguates these is not known.

### 4. Rooms with no lighting

`HO_1_WomensRoom` and others render unlit. Vertex colours are present in every
packet, so it is not a missing stream; suspect the world's own light data, or a
section whose colours are genuinely zero.

### 5. Textures missing versus materials with no texture

A mesh whose `texName` resolves but whose texture is absent renders black, and
so does a material that legitimately has no texture — the big black wall panel
in `HO_1_Lobby`. These two cases have to be told apart before either can be
diagnosed.

### 6. Native fog

`CFogConfig`, one instance per level, visible in the type table and not read.

### 7. Smaller items

* Collision face indices are read as `uint8`, so nothing past vertex 255 is
  reachable.
* `CStaticCamera` properties beyond the transform are not decoded, so
  jump-to-camera framing is approximate — no FOV, no tilt.
* `baseColorFactor` is not written for untextured materials in the glTF export,
  so they come out white although the viewport has them right.
* FX sheets in character and enemy containers are full-size particle blanks and
  draw as huge quads; the game scales them at runtime and no marker for that has
  been found.
* Rare: something renders black where it should be transparent. Not yet
  reproduced on a named asset.
* Two blended layers are not depth-sorted against each other, so they can
  composite in the wrong order.

---

## Both games

Executable analysis has started — see [EXECUTABLES.md](EXECUTABLES.md).

Done: both binaries are mapped, the PS2 one keeps all its section names and
58 KB of VU1 microcode, and its **complete class registry is recovered** — 126
classes with factory address and object size, in
[`sho_class_registry.json`](sho_class_registry.json).

Next, in order of value:

1. Read a few PS2 factories to learn how properties are bound. That settles the
   per-class property semantics for both games, and with it the Wii `0x0704`
   parsing and the volume classes whose property 1 is an extent, not a matrix.
2. The PS2 blend mode, set through the GS `ALPHA` register, which would replace
   the hand-maintained additive list.
3. The same registry walk on `main.dol`; the technique is identical but the
   scan needs indexing to cover its 4 MB text segment.
