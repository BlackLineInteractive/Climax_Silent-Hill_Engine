# Animation Support — Technical Specification

Scope: skeletal animation for Climax-engine models, plus a playback UI. Covers
the container side (finding and pairing the data), the format side (skeleton,
skinning, keyframes), and the runtime side (evaluation, GPU skinning, player).

Everything below is grounded in the retail PS2 data and in the reference
implementation at `leeao/SilentHillOriginsPS2`. Offsets and encodings that were
read out of the data are marked **verified**; the rest is marked as such.

---

## 0. Prerequisite: fix container parsing, not the models

The current loader still resolves a few things per-object instead of from the
container structure. Animation must not be layered on top of that — it needs a
correct object graph or every animated model will inherit the same guesswork.

Two things have to be true before starting:

1. **A section payload is a chunk sequence.** Already implemented: `rwID_RWS`
   opens with `0x23`/`0x24` and `0x29` headers and the real `Clump` follows.
   The same walk must be used for every section type, with no special cases.
2. **A model's placement comes from the object graph, not from heuristics.**
   Property 1 of a `0x0704` object is not always a model transform — on volume
   classes such as `CPhysicsObject` it is the extent of a collision box
   (measured scales `(1.0, 3.54, 26.1)` and `(8.87, 5.54, 5.66)` in IntroRoad).
   The class name determines how the matrix is used. Encode that as a table of
   known classes, not as a scale threshold.

Until both hold, animated characters will be mis-scaled the same way props are.

---

## 1. Where the data lives

### 1.1 Sections

| Section | Root chunk | Contents |
|---------|-----------|----------|
| `rwID_CLUMP` | `Clump (0x0010)` | Skeleton (FrameList) + skinned geometry |
| `rwID_RWS` | `0x23` / `0x24` header, then `Clump` | Same, wrapped |
| `rwID_HANIMANIMATION` | `0x001B` | Skeletal animation clip |
| `rwID_DMORPHANIMATION` | *(unverified)* | Delta-morph clip, facial |

The section tag carries the source filename, which is the clip's identity:
`IGC_2_Chase.Travis.anm`, `IGC_2_Chase.Face.dma`. **Verified.**

Counts across the retail archive: 148 `HANIMANIMATION` references, 63
`DMORPHANIMATION`. **Verified.**

### 1.2 Pairing a clip to a model

A `0x0704` object holds 16-byte GUID properties, one per section it owns. An
animated actor references its clump *and* its clips from the same object — for
example `CPlayerSpawner -> rwID_HANIMANIMATION + rwID_DMORPHANIMATION`, and
`CIGCCamera -> rwID_CLUMP + rwID_HANIMANIMATION`. **Verified.**

This is the pairing mechanism. Do not match by name.

---

## 2. Skeleton

### 2.1 FrameList (`0x000E`)

Child of `Clump`. Its `Struct` child holds:

```
u32  frameCount
frameCount x {
    f32  rot[9]        3x3 basis, row-major
    f32  pos[3]
    i32  parentIndex   -1 for root
    u32  flags
}                      56 bytes per frame
```

**Verified** — this is what the current loader already reads for `g_Clumps`.

After the struct there is one `Extension (0x0003)` **per frame**, in order.
Inside each:

| Chunk | Meaning |
|-------|---------|
| `0x011E` | HAnim PLG — bone identity and, on the root, the whole bone table |
| `0x011F` | User-data PLG — bone name |

### 2.2 HAnim PLG (`0x011E`)

```
i32  version
i32  boneId          this frame's bone id
u32  boneCount       non-zero only on the frame that owns the hierarchy
if boneCount:
    i32 flags
    i32 keyFrameSize
    boneCount x {
        i32 boneId
        i32 skinBoneIndex     index into the skin's bone arrays
        i32 boneType
    }
```

The frame with a non-zero `boneCount` is the skeleton root; the table maps
bone ids to skin indices. **Verified** against the reference reader.

### 2.3 Bone names (`0x011F`)

```
i32 numSets
numSets x {
    i32 typeNameLen; skip typeNameLen
    i32 unknown
    i32 unknown
    i32 nameLen; char name[nameLen]     only when nameLen > 1
}
```

Missing name falls back to `RootBone` for frame 1, otherwise `Bone<index>`.

