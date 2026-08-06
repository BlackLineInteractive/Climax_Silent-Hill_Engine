# The game executables

Static analysis of `SLES_551.47` (Silent Hill Origins, PlayStation 2, MIPS
R5900) and `main.dol` (Silent Hill: Shattered Memories, Wii, PowerPC).

This is the only route to the things that are provably *not* in the asset data —
the PS2 blend mode, the Wii TEV setup, per-class property semantics — because
those live in code, not in the containers. See [TODO.md](TODO.md) for which
open questions depend on it.

---

## 1. `SLES_551.47` — Origins, PS2

A stripped but otherwise complete ELF: no symbol table, but every **section name
survives**, which is far more than these discs usually keep.

```
ELF32 little-endian, machine 8 (MIPS), entry 0x001F62D0
one PT_LOAD: file 0x1000, vaddr 0x00100000, filesz 5 980 936, memsz 6 513 676
```

| Section | Address | Size | Notes |
|---------|---------|------|-------|
| `.text` | 0x00100000 | 2 270 260 | main code |
| `.vutext` | 0x0032A440 | 58 064 | **VU1 microcode** — the geometry pipeline |
| `.DVP.ovlytab` | — | 432 | VU overlay table |
| `.DVP.overlay.*` | — | — | 36 named VU overlays, keyed by hash |
| `.data` | 0x00338780 | 3 477 380 | |
| `.vudata` | 0x00689710 | 192 | |
| `.rodata` | 0x00689800 | 170 856 | strings, tables |
| `.bss` | 0x006B4700 | 531 724 | |

Two build paths survive, and they pin the middleware exactly:

```
c:/silenthillps2/code/game/libs/RW/Graphics/src/RpPDS/sky2/G3_2DFill/G3_2DFill_Node.c
c:/silenthillps2/code/game/libs/RW/Graphics/src/RpPDS/sky2/G3_2DStroke/G3_2DStroke_Node.c
```

`RpPDS/sky2` is RenderWare's PS2 platform-dependent pipeline, which is what
produces the VIF packets documented in [SH_FORMAT.md](SH_FORMAT.md) §4.

### 1.1 Section tags

The complete set of `rw*ID_*` tags the engine knows, which is a superset of what
the retail containers actually use:

```
rwID_2DMAESTRO        rwID_AINAVMESH        rwID_ASSETGROUPTAG   rwID_ATOMIC
rwID_AUDIOCUES        rwID_CBSP             rwID_CLUMP           rwID_COMBATCOLLISION
rwID_DMORPHANIMATION  rwID_HANIMANIMATION   rwID_KFONT           rwID_POLYAREA
rwID_RWS              rwID_SOUNDBANK        rwID_SPLINE          rwID_STATETRANSITION
rwID_TEXDICTIONARY    rwID_WORLD            rwaID_WAVEDICT       rwpID_BODYDEF
rwpID_GENERICDEF      rwpID_RAGDOLLDEF
```

Never seen in a PS2 container but present in the engine: `rwID_2DMAESTRO`,
`rwID_ASSETGROUPTAG`, `rwID_AUDIOCUES`, `rwID_COMBATCOLLISION`,
`rwID_POLYAREA`, `rwID_SOUNDBANK`, `rwID_STATETRANSITION`, `rwpID_GENERICDEF`,
`rwpID_RAGDOLLDEF`.

### 1.2 The class registry

Every game-object class registers itself at start-up through one function at
`0x001FE298`, called with four arguments:

```
Register(registryEntry, className, factory, objectSize)
```

Worked example, the `CColorLight` registration at 0x00140574:

```
lui   $s0, 0x69
addiu $s0, $s0, 0x10a8      ; -> "CColorLight"
jal   0x1fca98              ; look the name up / hash it
move  $a0, $s0
lui   $v1, 0x6b
lui   $a0, 0x6b
lui   $a2, 0x14
sw    $v0, 0x4d40($v1)      ; store the returned id
addiu $a0, $a0, 0x4d48      ; a0 = registry entry  0x006B4D48
move  $a1, $s0              ; a1 = "CColorLight"
addiu $a2, $a2, 0x5e8       ; a2 = factory         0x001405E8
jal   0x1fe298
addiu $a3, $zero, 0x6c      ; a3 = object size     108 bytes
```

Note that MIPS never stores an address as a word — it builds it from a
`lui`/`addiu` pair — so searching for a 32-bit pointer to a string finds
nothing. Every one of these names is referenced exactly once, from its own
registration.

Walking all 126 calls to `0x001FE298` and reconstructing the arguments recovers
the whole table. It is written to
[`sho_class_registry.json`](sho_class_registry.json): class name, registry entry
address, factory address and object size for all 126 classes.

A few entries, for the flavour of it:

