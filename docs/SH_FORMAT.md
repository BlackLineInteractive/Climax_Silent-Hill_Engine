# Climax Silent Hill Engine — File Format Reference

This document describes the container and archive formats used by the Climax
engine in *Silent Hill: Origins*. It is derived from analysis of the retail PAL
PlayStation 2 release (`SLES_551.47`, `SH.ARC` dated 2008-02-29, 491 170 829
bytes). Where a field's meaning has been confirmed against the data it is stated
plainly; where it has not, it is marked as unverified.

All integers are little-endian. All offsets are byte offsets from the start of
the enclosing structure unless stated otherwise. Sizes in tables are in bytes.

---

## 1. Notation and conventions

| Term | Meaning |
|------|---------|
| `u8`, `u16`, `u32`, `i32`, `f32` | Unsigned / signed integer, IEEE-754 single |
| *chunk* | A RenderWare-style block: 12-byte header followed by a payload |
| *section* | A top-level `0x0716` chunk holding one named resource |
| *object* | A top-level `0x0704` chunk holding one placed game-object instance |
| *qword* | 16 bytes, the PlayStation 2 DMA transfer unit |

The engine is built on RenderWare 3. Chunk type identifiers below `0x0100` are
stock RenderWare; identifiers in the `0x07xx` range are Climax extensions.

Throughout the format a single version constant appears in every chunk header:

```
RW_VERSION = 0x1C020065
```

This value corresponds to RenderWare 3.7.0.2. Its presence is the most reliable
way to distinguish a genuine chunk header from coincidental data, and every
parser in this repository uses it as such.

---

## 2. `SH.ARC` — the game archive

### 2.1 Purpose

`SH.ARC` is a flat, indexed, individually compressed archive holding every
runtime asset: level containers, texture dictionaries, object definitions and
menu images. `IGC.ARC` uses the same layout and holds in-game cutscene data.

### 2.2 Layout

An archive is four regions in fixed order:

```
+---------------------------+  0x00000000
| Header               20 B |
+---------------------------+  0x00000014
| Entry table   16 B x N    |
+---------------------------+
| Payloads (zlib streams)   |
+---------------------------+  nameTableOffset
| Name table                |
+---------------------------+  end of file
```

### 2.3 Header

| Offset | Type | Field | Description |
|--------|------|-------|-------------|
| 0x00 | `char[4]` | `magic` | `"A2.0"` (41 32 2E 30) |
| 0x04 | `u32` | `entryCount` | Number of entry records |
| 0x08 | `u32` | `firstDataOffset` | Offset of the first payload. Equal to `entry[0].offset`; redundant |
| 0x0C | `u32` | `nameTableOffset` | Offset of the name table. Also the end of the payload region |
| 0x10 | `u32` | `nameTableSize` | Length of the name table |

The name table always terminates the file:

```
nameTableOffset + nameTableSize == fileSize
```

This identity is an inexpensive and effective integrity check. A reader should
apply it before allocating anything, since a truncated archive fails it
immediately.

### 2.4 Entry table

`entryCount` records of 16 bytes each, beginning at offset `0x14`.

| Offset | Type | Field | Description |
|--------|------|-------|-------------|
| 0x00 | `u32` | `nameOffset` | Byte offset into the name table |
| 0x04 | `u32` | `offset` | Absolute file offset of the payload |
| 0x08 | `u32` | `compressedSize` | Length of the stored zlib stream |
| 0x0C | `u32` | `uncompressedSize` | Length after inflation |

Entries are ordered by ascending `offset`. Every `offset` is a multiple of
`0x20`; the packer pads each payload to that alignment, so the gap between
consecutive entries is `compressedSize` rounded up to 32 bytes.

### 2.5 Name table

A concatenation of NUL-terminated ASCII strings, one per entry, in entry order.
`nameOffset` indexes into this block. Names carry no directory component. Files
that belong to the level data have no extension (`MO_1_Room102`); everything else
is distinguished by one (`.txd`, `.jpg`, `.xml`, `.cmi`).

Object-definition entries use a dotted naming convention in which the part before
the dot is a class name and the part after is an instance name, for example
`CEnemyBehaviour.Butcher` or `CInventoryItemDef.Key_TO_TheatreTicket`. Such names
are not file extensions and should not be treated as such.

### 2.6 Payload encoding