### 2.4 Building the rest pose

Frame matrices are **local**. Compose to world by walking parents in order —
frames are stored parent-before-child, so a single forward pass works:

```
world[i] = (parent[i] >= 0) ? world[parent[i]] * local[i] : local[i]
```

Note the reference multiplies `local * parent`, i.e. row-vector convention.
With glm's column-major types the correct order is `parent * local`. **This is
the single most likely place to get a silently wrong skeleton** — validate by
checking that the rest pose reproduces the unskinned mesh.

---

## 3. Skinning

### 3.1 Skin PLG (`0x0116`)

Lives in the `Geometry`'s `Extension`, alongside `BinMeshPLG (0x050E)` and
`NativeDataPLG (0x0510)`.

Two layouts. Which one applies is decided by the geometry's native flag — the
same flag that already selects between stored indices and PS2 display lists.

**Non-native:**
```
u8   boneCount
u8   usedBoneCount
u8   maxWeightsPerVertex
u8   padding
u8   usedBoneIds[usedBoneCount]
u8   boneIndices[numVerts][4]
f32  boneWeights[numVerts][4]
f32  inverseBoneMatrix[boneCount][16]
f32  padding[3]
```

**Native (PS2)** — the case that matters here:
```
rwChunk struct header (12 bytes)
u32  platform
u8   boneCount
u8   usedBoneCount
u8   maxWeightsPerVertex
u8   padding
u8   usedBoneIds[usedBoneCount]
f32  inverseBoneMatrix[boneCount][16]
i32  padding[7]
```

**Note the asymmetry: the native layout lists no per-vertex indices or weights
after the inverse bind matrices.** The obvious assumption is that they ride in
the VIF packets instead. **They do not** — see §3.2.

### 3.2 Where PS2 skin weights are — measured

Every packet of a character carries exactly the same four streams as static
geometry:

```
addr 0  V3-32 / V4-32   positions
addr 1  V2-32 / V2-16   texture coords
addr 2  V4-8            vertex colours
addr 3  V3-8            normals
```

Measured on `CIGCCharacter.Alessa`: **all 104 packets** use addresses 0..3 and
nothing higher, with the layout `V3-32@0, V2-32@1, V4-8@2, V3-8@3` in every one.
There is no bone stream in the display list.

Indices and weights therefore have to be read out of the `Skin PLG (0x0116)`
itself. This was the one unknown that could have invalidated the whole plan, and
it is now closed.

---

## 4. Animation clip

### 4.1 Clip header

Root chunk `0x001B`, then:

```
i32  version
i32  typeId        0x1103 for the compressed keyframe format
i32  frameCount    total keyframe records, NOT timeline frames
i32  flags
f32  duration      seconds
```

Playback rate is 30 fps in the reference. **Verified.**

### 4.2 Type `0x1103` payload

```
f32  transOffset[3]
f32  transScalar[3]

frameCount x 8 bytes at frameHdrBase:
    i32  prevFrameByteOffset
    f32  time

frameCount x 12 bytes at frameHdrBase + frameCount*8:
    u32  rotCompressed1
    u16  rotCompressed2
    u16  tx, ty, tz
```

**Verified** against the reference reader.

### 4.3 Decoding a keyframe

Rotation is a quaternion packed into 48 bits, four 12-bit fields:

```
qx = ((c1 >> 20)                              - 2048) / 2047
qy = (((c1 >> 8) & 0xFFF)                     - 2048) / 2047
qz = ((((c1 << 4) & 0xFFF) | (c2 >> 12))      - 2048) / 2047
qw = ((c2 & 0xFFF)                            - 2048) / 2047
```

The reference stores the **conjugate**: `(-qx, -qy, -qz, qw)`. Reproduce that
and verify on a known clip; if the model turns inside out, drop the conjugate.

Translation is 16-bit normalised into a per-clip box:

```
t = (raw / 65535) * transScalar + transOffset
```

### 4.4 Rebuilding per-bone tracks

Keyframes are stored as a flat list, not grouped by bone. Each record points at
its predecessor by **byte** offset; the record stride is 20 bytes (8 header +
12 data), so:

```
prevIndex = currentIndex - (prevFrameByteOffset / 20)
```

