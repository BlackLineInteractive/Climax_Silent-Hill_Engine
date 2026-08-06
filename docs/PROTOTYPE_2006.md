# Silent Hill Origins: 2006 Prototype Archival Format (GR.ARC)

The August 7, 2006 PSP prototype uses an early Climax Engine data architecture directly inherited from *Ghost Rider*. The `GR.ARC` format used here is an early iteration of what would eventually become the `A2.0` and `0x0000FA10` (Shattered Memories) formats.

## 1. The Global Archive Header
The `GR.ARC` starts with a 16-byte header defining the TOC layout. Unlike the final `A2.0` format, this header has no string magic bytes.

```c
struct PrototypeArcHeader {
    u32 fileCount;       // Number of entries in the TOC. e.g. 61
    u32 tocSize;         // Byte offset to the end of the TOC (includes this header). e.g. 992
    u32 archiveSize;     // Total size of the GR.ARC file in bytes.
    u32 totalNodes;      // e.g. 1488. Likely the total number of chunks/objects across all files.
};
```

## 2. Table of Contents (TOC)
Immediately following the header at offset `0x10` is an array of `fileCount` entries.
Each entry is 16 bytes.

```c
struct PrototypeArcEntry {
    u32 nodeIndex;       // A monotonically increasing index linking this archive bundle to the metadata block.
    u32 offset;          // Absolute byte offset within GR.ARC where the file begins.
    u32 size;            // Size of the extracted file bundle in bytes.
    u32 padding;         // Always 0.
};
```

**Crucial Differences from Final Game:**
1. The TOC entries do **not** have string names or hashes. The engine maps these bundles strictly by their `nodeIndex`.
2. The TOC does not end with a string table. It ends exactly at `tocSize`.

## 3. The Metadata Block (File 0)
The very first entry in the TOC (`nodeIndex = 0`) defines a massive 2 MB "Metadata Block".
This block contains the entire Scene Graph and Object Definitions for the prototype!

```c
// The metadata block heavily utilizes the Ghost Rider 0x071C container.
u32 magic; // 0x0000071C
u32 size;  
u32 version; 
u32 numEntries;

for (u32 i = 0; i < numEntries; ++i) {
    char name[];      // Null-terminated string (e.g. "CZone", "CStaticCamera")
    align(4);
    u32 count;        // Number of instances of this class
}
```

This block embeds raw string paths corresponding to the original Climax LA environment builds:
* `z:\SilentHill\Design\Work\StudioProjects\Zones\1_Asylum\1st_Floor\Asylum_1flr_z1\Build Output\Playstation Portable\Target Resources\{4B68B47F-19A8-4BFD-991D-90932351E742}.bsp`
* `FrontDesk`, `OldComputer`, `chairDesk02_mp`

## 4. Asset Bundles (Files 1..N)
The subsequent files (1 through 60) are the actual levels and assets.
Unlike the final game, where `.arc` files are cleanly separated, the prototype appears to encrypt or compress the downstream asset bundles using a proprietary scheme (no standard Zlib `78 DA` headers are visible).

However, the internal payloads extracted via `attrmap.py` demonstrate that the raw geometry chunks inside these bundles rely on a twisted RenderWare header structure, frequently storing chunks as `[Size][Version][Type]` rather than `[Type][Size][Version]`.

## 5. Game Executable (`EBOOT.BIN`)
The game engine logic is contained entirely within `/PSP_GAME/SYSDIR/EBOOT.BIN`. As is standard for PSP games, this file is an encrypted PRX/ELF. 
If decrypted (via PPSSPP or a PRX decrypter tool), it can be analyzed in Ghidra. Due to the early stage of development, this executable still carries the core `Ghost Rider` logic, property parsing maps (which `attrmap.py` decodes), and the `0x071C` Climax container dispatcher.

## 6. Audio Files (`DATA/MUSIC/*.RWS`)
Unlike the final game which uses custom Climax Audio Banks, the prototype relies directly on **RenderWare Audio Streams (`.RWS`)**.
- Example files: `MUSTITLE.RWS`, `MUSCOMBA.RWS`, `MUSFREEZ.RWS`.
- This confirms that audio middleware was entirely RenderWare-based at this stage before Climax transitioned to their bespoke sound engine for the final SHO release.

## 7. PlayStation Portable Modules (`.prx`)
The `USRDIR/module` and `USRDIR/kmodule` folders contain standard Sony SCE utility modules. These are not unique to Silent Hill but were used by the Ghost Rider engine to handle basic PSP hardware interfacing.
- **Audio/Video:** `audiocodec.prx`, `videocodec.prx`, `mpeg.prx`, `libpsmfplayer.prx` (suggests video playback was implemented).
- **Crypto/Hash:** `libmd5.prx`, `libsha1.prx`, `libbase64.prx`
- **Other:** `libfont.prx` (PSP system font library), `libatrac3plus.prx` (Sony's proprietary audio codec, likely decoding the `.RWS` audio streams).