Every payload is a raw zlib stream in the sense of RFC 1950, beginning with the
two-byte header `78 DA` (deflate, 32 KiB window, maximum compression). No entry
is stored uncompressed. Inflating an entry must yield exactly
`uncompressedSize` bytes; any other result indicates a damaged archive.

### 2.7 Verification

The layout above was checked exhaustively against the retail PS2 `SH.ARC`:

| Property | Result |
|----------|--------|
| Entries | 1487 |
| Payloads beginning `78 DA` | 1487 / 1487 |
| Payloads inflating to the declared size | 1487 / 1487 |
| Offsets strictly increasing | yes |
| Offsets aligned to `0x20` | yes |
| `nameTableOffset + nameTableSize == fileSize` | exact |

Composition by extension: 657 `.txd`, 269 extensionless containers, 266 `.jpg`,
40 `.xml`, 16 `.cmi`, and 239 dotted object definitions.

Reference implementation: [`src/Arc.cpp`](src/Arc.cpp).

---

## 3. The level container

An extensionless archive entry is a *container*: one self-contained level or
zone, holding its geometry, collision, audio banks, navigation mesh, and the list
of game objects placed within it.

### 3.1 Overall structure

```
+---------------------------------------+  0x00
| 0x071C type directory                 |
+---------------------------------------+
| Chunk                                 |   0x0716 resource section
| Chunk                                 |   0x0704 game object
| Chunk                                 |   ...
+---------------------------------------+  end of file
```

After the directory the file is a flat, non-nested list of chunks. Sections and
objects are interleaved in no fixed order.

### 3.2 Chunk header

Every chunk, at every level of the format, begins with the same 12 bytes:

| Offset | Type | Field |
|--------|------|-------|
| 0x00 | `u32` | `type` |
| 0x04 | `u32` | `size` — payload length, excluding this header |
| 0x08 | `u32` | `version` — always `RW_VERSION` |

The next chunk begins at `offset + 12 + size`.

### 3.3 Type directory (`0x071C`)

The container opens with a directory enumerating the game-object classes it
contains and the instance count of each. It is a chunk like any other; its
payload begins at `0x0C`.

| Offset | Type | Field |
|--------|------|-------|
| 0x0C | `u32` | Number of entries |
| 0x10 | — | First entry |

Each entry is a NUL-terminated ASCII class name, padded with NUL bytes to the
next 4-byte boundary, followed by a `u32` instance count. The directory ends at
`0x0C + size`.

The counts are authoritative. Summing them gives the exact number of `0x0704`
chunks that follow, which makes the directory a useful self-check on any object
parser — see §3.7.

### 3.4 Resource section (`0x0716`)

A section wraps one named resource. Its payload begins with a variable-length
header:

| Type | Field | Description |
|------|-------|-------------|
| `u32` | `count` | Purpose unverified; varies per section type |
| `u32` | `tagLen` | Length of the tag field that follows |
| `u8[tagLen]` | `tag` | Optional name, NUL-terminated and padded. Usually empty (`tagLen == 4`) |
| `u8[16]` | `guid` | Identifier by which game objects reference this section |
| `u32` | `nameLen` | Length of the section name |
| `u8[nameLen]` | `name` | Resource type name, e.g. `rwID_WORLD` |
| `u32` | `path1Len` | |
| `u8[path1Len]` | `path1` | Absolute build path of the source asset |
| `u32` | `path2Len` | |
| `u8[path2Len]` | `path2` | Absolute build path of the source texture directory |

The resource data follows immediately after `path2`.

`tagLen` is not constant, and a parser that assumes it is will read the section
name from the wrong offset. Most sections leave the tag empty, but
`rwID_AINAVMESH` stores the source navigation-mesh filename there, for example
`MO_1_Room102_Navmesh.nav`, which shifts everything after it by 24 bytes.

The `guid` matches the GUID embedded in `path1`. In
`Z:\Silent Hill Origins\...\{88D8905A-83C8-4503-BE96-3EE17C9ECC04}.bsp`, the
braced value is the same 16 bytes in the usual mixed-endian GUID ordering. The
build paths are left in the retail data and are a useful cross-check.

#### Observed section names