The first N records, where N is the bone count, are the first keyframe of each
bone. Every later record inherits the `nodeID` of the record it points back to.
Deriving N: the reference scans for the first record whose predecessor is not
itself, which is fragile — prefer taking the bone count from the HAnim PLG and
asserting the two agree.

Result: per bone, a list of `(time, quat, translation)` sorted by time.

---

## 5. Runtime

### 5.1 Data model

```cpp
struct Bone { std::string name; int parent; glm::mat4 restLocal, invBind; };

struct AnimTrack { std::vector<float> times;
                   std::vector<glm::quat> rot;
                   std::vector<glm::vec3> pos; };

struct AnimClip { std::string name; float duration; float fps;
                  std::vector<AnimTrack> tracks; };   // one per bone

struct Skeleton { std::vector<Bone> bones;
                  std::vector<AnimClip> clips; };
```

Attach a `Skeleton` to the section that owns the clump, so instancing keeps
working: several placed objects can share one skeleton and play different clips.

### 5.2 Evaluation

Per frame, per animated instance:

1. Binary-search each track for the surrounding keyframes.
2. `slerp` rotation, `lerp` translation.
3. Compose local, then walk parents to world.
4. `skinMatrix[i] = world[i] * invBind[i]`.

### 5.3 GPU skinning

Extend `Vertex` with `u8 boneIndex[4]` and `f32 boneWeight[4]`, add two vertex
attributes, and upload `skinMatrix` as a uniform array (bone counts here are
small; a UBO is unnecessary). Vertex shader:

```glsl
mat4 skin = boneMat[idx.x] * w.x + boneMat[idx.y] * w.y
          + boneMat[idx.z] * w.z + boneMat[idx.w] * w.w;
gl_Position = mvp * skin * vec4(P, 1.0);
```

Static meshes pass an identity skin matrix, so one shader covers both and the
existing `model` uniform keeps its meaning.

---

## 6. Player UI

A panel, enabled only when the loaded container has at least one skeleton.

- **Clip list** — name from the section tag, duration, keyframe count.
- **Transport** — play / pause / stop, loop toggle.
- **Timeline** — scrubber in seconds with the current frame shown; dragging it
  evaluates without playing.
- **Speed** — 0.1x to 4x, default 1x.
- **Bone overlay** — draw the skeleton as lines, reusing `AppendMarker` and the
  existing solid-colour program. Invaluable for spotting a wrong compose order.
- **Rest pose** — a button that clears the clip and shows the bind pose.

Time advances from the frame delta, not from a fixed step, and wraps on
`duration` when looping.

---

## 7. Risks, in the order they will bite

1. ~~**Native skin weights.**~~ **Settled.** Measured on `CIGCCharacter.Alessa`:
   all 104 packets carry exactly four streams at VU addresses 0..3 and nothing
   higher, so weights are *not* in the display list. They live in the
   `Skin PLG (0x0116)`, native layout, as described in §3.1. Step 1 of §8 can be
   skipped.
2. **Matrix convention.** The reference is row-vector; glm is column-vector.
   Compose order is wrong in exactly one place and the symptom — a subtly
   deformed model — looks like bad weights.
3. **Quaternion conjugate.** Cheap to test both ways, expensive to debug later.
4. **Keyframe grouping.** The `prevFrame / 20` chain is easy to get right and
   easy to get silently wrong; assert against the HAnim bone count.
5. **Clip/model pairing.** Trust the GUID references, never the names.

---

## 8. Order of work

1. ~~Dump VIF stream layouts.~~ Done — weights are in `Skin PLG`, not the packets.
2. FrameList + HAnim + names → skeleton, rendered as a bone overlay in the rest
   pose. Verifiable on its own.
3. Skin PLG → inverse bind matrices, per-vertex indices and weights.
4. Clip `0x1103` → tracks, checked against `duration` and bone count.
5. CPU evaluation with the bone overlay animating. Still no skinning.
6. GPU skinning.
7. Player UI.

Steps 2, 4 and 5 each produce something visible on their own. Do not go past a
step whose result cannot be seen.

---

## 9. Out of scope

Delta-morph (`rwID_DMORPHANIMATION`, facial) — the reference has `rDeltaMorphPLG`
and chunk `0x0122`, but it targets vertex positions rather than bones and is a
separate feature. Note it, do not build it here.
