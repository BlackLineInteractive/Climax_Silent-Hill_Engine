# Climax Silent Hill Engine Toolkit

[![build](https://github.com/BlackLineInteractive/SilentHillOrigins-LevelViewer/actions/workflows/build.yml/badge.svg)](https://github.com/BlackLineInteractive/SilentHillOrigins-LevelViewer/actions/workflows/build.yml)

<a href="https://youtube.com/@blacklineinteractive">
  <img src="https://img.shields.io/badge/YouTube-Blackline_Interactive-FF0000?style=flat-square&logo=youtube&logoColor=white" alt="Blackline Interactive YouTube"/>
</a>

---

**Climax Silent Hill Engine Toolkit** — a real-time 3D level viewer, asset decoder, and archive extraction toolkit for game levels and locations built on Climax Engine: **Silent Hill Origins** (PS2 / PSP), **Silent Hill: Shattered Memories** (Wii / PS2 / PSP), and **Ghost Rider** (PS2 / PSP).

Opens proprietary Climax Engine container files (no file extension — named like `MO_1_Room102`), decodes native PS2 and Wii GPU textures, and renders full level geometry, baked lighting, collision meshes, and placed game objects interactively. Decodes the game audio as well: the per-level sound banks, the music streams and the cutscene dialogue. Includes full archive unpackers for `SH.ARC`, `data.arc`, and `igc.arc`.

> License: [GPL-3.0](LICENSE) — free to use, study, change and share; derived work stays under the same license.

---

## Screenshots

![Dhalia House](media/s2-v0.2.png)
![Pool Area](media/poolarea-s0-v0.2.png)
![Intro Road 2](media/s1-v0.2.png)
![Motel](media/MO_1_Courtyard.png)
![Butcher's Great Sword](media/ButchersGreatSword.png)
![Audio Test Room](media/audio-test-room.png)

![Ghost Rider](media/GhostRider.png)

![SHSM Wii](media/shsm-wii-0.png)
![SHSM Wii 1](media/shsm-wii-1.png)
![SHSM Wii 2](media/shsm-wii-2.png)
![SHSM Wii 3](media/shsm-wii-3.png)
![SHSM Wii 4](media/shsm-wii-4.png)
![SHSM Wii 5](media/shsm-wii-5.png)
![SHSM Wii 6](media/shsm-wii-6.png)
![SHSM Wii 7](media/shsm-wii-7.png)
![SHSM Wii 8](media/shsm-wii-8.png)
![SHSM Wii 9](media/shsm-wii-9.png)
![SHSM Wii 10](media/shsm-wii-10.png)

---

## Features

- **Mount `SH.ARC` directly** — browse and load levels by their real internal names
  (`MO_1_Room102`, `TH_E_Lobby`, …), with matching texture dictionaries pulled in automatically
- **Placed game objects** — reads the `0x0704` instance chunks, so spawners, cameras,
  pickups, lights and triggers appear at their real world coordinates instead of the origin
- **Correct model placement** — models stored in `rwID_RWS` / `rwID_CLUMP` sections are
  instanced wherever the game objects that reference them say, instead of piling up at 0,0,0
- **Level cameras** — jump to any of the fixed cameras the game cuts between
- **glTF 2.0 export** — one mesh per texture name, with embedded textures and level lights
- **Shattered Memories (Wii)** — mounts `data.arc` / `igc.arc`, recurses into their
  nested archives, labels entries from their contents (the archive stores no names),
  and renders them: GX display-list geometry for levels, props and characters, plus
  the GameCube/Wii textures — CMPR, RGBA8, RGB5A3, I4/I8/IA4/IA8, RGB565
- **Playback panel** — sound, skeletal animation and UV animation in one window:
  the level's own sound bank (footsteps, doors, room tone), the 75 music streams from
  `MUSIC/` and the 35 cutscene tracks from `IGC.ARC`, with seeking, looping and WAV export
- **Material blend modes from the asset** — the `0x0A01` material extension carries the
  mode the engine reads through `ClimaxT1MaterialGetFrameBlendMode`: standard alpha,
  additive, subtractive, and `NONE`, which writes the surface straight out and ignores
  alpha entirely. That last one is what character skin relies on
- **Animated fire** — the `0x2B` sections hold RenderWare UV animations, and the flame
  materials name one. Two tiled sheets scroll past each other at different rates; without
  it a flame is a still rectangle
- Parse and render SHO container files with embedded geometry, materials, and PS2 textures
- Decode native PS2 PSMT4 / PSMT8 / PSMCT32 texture formats via software unswizzle
- Eight render modes: Textured, Vertex Color, Flat Shaded, Normals, Depth, Checkerboard, Unlit
- Skeletal animation clips decoded — 3029 across the archive, with the bone table read from
  the HAnim plugin; evaluation and skinning are not wired up yet
- Collision mesh overlay with optional semi-transparent fill
- Clump object markers (octahedron wireframe + projected labels)
- ImGuizmo translate gizmo for interactive pivot repositioning
- Orbit sphere gizmo for camera rotation with SDL cursor capture
- Structured hierarchy browser with section tree and material inspection
- Texture atlas browser with per-texture metadata
- File browser for loading levels at runtime

---

## Format Notes

### `SH.ARC` / `IGC.ARC` — the game archive

An `"A2.0"` archive: a 20-byte header, a 16-byte record per file, and a name table
that occupies the tail of the file. Payloads are raw zlib streams, except where
`uncompressedSize` is zero — those are stored uncompressed, which is how all 35
cutscene entries of `IGC.ARC` are written.

```
Header (20 bytes)
  0x00  char magic[4]        "A2.0"
  0x04  u32  entryCount
  0x08  u32  firstDataOffset
  0x0C  u32  nameTableOffset  (== end of the payloads)
  0x10  u32  nameTableSize    (nameTableOffset + nameTableSize == file size)

Entry (16 bytes, entryCount of them, from 0x14)
  0x00  u32  nameOffset       byte offset into the name table
  0x04  u32  offset           absolute, 0x20-aligned
  0x08  u32  compressedSize   zlib stream length
  0x0C  u32  uncompressedSize
```

Verified against the retail PS2 `SH.ARC`: all 1487 entries inflate to exactly the
declared size, offsets are monotonic and aligned, and the name table ends precisely
at EOF. Reader: [src/Arc.cpp](src/Arc.cpp).

### The container

A chunked binary format. A top-level `0x071C` directory block lists every game-object
type and how many instances exist, followed by a flat chunk list:

| Chunk    | Description                                             |
|----------|---------------------------------------------------------|
| `0x0716` | Named resource section (WORLD, CBSP, WAVEDICT, AINAVMESH…) |
| `0x0704` | A placed game-object instance                           |

A `0x0716` section header is
`[count][tagLen][tag][guid(16)][nameLen][name]` followed by two build-path strings.

A `0x0704` body is a flat list of tagged records — `[u32 size][u32 id][payload]` —
where the top byte of `id` selects the kind (`0x20` class name, `0x40` GUID,
`0x80` instance name, `0x00` indexed property). **Property 1 is a 64-byte
column-major 4×4 world matrix** — the object's placement. Names are 0xBF-padded.
Property indices restart at 0 for each component of an object, so a group boundary
is simply "the index stopped increasing".

A 16-byte property is the GUID of a `0x0716` section the object owns. That is how
geometry gets placed: `rwID_WORLD` sections are already in level space, but
`rwID_RWS` and `rwID_CLUMP` sections are re-usable models, and the object holding
the reference supplies the transform. The same model is often referenced several
times — in `IntroRoad` one RWS model is instanced at four different spots.

`CColorLight` carries an RGBA colour in group 0 property 0, and
`[type][cone angle°][range][enabled]` in group 1.

Cross-checked over the whole retail archive: 254 of 255 containers parse to exactly
the object count their own type directory declares (11 834 objects, 7 065 of them
placed). The single outlier declares a `CWiiStndController` that the PS2 build does
not ship.

Sections that may appear inside a `0x0716`:

| Block type | Description                    |
|------------|--------------------------------|
| CLUMP      | RenderWare-style geometry tree |
| CBSP       | Collision BSP mesh             |
| TEXDICTION | Texture dictionary (TXD)       |
| BINMESH    | Pre-indexed triangle lists     |
| WAVEDICT   | The level's sound bank         |

### Audio

Four containers, two codecs. `rwaID_WAVEDICT` sections hold the level's own sound
bank — 2980 named samples across the 255 retail containers, all mono Sony 4-bit
ADPCM at 6–32 kHz. `MUSIC/*.RWS` are RenderWare Audio streams, also mono ADPCM,
with the rate and channel count in the 0x080E header and the data at a fixed
offset of 2048. `IGC.ARC/*.IGCStream` multiplexes 1024-byte pieces of a 48 kHz
16-bit stereo ADS into a record stream, tagged `0xA000`; concatenating them
reassembles the ADS exactly. Decoder: [src/Core/AudioParser.cpp](src/Core/AudioParser.cpp).

See **[docs/ANIMATION_SPEC.md](docs/ANIMATION_SPEC.md)** for the animation
plan: skeleton, skinning, compressed keyframes and the playback UI.

See **[docs/EXECUTABLES.md](docs/EXECUTABLES.md)** for what the game binaries
themselves give up: section layout, the engine's full tag list, and the
recovered class registry.

See **[docs/TODO.md](docs/TODO.md)** for the current work queue: what is broken,
what is unimplemented, and what is already known about each.

See **[docs/SHSM_ARC_FORMAT.md](docs/SHSM_ARC_FORMAT.md)** for the Wii *Shattered
Memories* archives and containers: the `0x0000FA10` archive, the big-endian
section and game-object records, and the GameCube/Wii texture formats.

See **[docs/SH_FORMAT.md](docs/SH_FORMAT.md)** for the full format reference: archive
layout, container chunks, the game-object record encoding, the PS2 display-list
geometry, collision and texture formats, with the verification figures behind
each claim.

The container parser lives in [src/Loader.cpp](src/Loader.cpp); the PS2 texture
decoder is in [src/PS2Texture.cpp](src/PS2Texture.cpp).

---

## Build

Builds on **Linux, macOS and Windows**; every push is built on all three by
[GitHub Actions](.github/workflows/build.yml), which also uploads a ready binary
per platform as a workflow artifact.

### Requirements

| Dependency | Notes                                    |
|------------|------------------------------------------|
| CMake 3.16 | 3.21+ on Windows to auto-copy the DLLs   |
| GCC / Clang / MSVC (C++17) |                          |
| OpenGL 3.3 | core profile                             |
| GLEW       | system package                           |
| SDL2       | system package                           |
| GLM        | header-only; fetched automatically if missing |

ImGui and ImGuizmo are always downloaded during the CMake configure step.

### Install the system dependencies

```bash
# Debian / Ubuntu
sudo apt install build-essential cmake pkg-config libsdl2-dev libglew-dev libglm-dev
```

```bash
# macOS (Homebrew)
brew install cmake sdl2 glew glm
```

```bash
# Windows (vcpkg — reads the dependency list from vcpkg.json)
vcpkg install --triplet x64-windows
```

### Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
```

On Windows add the vcpkg toolchain to the configure step:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake
```

Then run it — the TXD list is optional, textures may be embedded in the container:

```bash
./build/ClimaxGameEngineToolkit path/to/MO_1_Room102 [texture.txd ...]
```

### Makefile (local vendor/)

```bash
make -j$(nproc)
./ClimaxGameEngineToolkit path/to/MO_1_Room102 [texture.txd ...]
```

---

## Usage

Preferred — point it at the game archive and pick levels by name:

```
ClimaxGameEngineToolkit path/to/SH.ARC [LevelName]
```

Without a level name the viewer just mounts the archive; use the **Archive** panel
to browse the 269 level containers by name. Selecting one loads it together with
every `.txd` the archive names after it (the game names shared dictionaries after
the rooms they bridge, e.g. `MO_1_Room102-MO_1_PoolArea.txd`).

Loose, already-extracted files still work:

```
ClimaxGameEngineToolkit path/to/MO_1_Room102 [txd1 txd2 ...]
```

Export a level to glTF without opening the window:

```
ClimaxGameEngineToolkit path/to/SH.ARC MO_1_Room102 --export room.glb
```

Container files have **no extension** — `MO_1_Room102`, `MO_1_Courtyard`, etc.
Textures can be embedded in the container or supplied as separate TXD archives.

---

## Controls

Press **F2** in the application for the full manual.

| Action                  | Input                                    |
|-------------------------|------------------------------------------|
| Orbit camera            | Right-click drag / orbit sphere (top-right) |
| Zoom                    | Scroll wheel (proportional to distance)  |
| Move pivot              | Gizmo arrows / plane handles — follows the cursor 1:1 |
| Snap pivot while moving | Hold **Ctrl** (or tick **Snap**)         |
| Jump to a level camera  | **Jump to camera** dropdown              |
| Reset camera            | **1**                                    |
| Hide / show the UI      | **F1**                                   |
| Manual                  | **F2**                                   |
| Hide / show the gizmo   | **G**                                    |
| Play a sound            | Click it in the **Audio** panel          |
| Show / hide the Audio panel | **Panels → Audio**                   |

The gizmo, the orbit sphere and the camera never fight over the mouse: a hovered
gizmo does not block wheel-zoom or right-drag orbit, only one of them can claim a
left-button drag at a time, and the gizmo only appears once a level is loaded.

---

## Render Modes

| Mode         | Description                                   |
|--------------|-----------------------------------------------|
| Textured     | Albedo texture multiplied by vertex color     |
| Vert. Color  | Vertex color only, no texture                 |
| Flat         | Per-face normal shading via dFdx/dFdy         |
| Normals      | Face normals visualised as RGB                |
| Depth        | Linear depth in greyscale                     |
| Checker      | UV checkerboard (8x8 tiles)                   |
| Unlit        | Texture sampled without any color modulation  |

---

## Project Structure

```
src/
  main.cpp        — render loop, shaders, input
  Loader.cpp/h    — SHO/TXD parser, geometry upload
  UI.cpp/h        — structure tree, texture browser, file browser
  PS2Texture.cpp/h— PS2 VRAM format decoder
  Arc.cpp/h       — SH.ARC ("A2.0") archive reader
  AudioParser.cpp/h— WAVEDICT / RWS / ADS / IGC audio decoders
  WiiTexture.cpp/h— GameCube/Wii GX texture decoder
  WiiGeometry.cpp/h— GameCube/Wii display-list geometry decoder
  Export.cpp/h    — glTF 2.0 / GLB writer (groups geometry by texture)
  Common.h/cpp    — shared state, types, globals
vendor/            (Makefile build only — CMake fetches these into build/_deps)
  imgui/
  imguizmo/
```

---

## License

[GNU General Public License v3.0](LICENSE) — Copyright (c) 2025-2026 Blackline Interactive.

You may use, study, change and share this program. If you distribute it, or
anything derived from it, that work must carry the same license and its source
must be available.