| Name | Contents |
|------|----------|
| `rwID_WORLD` | Static level geometry, already in level space |
| `rwID_CLUMP` | RenderWare clump: a re-usable model with a frame hierarchy |
| `rwID_RWS` | RenderWare stream: a re-usable model |
| `rwID_CBSP` | Collision BSP |
| `rwID_POLYAREA` | Polygonal area, used to constrain camera movement |
| `rwID_SPLINE` | Spline path |
| `rwID_AINAVMESH` | AI navigation mesh |
| `rwaID_WAVEDICT` | Audio wave dictionary |
| `rwID_AUDIOCUES` | Audio cue table |
| `rwID_HANIMANIMATION` | Hierarchical (skeletal) animation |
| `rwID_DMORPHANIMATION` | Delta-morph animation, used for facial animation |
| `JPG` | Embedded JPEG image |

### 3.5 Game object (`0x0704`)

A `0x0704` chunk is one placed instance of a game-object class. Its payload is a
`u32` of unknown purpose followed by a flat list of tagged records:

```
u32  recordSize      total size of the record, including these 8 bytes
u32  recordId
u8   payload[recordSize - 8]
```

`recordId` packs a kind in its most significant byte and a property index in the
low 24 bits:

| Kind | Payload | Meaning |
|------|---------|---------|
| `0x20` | text | Class name of the component being described |
| `0x40` | 16 bytes | GUID of this component |
| `0x80` | text | Instance or base-class name |
| `0x00` | varies | Indexed property, index taken from the low 24 bits |

Text payloads are NUL-terminated and then padded to `recordSize` with the filler
byte `0xBF`. A reader must stop at the first `0x00` or `0xBF`.

An object is composed of several *components*, each contributing its own block of
indexed properties. The property index restarts at zero for each component, so a
component boundary is identified by the index failing to increase. Record kinds
`0x20`, `0x40` and `0x80` do not reliably delimit components and should not be
used for that purpose.

#### 3.5.1 Placement

A property with a 64-byte payload and index 1 is the object's world transform: a
4x4 matrix in column-major order.

**The fourth row of the stored matrix is entirely zero, including `m[3][3]`.**
The value written is a 3x4 affine transform occupying a 4x4 slot; the homogeneous
row was never filled in. A consumer must set `m[0][3] = m[1][3] = m[2][3] = 0`
and `m[3][3] = 1` before use. Using the matrix as stored gives every transformed
vertex a `w` of zero, and the perspective divide then sends the geometry to
infinity.

This applies to all 11 801 matrices present in the retail archive, without
exception.

Column 3 is the translation. Columns 0 through 2 are the basis vectors and carry
scale where the object is scaled, which is common for trigger volumes.

An identity transform is meaningful and not an error. Classes that exist purely
as logic — `CZone`, `CZoneCollision`, `GameMessage`, `CMessageRelay`,
`CAudioLocatorBehaviour` — have no position and legitimately store identity.

#### 3.5.2 Resource references

A property with a 16-byte payload is the GUID of a `0x0716` section that the
object owns. This is the mechanism by which model geometry is placed in the
level.

Geometry in a `rwID_WORLD` section is baked into level space and is drawn once,
untransformed. Geometry in a `rwID_RWS` or `rwID_CLUMP` section is a re-usable
model stored in its own local space; the object holding the reference supplies
the placement. The same section is frequently referenced by several objects, each
producing an independent instance. In `IntroRoad` a single `rwID_RWS` section is
referenced by four `CSceneObject` instances at four separate positions, and one
`rwID_CLUMP` by two `CPhysicsObject` instances.

Reference counts across the retail archive, by referenced section type:

| Section | References |
|---------|-----------:|
| `rwID_WORLD` | 609 |
| `rwID_CLUMP` | 422 |
| `rwID_POLYAREA` | 376 |
| `rwaID_WAVEDICT` | 216 |
| `rwID_AUDIOCUES` | 216 |
| `rwID_RWS` | 163 |
| `rwID_AINAVMESH` | 154 |
| `rwID_HANIMANIMATION` | 148 |
| `rwID_DMORPHANIMATION` | 63 |
| `JPG` | 40 |
| `rwID_SPLINE` | 39 |
| `rwID_CBSP` | 1 |

A model section that no object references has no defined position. Such sections
exist in most containers and should not be drawn at the origin by default.

### 3.6 Decoded classes

#### `CColorLight`

| Component | Index | Type | Meaning |
|-----------|-------|------|---------|
| 0 | 0 | `u8[4]` | Colour, R G B A |
| 1 | 0 | `i32` | Light type. Values 2 and 3 observed |
| 1 | 1 | `f32` | Cone angle, degrees. Values above 180 indicate an omnidirectional light |
| 1 | 2 | `f32` | Range, world units |
| 1 | 3 | `i32` | Enabled flag |

