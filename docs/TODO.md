# Work queue

Ordered by value, with what is already known about each so the next attempt
does not start from nothing. Format references: [SH_FORMAT.md](formats/SH_FORMAT.md)
for Origins, [SHSM_ARC_FORMAT.md](formats/SHSM_ARC_FORMAT.md) for Shattered Memories.

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
format design; see [ANIMATION_SPEC.md](formats/ANIMATION_SPEC.md).

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
`rwID_MTEFFECTDICT` — catalogued, contents unknown.

`rwID_KFONT` is decoded — see [SH_FORMAT.md](formats/SH_FORMAT.md). Four blocks:
controller glyphs, a 16-colour palette, kerning pairs, and the character set
(137 in `FontEUR`, 1261 in `FontJAP`).

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

### 2. Animation — working

Characters animate. Clips are read, the pose is evaluated, rigid pieces follow
their frame and weighted pieces go through the shader's skinning branch.

**Finding the clips.** They are `0x1B` chunks sitting directly in the stream,
not inside a `0x716` shell, and they are not word aligned — the same pair of
traps the UV animations sprang, which is why the existing decoder had never once
run. `CPlayerBehaviour.Travis` yields 147 clips; the archive holds 3029. The
layout is confirmed by arithmetic over all of them: every clip satisfies
`chunkSize == 20 + 24 + records*20`.

**Track roots are marked by a saved pointer, not by a zero.** Each record holds
a byte offset back to the previous keyframe of its track, but the first record
of each track keeps the runtime pointer the file was written with — large
negatives stepping by exactly the 20-byte stride. Testing `prevOffset == 0` for
a root finds nothing at all. The rule that works is "anything that does not
resolve to an earlier record in this clip". The track count that falls out
matches the HAnim table: 53 tracks against 54 frames, the odd one being the
clump root.

**Where the weights are.** Not in the Skin PLG — Ghost Rider settles it, since
`_rpSkinGeometryNativeRead` never writes `skin[0x14]` or `skin[0x18]`, the two
fields `RpSkinGetVertexBoneIndices` and `RpSkinGetVertexBoneWeights` return. They
ride with the native geometry instead, immediately after the VIF packets: four
floats per vertex, each float's lowest byte holding the bone matrix's address in
VU memory. A matrix spans four quadwords, so the index is `byte / 4`.

**The two defects that actually mattered**, both found by measurement after
guessing at the bone index three times and getting nowhere:

* *Weights slipped two places at every packet seam.* There is one record per
  **unique** vertex, but a triangle strip runs on across packet boundaries, so
  every packet after the first restates the two vertices that closed the one
  before it. A six-packet geometry uploads 220 vertices for 210 records —
  exactly +2 per join. Reading the records straight through accumulated that
  slip and tore the model apart. Each packet after the first now starts two
  records back.
* *The rotations were conjugated.* The reference stores them that way and the
  spec says to reproduce it, then check: if the model inverts, drop it. It
  inverted — Travis faced backwards — so the components go in as they are.

Also fixed along the way: a VIF packet whose stream addressed a VU slot above 3
was discarded **together with its geometry**, and `glm::quat` takes the scalar
first, where the old code passed `(x, y, z, w)`.

**Still open:** facial animation (`rwID_DMORPHANIMATION`), and clips are named
`Clip_N` because the section tag carries the type, not the source filename.

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

### 4. Camera FOV — found and applied

Level cameras had placement but no field of view; the viewport always used a
fixed guess.

Ghost Rider's `HandleAttributes__Q26Camera13CStaticCameraRCQ23RWS16CAttributePacket`
gives `CStaticCamera` a 5-entry property table, but a first attempt to read the
matching field in SHO's own `0x0704` records gave the same bit pattern
(-1.498) on every camera in every level -- a dead giveaway of a bug, not a
constant, since framing varies per camera by design.

