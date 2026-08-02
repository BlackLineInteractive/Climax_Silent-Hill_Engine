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
container** — the same situation as the PS2 blend mode living in the GS `ALPHA`
register. What the asset does carry is the `0x0129` material extension: a single
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

### 1. UV tile offset — the biggest remaining defect

Character parts address neighbouring atlas tiles, so `GL_REPEAT` folds them onto
one quadrant and, for example, the face samples hair. Measured on
`TravisWithAlessaAmbassador`: `Ambassador_1` V 0.004–1.996, `Ambassador_2` U and
V both 1.0–2.0, `Burnt_Alessa_1` U 1.01–1.99, face 0–1.

The source of the offset has **not** been found. Character materials do carry
non-empty `0x0120` / `0x011F` extensions where level materials do not, which is
the obvious place to look next.

### 2. Animation

Specified in [ANIMATION_SPEC.md](ANIMATION_SPEC.md) and not implemented. The
riskiest unknown is settled: skin weights are **not** in the VIF packets — all
104 packets of `CIGCCharacter.Alessa` carry exactly four streams at VU addresses
0–3 — so they live in `Skin PLG (0x0116)` in its native layout, and step 1 of
the spec can be skipped. Start from the skeleton (FrameList + HAnim) drawn as a
bone overlay in the rest pose.

### 3. Rooms with no lighting

`HO_1_WomensRoom` and others render unlit. Vertex colours are present in every
packet, so it is not a missing stream; suspect the world's own light data, or a
section whose colours are genuinely zero.

### 4. Textures missing versus materials with no texture

A mesh whose `texName` resolves but whose texture is absent renders black, and
so does a material that legitimately has no texture — the big black wall panel
in `HO_1_Lobby`. These two cases have to be told apart before either can be
diagnosed.

### 5. Native fog

`CFogConfig`, one instance per level, visible in the type table and not read.

### 6. Smaller items

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