Sample values from `MO_1_Room102`: `95 95 95 FF` at 45° / 10 units;
`19 19 19 FF` at 173.16° / 3210 units; `95 95 5D FF` at 45° / 5 units.

#### `CStaticCamera`

Component 0 properties 0 through 8 are floats and integers whose individual
meanings are **not yet established**. Values from `MO_1_Room102` are
`1.25, 2.0, 3.0, 0, 90.0, 0.2, -1, 0.5, 1.5`. The 90.0 is plausibly a field of
view and the 0.2 a near plane, but neither has been confirmed. The camera's
position and orientation come from the transform described in §3.5.1, not from
these properties.

Camera classes observed: `CStaticCamera`, `CConstraintCamera`, `CIGCCamera`.

### 3.7 Verification

Parsing every extensionless entry in the retail archive and comparing the number
of `0x0704` chunks found against the total declared by each container's own
`0x071C` directory:

| Result | Count |
|--------|------:|
| Containers matching exactly | 254 |
| Containers not matching | 1 |
| Containers failing to inflate | 0 |
| Objects parsed | 11 834 |
| Objects carrying a non-identity transform | 7 065 |

The single mismatch is `GlobalStream`, whose directory declares one
`CWiiStndController`. No such chunk is present; the PlayStation 2 build does not
ship the Wii controller configuration that the shared directory still lists.

Reference implementation: [`src/Loader.cpp`](src/Loader.cpp).

---

## 4. Geometry

Level geometry is stored as PlayStation 2 display lists: sequences of VIF1
commands that upload vertex data to VU1 memory and invoke a microprogram. The
format therefore cannot be read as a plain vertex array; the command stream must
be walked.

> **Status.** The structure and the traversal rule below have been confirmed
> against `IntroRoad`. The loader currently in the repository still uses an older
> byte-pattern scan and does not implement what follows.

### 4.1 Material list

Materials are stored in stock RenderWare chunks:

| Type | Chunk |
|------|-------|
| `0x0008` | Material list |
| `0x0007` | Material |
| `0x0006` | Texture |
| `0x0002` | String |

A material list opens with a `0x0001` struct chunk whose first `u32` is the
material count, followed by that many `0x0007` material chunks. Each material
contains a `0x0006` texture chunk, whose first `0x0002` string child is the
texture name. That name is the key into the texture dictionary.

### 4.2 Batch table (`0x050E`)

A `0x050E` chunk precedes each geometry object and declares how the vertex stream
is divided between materials.

| Offset | Type | Field |
|--------|------|-------|
| 0x0C | `u32` | `flags` — 0 selects indexed triangle lists, 1 selects strips |
| 0x10 | `u32` | `meshCount` |
| 0x18 | — | First batch record |

Each batch record is:

| Offset | Type | Field |
|--------|------|-------|
| 0x00 | `u32` | `indexCount` |
| 0x04 | `u32` | `materialIndex` |

When `flags` is 0 the record is followed by `indexCount` `u32` indices; when it
is 1 it is not. Batches appear in the same order as the geometry that follows.

The counts are index counts, not counts of vertices uploaded to the VU. The two
differ, because a strip carries degenerate and adjacency vertices that are not
indexed. Any material assignment that accumulates raw upload counts against these
figures will drift. Measured on the main batch table of `IntroRoad`: 37 batches
declaring 57 396 indices against 59 096 vertices actually uploaded.

### 4.3 VIF command stream

The stream is a sequence of 32-bit VIF codes. Each code is:

| Bits | Field |
|------|-------|
| 31 | Interrupt |
| 30–24 | Opcode |
| 23–16 | `num` |
| 15–0 | `immediate` |

Opcodes relevant to this format:

| Opcode | Name | Trailing data |
|--------|------|---------------|
| `0x00` | `NOP` | — |
| `0x01` | `STCYCL` | — (`immediate` carries `CL` in bits 0–7, `WL` in bits 8–15) |
| `0x02` | `OFFSET` | — |
| `0x03` | `BASE` | — |
| `0x04` | `ITOP` | — |
| `0x05` | `STMOD` | — |
| `0x10`, `0x11`, `0x13` | `FLUSHE`, `FLUSH`, `FLUSHA` | — |
| `0x14`, `0x15`, `0x17` | `MSCAL`, `MSCALF`, `MSCNT` | — |
| `0x20` | `STMASK` | 4 bytes |
| `0x30`, `0x31` | `STROW`, `STCOL` | 16 bytes |
| `0x4A` | `MPG` | `num x 8` bytes |
| `0x50`, `0x51` | `DIRECT`, `DIRECTHL` | `immediate x 16` bytes |
| `0x60`–`0x7F` | `UNPACK` | see below |