The bug: a `0x0704` object is not one flat property list. It is several
**named components back to back**, each restarting its own index at 0 --
`CStaticCamera`, then `CBaseCamera`, then `CBaseBehaviour`, then
`CSystemCommands` (which owns the placement matrix at its own index 1, already
read elsewhere). The scan flattened every component's index space into one
dict and later components overwrote earlier ones, so what looked like
`CStaticCamera` property 0 was actually `CBaseBehaviour` property 0 -- the
same near-constant on every object because it is a base class most cameras
never touch.

Once components are kept separate the values stop being constant -- but the
first candidate found this way, `CStaticCamera` index 4, was **not** the field
of view. It sits at 90.0 on 420 of 473 objects, and Ghost Rider settles what
actually is:

    Camera::CBaseCamera::HandleAttributes
        beql $v0, $s2, 0x00114a18      ; $s2 = 2
        ...
      0x00114a18: jal SetFOV__Q26Camera11CBaseCameraf

So **field of view is `CBaseCamera` property 2**, on the base class every
camera derives from, which is why constraint and cutscene cameras carry it too.
The values look like something a designer picked -- 18 distinct across the
archive (55, 60, 50, 52, 46, ...), including fractional ones like 56.52 and
66.96 within a single level.

That dispatch was invisible at first because the case is reached through a
*branch-likely* instruction, and `tools/mips.py` did not decode the `beql` /
`bnel` / `blezl` / `bgtzl` family at all -- it rendered them as `op0x14.…`.
Fixed; worth remembering that an undecoded opcode hides control flow silently
rather than loudly.

**There are seven camera classes, not one**, and only the fixed ones are
handled so far:

    CStaticCamera       473   fully fixed
    CConstraintCamera   417   follows the player within limits  <- not implemented
    CIGCCamera           53   cutscenes
    CTnfCamera           15
    CFmaCameraNode       13
    CPeepholeCamera      10   door peephole
    CFmaCamera            8

`CConstraintCamera` is the "semi-static" one the player notices in game -- it
tracks Travis inside a constrained volume. Ghost Rider gives it a 21-property
table against `CStaticCamera`'s 5, so the tracking limits and rates are all in
there; none of it is read yet. Note its component chain starts with
`CSystemCommands`, not with the camera class, which is why a component-blind
reader picks up the wrong properties for it.

### 4a. Camera aim — it was in the placement matrix all along

Fixed cameras were aimed at the player rather than where the designer pointed
them, on the conclusion that the placement matrix carried no orientation. That
conclusion came from one level: all four cameras in `HO_1_Hallway1` share a
single rotation, so the matrices looked like placeholders.

Measured across the whole archive they are not:

    989 camera placement matrices
    358 distinct rotations
    175 of 229 levels hold more than one
    307 cameras are pitched, up to 83 degrees down a stairwell

The rows are RenderWare's usual `right / up / at / pos`, so **row 2 is the look
direction**. The sign is settled by the data as well: measured against every
walkable marker in each level (spawners, map items, zone and plane triggers),
`+row2` aims nearer the playable space than `-row2` on 654 cameras to 314, mean
cosine 0.97.

The lesson is the one this file keeps relearning: a property that looks
constant may just be constant *in the level you happened to open*. The check
costs one pass over the archive.

**But the authored aim is not universal, and that matters more than it sounds.**
Using it everywhere made the framing worse, not better, which forced a second
measurement:

    CStaticCamera      473   authored pitch mean -10.5 deg, needed -15.8  ->  error -3.6
    CConstraintCamera  417   authored pitch ZERO on 400 of 417,  needed  -11.1

So `CConstraintCamera` has no authored pitch at all — its vertical aim is
computed at run time, which is exactly what the class name says and why it is
the one that visibly follows Travis. Only `CStaticCamera` is genuinely aimed.

And even `CStaticCamera` is not always: its authored direction holds a walkable
marker in frame on 350 of 471 instances. The other 121 live in the **29 of 201
multi-camera levels where every camera shares one rotation** — a default the
designer never touched. `HO_1_Hallway1` is one of them: all four cameras read
yaw 180.0, pitch 0.0, while the aim needed to see the spawn points ranges from
102 to 163 degrees of yaw and 5 to 25 down.