| Class | Factory | Size |
|-------|---------|------|
| `CColorLight` | 0x001405E8 | 108 |
| `CStaticCamera` | 0x0011D7C0 | 352 |
| `CPhysicsObject` | 0x0013D490 | 416 |
| `CFogConfig` | 0x00160770 | 184 |
| `CPlayerBehaviour` | 0x00136CC8 | 7152 |
| `CEnemyBehaviour` | 0x0012B630 | 11664 |
| `CIGCCharacter` | 0x00184650 | 5344 |

126 classes are registered against the 115 name strings visible in `.rodata`,
and the list includes plenty the containers never instance — `AreaTriggerBox`,
`AudioReverb`, `SavePoint`, `cBlackOutFSE`, `CFullScreenRain`.

**Why this matters.** Each factory address is that class's constructor, and the
constructor is where the class binds its serialised properties. That is the
direct route to the per-class property semantics `SH_FORMAT.md` §3 currently
describes empirically — including the volume classes whose property 1 is a
collision extent rather than a placement matrix.

### 1.3 The container chunk vocabulary

A second registration table, at `0x00200238`, pairs every container chunk type
with the function that reads it:

```
Register(chunkType, handler)
```

All 16 registrations, cross-checked against 60 retail containers:

| Chunk | Handler | Occurrences in retail data |
|-------|---------|---------------------------|
| 0x0700 | 0x001D8190 | 0 |
| **0x0704** | 0x001FC208 | **998** — placed game object |
| 0x0705 | 0x001FC170 | 0 |
| 0x0706 | 0x001D7648 | 0 |
| 0x0707 | 0x001D7658 | 0 |
| 0x070B | 0x001D80D0 | 0 |
| 0x070C | 0x001D8130 | 0 |
| 0x070D | 0x001FC0C0 | 0 |
| 0x070E | 0x001FC150 | 0 |
| 0x070F | 0x00201AF8 | 0 |
| 0x0712 | 0x001FF910 | 0 |
| 0x0715 | 0x00201B78 | 0 |
| **0x0716** | 0x00201C60 | **334** — named section |
| 0x071A | 0x001FC280 | 0 |
| 0x071B | 0x001FC208 | 0 — same handler as 0x0704, so the same layout |
| 0x071D | 0x001D8278 | 0 |

`0x071C`, the type directory, appears 57 times in the data but has **no entry in
this table**: it is read by the container's own header code rather than through
the generic chunk dispatcher.

This closes a question that could only be answered from the executable: the
container vocabulary is 17 chunk types, the shipped data uses exactly three of
them, and the toolkit already handles all three. The other fourteen are engine
capability that the game never exercised.

### 1.5 Parsing the container the way the engine does

Three functions define the whole container format, and each was read rather
than inferred. Ghost Rider ships unstripped, so they carry their real names;
Silent Hill Origins has the same code at different addresses.

**`RWS::CStreamHandler::ProcessStream`** — the outer walk.

```c
ProcessStream(RwStream* s) {
  goto check;
loop:
  RwStreamReadChunkHeaderInfo(s, &hdr);
  entry = find(handlers, hdr.type);
  if (entry == end) RwStreamSkip(s, hdr.length);   // unknown chunk
  else              entry->handler(&hdr, s);        // known chunk
check:
  if (!IsEOF(s)) goto loop;
  CloseStream(s);
}
```

Two things matter. The loop is driven purely by end-of-stream, with no byte
budget — this toolkit needs one because it runs the same walk recursively over
sub-ranges instead of real sub-streams. And **the engine never repositions the
stream after a handler**: every handler must consume exactly its chunk. That
contract is now enforced by a tripwire in `CResourceHandler::ProcessStream`,
which reports any handler that reads the wrong number of bytes. On the retail
archive it fires zero times.

**`RWS::LoadEmbeddedAsset`** — the `0x0716` section handler.

```c
u32 headerSize;  RwStreamRead(s, &headerSize, 4);
void* buf = Alloc(headerSize);
RwStreamRead(s, buf, headerSize);          // the header in one block
u32 payloadSize; RwStreamRead(s, &payloadSize, 4);

u32   tagLen  = *(u32*)buf;
char* tag     = buf + 4;
u8*   guid    = tag + tagLen;              // 16 bytes
u32   nameLen = *(u32*)(guid + 0x10);
char* name    = guid + 0x14;
u32   pathLen = *(u32*)(name + nameLen);
LoadResource(tag, guid, name, ..., s, payloadSize);
```

Every read is a plain little-endian `lw`; there is no byte swapping anywhere,
which settles the byte order of the PlayStation 2 container for good. The engine
also validates nothing — it reads a length, allocates, and walks with pointer
arithmetic. Our own guards are therefore ours alone, and they must only ever
reject corrupt data: a guard that rejected a valid `tagLen` is what silently
disabled all geometry once.

**`RWS::CAttributePacket::CreateEntity`** — the `0x0704` record walk.