For `UNPACK`, bits 2–3 of the opcode give `VN` (component count minus one) and
bits 0–1 give `VL` (element width):

| `VL` | Width | Note |
|------|-------|------|
| 0 | 32 bits | |
| 1 | 16 bits | |
| 2 | 8 bits | |
| 3 | 16 bits total when `VN == 3`, otherwise 16 bits per component | V4-5 packed colour |

Bytes consumed by the payload:

```
bytesPerVector = ceil(components * bitsPerComponent / 8)
writeCount     = (num == 0) ? 256 : num
sourceCount    = writeCount
if (CL < WL):
    sourceCount = (writeCount / WL) * CL + min(writeCount % WL, CL)
payloadBytes   = align4(sourceCount * bytesPerVector)
```

`num` of zero denotes 256 vectors, not none. This transcription of the opcode
table and the payload-size rule follows the VIF1 interpreter in the
[PS2Recomp](https://github.com/) runtime.

### 4.4 Vertex packet

A packet uploads one triangle strip and kicks the microprogram. Its layout is
fixed, and each of the four data streams is preceded by its own `STCYCL`:

```
STCYCL   CL=4 WL=1
UNPACK   V3-32   imm 0x8000    positions        12 bytes per vertex
STCYCL   CL=4 WL=1
UNPACK   V2-32   imm 0x8001    texture coords    8 bytes per vertex
STCYCL   CL=4 WL=1
UNPACK   V4-8    imm 0xC002    vertex colours    4 bytes per vertex
STCYCL   CL=4 WL=1
UNPACK   V3-8    imm 0x8003    normals           3 bytes per vertex
ITOP     vertex count
MSCALF
FLUSH
FLUSH
```

`CL = 4` with `WL = 1` writes every fourth VU quadword, so the four streams
interleave into a stride-4 vertex layout: positions occupy VU addresses 0, 4, 8
and so on, texture coordinates 1, 5, 9, and so forth.

Bit 15 of each `immediate` marks the address as relative to the double-buffer
pointer `TOPS`; the low bits give the stream's slot, 0 through 3. The colour
stream additionally sets bit 14, which selects zero-extension rather than sign-
extension of its 8-bit components — correct for unsigned colour.

Some sections use `V4-32` for positions instead of `V3-32`, giving a 16-byte
stride. Both forms occur in `IntroRoad`.

Every `UNPACK` in a packet carries the same `num`. That value is the vertex count
of the strip.

Vertex colours are stored here, in the same packet as the positions. They are the
level's baked lighting; a level that renders unlit is a symptom of the colour
stream being missed, not of the data being absent.

Packets are separated by a run of the filler byte `0xE5` extending to the start of
the next packet, and by a 16-byte header that is not VIF. Neither needs to be
decoded if the walker anchors on packet starts rather than reading the stream
continuously — see §4.5.

### 4.5 Traversal

A packet is located by scanning 4-byte-aligned words for the position `UNPACK`:
a 32-bit unpack of three or four components whose `immediate` is exactly
`0x8000`. From that anchor the packet reads cleanly to its `MSCAL`, and the
inter-packet filler and header are stepped over without being interpreted.

Anchoring on `STCYCL` alone does not work. Its encoded word occurs frequently
inside vertex data — 3548 times in the first `rwID_WORLD` section of `IntroRoad`
at arbitrary alignment against 1200 genuine occurrences — so the anchor must be
the `UNPACK` itself.

Applied to the first `rwID_WORLD` section of `IntroRoad`, the scan finds 300
packets. Every one of them has the identical four-stream layout above, with no
spurious streams. The count is corroborated independently: the section contains
1200 aligned `STCYCL` words, exactly four per packet, and 300 position `UNPACK`
words.

### 4.6 Relating packets to materials

The batch table counts triangle indices; a packet carries strip vertices. The two
are related by

```
indices contributed by a packet = 3 * (vertexCount - 2)
```

For the first `rwID_WORLD` section of `IntroRoad`, 300 packets carrying 20 054
strip vertices yield roughly 59 400 indices against the 57 396 the batch table
declares, the remainder being accounted for by short strips.

Material assignment therefore walks packets in order, accumulating that index
count against each batch's declared `indexCount` and advancing to the next batch
when the total is reached. Accumulating raw upload counts instead, as the older
loader does, drifts immediately and shifts every subsequent piece of geometry
onto the wrong material.

---

## 5. Collision (`rwID_CBSP`)

The collision section contains a `0x1100` chunk. In the retail containers this
chunk does not begin at the section's data offset; it is preceded by 8 bytes of
data whose meaning is unverified, so a reader must scan for it rather than assume
alignment.

Within the `0x1100` payload:

| Offset | Type | Field |
|--------|------|-------|
| 0x08 | `u32` | Vertex count |
| 0x0C | `u32` | BSP node count |
| 0x20 | — | First vertex |

Vertices are 16 bytes: three `f32` coordinates and 4 bytes of flags. The face
list follows the vertex array and the node array, the latter being 8 bytes per
node. Each face is 4 bytes: three `u8` vertex indices and one flag byte.

**The `u8` index width limits a face to referencing the first 256 vertices.**
Sections declaring more vertices than that exist, and their faces beyond the
limit cannot be expressed in this record layout. Either an additional index
mechanism has been missed, or the geometry is partitioned by the BSP node array
in a way that keeps each leaf's indices within range. This has not been resolved,
and collision output should be treated as incomplete.

---

## 6. Texture dictionaries

Texture dictionaries are stored either as separate `.txd` archive entries or
embedded in a container. Naming for shared dictionaries follows the rooms they
bridge: `MO_1_Room102.txd` holds textures unique to that room, while
`MO_1_Room102-MO_1_PoolArea.txd` holds textures shared by the two, and both
orderings of the pair occur.

### 6.1 Texture record

Records are chunks of type `0x15`. Relative to the start of the pixel-format
block:

| Offset | Type | Field |
|--------|------|-------|
| 0x00 | `u32` | Width |
| 0x04 | `u32` | Height |
| 0x08 | `u32` | Bit depth |
| 0x30 | `u32` | Pixel data size |
| 0x34 | `u32` | Palette data size |
| 0x38 | `u32` | Horizontal wrap mode |
| 0x3C | `u32` | Vertical wrap mode |

Both the pixel block and the palette block begin with an 80-byte header that is
skipped; the payload follows it.

### 6.2 Pixel formats

Textures are stored in native PlayStation 2 GS layouts and must be unswizzled.
Only the 4- and 8-bit paletted formats are handled by the current decoder.

For 8-bit data the GS block layout is undone with the standard PSMT8
deswizzle, and the 1024-byte palette itself is reordered, since the GS stores
CLUT entries in an interleaved order:

```
newIndex = (p & 0xE7) + ((p & 0x08) << 1) + ((p & 0x10) >> 1)
```

The 4-bit path currently expands nibbles to bytes and reuses the 8-bit
deswizzle. **This is not the correct PSMT4 layout** and is a known source of
incorrect texture appearance.

Alpha is stored in the PlayStation 2 convention where 0x80 represents full
opacity, so decoded alpha is doubled and clamped. Vertex colours use the same
convention: 128 is full intensity, not 255.

---

## 7. Animation

Two animation section types appear in containers, both tagged with their source
filename:

| Section | Example tag |
|---------|-------------|
| `rwID_HANIMANIMATION` | `IGC_2_Chase.Travis.anm` |
| `rwID_DMORPHANIMATION` | `IGC_2_Chase.Face.dma` |

The first is hierarchical skeletal animation, the second delta-morph animation
used for faces. Animations are referenced by GUID from `0x0704` objects in the
same way as geometry, so the object that owns an animation is identifiable.

The internal layout of neither section has been analysed.

---

## 8. Coordinate system

The engine uses a right-handed, Y-up coordinate system. Level geometry and object
transforms share it directly; no conversion is applied when rendering, and none
is applied when exporting to glTF, which uses the same convention.

---

## 9. Sources

All figures in this document were produced by parsing the retail PAL PlayStation 2
release. The VIF opcode table and `UNPACK` sizing rules in §4.3 follow the VIF1
interpreter in the PS2Recomp runtime.

Implementations in this repository:

| File | Covers |
|------|--------|
| [`src/Arc.cpp`](src/Arc.cpp) | §2 |
| [`src/Loader.cpp`](src/Loader.cpp) | §3, §4, §5, §6.1 |
| [`src/PS2Texture.cpp`](src/PS2Texture.cpp) | §6.2 |
| [`src/Export.cpp`](src/Export.cpp) | §8 |