`Game::CameraAim` therefore honours the authored direction only while it still
holds the subject, and tracks otherwise. That is a fallback, not a reading of
the engine — what the real code does with a defaulted matrix is unknown.

`CBaseCamera` property 1 — the `"...S"` name — is not the aim either. It is the
event name the camera answers to: `HO_1_Hallway1`'s `PlaneTrigger` names
`camHallwayS` and `camEntranceS`, while `CMessageRelay` addresses the plain
`camHallway`. Both names have to resolve or half the level logic goes dark.

### 4b. The message system — how level logic is actually wired

Everything in RenderWare Studio talks through `RWS::CEventHandler`. The core
API, from Ghost Rider:

    RegisterMsg(CEventId&, const char*, const char*)   "I can send this"
    LinkMsg(CEventId&, const char*, unsigned short)    "notify me when it fires"
    LinkMsgToEventHandler(CEventHandler*, CEventId&, const char*, unsigned short)
    QueueMsg(const CMsg&, void*) / FlushQueuedMessages()
    RWS::_SendMsg(CMsg&)                               global dispatch

Note both `RegisterMsg` and `LinkMsg` take **`const char*`** — the wiring is by
**name**, not by pointer or GUID. That was worth checking rather than assuming:
across `DH_1_Hallway`, 0 of 103 GUID references in `0x0704` objects point at
another game object, so objects genuinely do not reference each other that way.

The names are plain string properties on the objects we already parse:

    PlaneTrigger    prop 3 = "camLanding01"    prop 4 = "camLanding02"
    PlaneTrigger    prop 3 = "camStairway02"   prop 4 = "camStairway01"
    CStaticCamera   prop 0 = "camStairway01"   prop 1 = "camStairway01S"
    CZone           prop 0 = "DH_1_Exterior"   prop 1 = "DH_1_Hallway"

So a `PlaneTrigger` is a plane that, when crossed, switches from one named
camera to another — one property per crossing direction. That is the mechanism
behind Silent Hill's hard camera cuts on stairs and in corridors.
`CStaticCamera` publishes the name it is found by, and `CZone` names the zones
it borders.

This also fixes a smaller thing: camera entries in the viewer were labelled
`CStaticCamera` / `CConstraintCamera` because the label was taken from the
component name. The real designer-authored name is property 0.

`CEventHandler::RegisterStreamChunkHandlers` registers chunk **0x712**, but no
0x712 section exists anywhere in `SH.ARC` -- the 13 byte-scan hits are noise
(one per container, no valid structure). The links are carried as the string
properties above, not as a separate chunk.

**Not implemented yet:** nothing dispatches these. Next step is a name table
(object name -> object) plus the trigger/camera pairing, which is enough to
switch cameras when the player crosses a plane -- the first piece of real game
logic, and it needs no format we do not already read.

### 4c. Collision — solved

The collision overlay draws a fan of long spikes converging on a few points.
Two real index bugs were found and fixed on the way (indices were not rebased
between blocks, and a vertex failing the finite check was skipped instead of
occupying its slot, shifting every later index) but neither was the cause.

The cause is that **`rwID_CBSP` holds no triangle list at all.** Ghost Rider's
`CollisionBSP::StreamRead` (reached from
`ResourceLoader::CCBSPStreamLoader::Read`, whose `GetTypeID` returns **0x1100**
-- the inner chunk id seen in the data) reads a magic word `0x0C011B59`, a
version of 1, then **six** counts, then six arrays:

    A x 16   vertices
    B x  8   BSP nodes
    C x  2   u16
    D x 16   planes        (normal + distance: [0,1,0,0], [1,0,0,4.999], ...)
    E x  8   node records  (four u16; the third runs as a sequence, the fourth is flags)
    F x  2   u16 node indices (max 98 against B = 99)

For `HO_1_Hallway1` that is 70/99/29/37/73/136, totalling 3418 bytes against
3438 available -- the layout adds up.