```c
for (;;) {
    if (*(u32*)(rec + 4) == 0x40000000)    // record kind is the top byte of id
        copy16(rec + 8);                    // the GUID payload
    rec += *(u32*)(rec + 0);                // advance by the record's own size
}
```

Confirms `[u32 size][u32 id][payload]` to the byte, and that `0x40` is the GUID
kind. See §1.2 and `tools/attrmap.py` for what the remaining kinds carry.

Two further points fall out of reading the code, and both say the toolkit's
current shape is already right.

**`0x071C`, the type directory, is dead weight at runtime.** It has no entry in
the handler table, and the constant appears nowhere in the container code — the
three matches in the binary are ordinary numbers inside `CHellBike` and an
unrelated `HandleAttributes`. The engine therefore meets it as an unknown chunk
and skips it. It is build-tool metadata. Keeping it as a cross-check is
worthwhile (254 of 255 retail containers hold exactly the object count it
declares), but nothing may depend on it, and if it ever disagrees with the data
the engine's own behaviour is to ignore it.

**The section tag does not select a reader.** `rwID_WORLD`,
`rwID_TEXDICTIONARY` and `rwID_CBSP` have zero code references; only
`rwID_CLUMP` is mentioned at all, and not from the loader. Routing happens one
level down, on the payload's own RenderWare chunk type -- `0x000B` World,
`0x0010` Clump, `0x0016` TexDictionary, `0x1100` CBSP, `0x0809` WaveDict. The
tag and GUID exist for the resource manager to find things by name later.

### 1.6 Disassembling it

`capstone` reads this with `CS_MODE_MIPS64 | CS_MODE_LITTLE_ENDIAN`. Plain
`CS_MODE_MIPS32` decodes the R5900 as MIPS SIMD and produces nonsense
(`aver_u.h`, `addu.qb` where the real instruction is a 128-bit load or store).
Even in MIPS64 mode the R5900's own 128-bit instructions come out wrong, but
control flow, loads, stores and immediates are all correct, which is enough to
follow a function.

---

## 2. `main.dol` — Shattered Memories, Wii

A standard DOL: no sections names, just load segments.

```
entry 0x80006310, bss 0x80457480 size 887 860

text0  file 0x000100 -> 0x80004000      9 952
text1  file 0x0027E0 -> 0x800067A0  4 004 800
data0  file 0x3D43A0 -> 0x800066E0         96
data1  file 0x3D4400 -> 0x80006740         96
data2  file 0x3D4460 -> 0x803D8360      1 024
data3  file 0x3D4860 -> 0x803D8760         32
data4  file 0x3D4880 -> 0x803D8780     26 368
data5  file 0x3DAF80 -> 0x803DEE80    492 992
data6  file 0x453540 -> 0x80526720     10 720
data7  file 0x455F20 -> 0x8052B5E0     19 104
```

**173 class names**, against the PS2 game's 126 — the sequel grew a lot. New
families that have no Origins counterpart: `CAIWorldBehaviour`,
`CAIFrozenLakeEnemyRespot`, `CAnimSkeleton`, `CAmbientVoicemail`,
`CAmbientTextMessage`, `CAmbientInterferencePoint`, `CButtonPuzzle`,
`CButtonMashListener`, `CBasicMount`, `CAnimatedPOVCamera`.

The binary also embeds copies of both archive directories, and a third complete
archive of boot resources — see [SHSM_ARC_FORMAT.md](SHSM_ARC_FORMAT.md) §2.4,
where those are used to prove that resource names exist nowhere on the disc.

### 2.1 What is not done here yet

The equivalent registry walk. PowerPC builds an address from an `addis`/`ori`
or `addis`/`addi` pair, the same shape as the MIPS `lui`/`addiu`, so the same
technique applies — but the scan has to cover the 4 MB `text1` segment, and the
naive Python search over it is too slow to be worth running as written. Indexing
`addis` instructions by their immediate first would make it quick.

---

## 3. Next targets

In rough order of value:

1. **Read a few PS2 factories** from `sho_class_registry.json` and work out how
   properties are bound. This settles the per-class property semantics for both
   games, since the container format is shared. The `0x0704` handler at
   `0x001FC208` is the other end of the same thread.
2. **The PS2 blend mode.** It is set through the GS `ALPHA` register in a GIF
   packet; finding the code that fills it per material would replace the
   hand-maintained additive list in the renderer.
3. **The Wii TEV setup** for ice and water. A Dolphin FIFO capture is the easier
   route to the same answer and is worth trying first.
4. **The archive key.** `SHSM_ARC_FORMAT.md` §2.4 rules out every hash of every
   recoverable name. Whatever the resource-request path in `main.dol` passes to
   the archive would settle it.
5. **`.vutext`** — 58 KB of VU1 microcode, the actual geometry pipeline. Only
   worth reading if the VIF interpretation ever turns out to be wrong, which so
   far it has not.