The loader reads the vertices correctly but then assumes triangle indices sit
immediately after the nodes, as `u8` triples with a 4-byte stride. That offset
lands inside C and D, so it has been reading **plane floats as vertex indices**
-- which is exactly the spike pattern. Neither D, E nor F parses as a triangle
list: F indexes nodes rather than vertices, and E's fields are out of vertex
range in 49 of 73 records.

That conclusion was wrong, and the mistake was a name. The second array was
called "BSP nodes" and the search for faces moved past it into C, D, E and F.
Ghost Rider settles it: `CollisionBSP::DebugRenderFace` takes `this->field_04`
-- the **second** array the stream reader fills -- indexes it by
`faceIndex * 8`, and reads three `u16` which it scales by 16 to reach the
vertex array. So that array is the face list:

    struct CollisionFace {   // 8 bytes
        uint16_t v0, v1, v2; // indices into the vertex array, stride 16
        uint16_t flags;      // surface type: 24601, 40984, 24599 seen
    };

Verified before changing any code: all 99 records in `HO_1_Hallway1` index
inside the 70-vertex array, and the triangles come out with a median area of
1.53 against a maximum of 4.25 -- even and small, as a corridor should be.

The fix took the overlay from a fan of spikes to real geometry:

    HO_1_Hallway1   before: mean edge 8.70, max 16.26
                    after : mean edge 3.07, max  4.13

Two genuine index bugs were also fixed on the way and remain worth keeping:
indices were not rebased between blocks, and a vertex failing the finite check
was skipped rather than occupying its slot, shifting every later index.

### 5. Rooms with no lighting

`HO_1_WomensRoom` and others render unlit. Vertex colours are present in every
packet, so it is not a missing stream; suspect the world's own light data, or a
section whose colours are genuinely zero.

### 6. Textures missing versus materials with no texture

A mesh whose `texName` resolves but whose texture is absent renders black, and
so does a material that legitimately has no texture — the big black wall panel
in `HO_1_Lobby`. These two cases have to be told apart before either can be
diagnosed.

### 7. Native fog

`CFogConfig`, one instance per level, visible in the type table and not read.

### 7b. Archive and container browsing as a tree

The archive panel is a flat list, which is fine for finding a level by name and
useless for understanding how a container is put together. Requested: a tree
view — archive → entry → sections → chunks → the objects and geometry inside —
with a switch back to the flat list, and filters and search over both.

The data for it already exists: `g_ShoSections` carries every section with its
offset, size, type and now its asset name, and `g_ContainerChunks` carries the
chunk list. Nothing needs decoding; this is presentation.

### 8. Smaller items

* `CConstraintCamera`'s 21-property table holds its tracking limits; none of
  them are read, so the follow cone in the viewer is a fixed 20° stand-in.
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

Executable analysis has started — see [EXECUTABLES.md](executables/EXECUTABLES.md).

Done: both binaries are mapped, the PS2 one keeps all its section names and
58 KB of VU1 microcode, and its **complete class registry is recovered** — 126
classes with factory address and object size, in
[`sho_class_registry.json`](generated/sho_class_registry.json).

Next, in order of value:

1. Read a few PS2 factories to learn how properties are bound. That settles the
   per-class property semantics for both games, and with it the Wii `0x0704`
   parsing and the volume classes whose property 1 is an extent, not a matrix.
2. The PS2 blend mode, set through the GS `ALPHA` register, which would replace
   the hand-maintained additive list.
3. The same registry walk on `main.dol`; the technique is identical but the
   scan needs indexing to cover its 4 MB text segment.

### 4c. Camera cuts reverse the player's direction — and that is correct

Walking through HO_1_Hallway2 with a key held down, a camera cut turns Travis
around: input is taken relative to the camera, so the same key means the
opposite direction once the shot changes. This is the original game's
behaviour and one of the things people remember about it, so it is kept.

What the game adds on top, and this does not yet, is a latch: the character
holds his existing world direction until the stick is re-centred, so a cut
mid-stride does not immediately spin him. Worth adding as an option rather than
a replacement — the reversal itself is not a bug to fix.
