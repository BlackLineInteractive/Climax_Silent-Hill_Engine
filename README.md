# Silent Hill Origins — 3D Level Viewer

[![build](https://github.com/BlackLineInteractive/SilentHillOrigins-LevelViewer/actions/workflows/build.yml/badge.svg)](https://github.com/BlackLineInteractive/SilentHillOrigins-LevelViewer/actions/workflows/build.yml)

A real-time 3D viewer for game levels and locations from **Silent Hill Origins** (PS2 / PSP),
with potential compatibility for **Silent Hill: Shattered Memories**.

Opens proprietary SHO container files (no file extension — named like `MO_1_Room102`), decodes native PS2 textures, and renders the
full level geometry, baked lighting, collision mesh, and placed objects interactively.

> License: [CC BY 4.0](LICENSE) — free to use and modify, **attribution required**.

---

## Screenshots

![Screenshot 1](screenshots/screenshot1.png)
![Screenshot 2](screenshots/screenshot2.png)

---

## Features

- Parse and render SHO container files with embedded geometry, materials, and PS2 textures
- Decode native PS2 PSMT4 / PSMT8 / PSMCT32 texture formats via software unswizzle
- Seven render modes: Textured, Vertex Color, Flat Shaded, Normals, Depth, Checkerboard, Unlit
- Collision mesh overlay with optional semi-transparent fill
- Clump object markers (octahedron wireframe + projected labels)
- ImGuizmo translate gizmo for interactive pivot repositioning
- Orbit sphere gizmo for camera rotation with SDL cursor capture
- Structured hierarchy browser with section tree and material inspection
- Texture atlas browser with per-texture metadata
- File browser for loading levels at runtime

---

## Format Notes

The SHO container is a chunked binary format used by PS2 titles.
A top-level 0x071C directory block wraps named 0x0716 section records.
Each section may contain:

| Block type | Description                    |
|------------|--------------------------------|
| CLUMP      | RenderWare-style geometry tree |
| CBSP       | Collision BSP mesh             |
| TEXDICTION | Texture dictionary (TXD)       |
| BINMESH    | Pre-indexed triangle lists     |

The parser lives in [src/Loader.cpp](src/Loader.cpp); the PS2 texture decoder is in
[src/PS2Texture.cpp](src/PS2Texture.cpp).

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
./build/SHOViewer path/to/MO_1_Room102 [texture.txd ...]
```

### Makefile (local vendor/)

```bash
make -j$(nproc)
./SHOViewer path/to/MO_1_Room102 [texture.txd ...]
```

---

## Usage

```
SHOViewer <container_file> [txd1 txd2 ...]
```

Container files have **no extension** — they are named like `MO_1_Room102`, `MO_1_Courtyard`, etc.
Texture files end in `.txd` (no extension on the texture dictionary is also possible).

Textures can be embedded inside the container or supplied as separate TXD archives.
The file browser inside the application can also open files at runtime.

---

## Controls

| Action                  | Input                                    |
|-------------------------|------------------------------------------|
| Orbit camera            | Right-click drag / orbit sphere (top-right) |
| Zoom                    | Scroll wheel (proportional to distance)  |
| Move pivot              | ImGuizmo arrows / plane handles — follows the cursor 1:1 |
| Snap pivot while moving | Hold **Ctrl** (or tick **Snap**, step is configurable) |
| Toggle pivot gizmo      | **Pivot gizmo** checkbox                 |
| Reset camera            | Key **1** or **Reset Camera** button     |
| Open file               | Open Level button                        |

The gizmo, the orbit sphere and the camera never fight over the mouse: a hovered
gizmo no longer blocks wheel-zoom or right-drag orbit, and only one of them can
claim a left-button drag at a time.

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
  Common.h/cpp    — shared state, types, globals
vendor/            (Makefile build only — CMake fetches these into build/_deps)
  imgui/
  imguizmo/
```

---

## License

[CC BY 4.0](LICENSE) — Copyright (c) 2026 Blackline Interactive.
You are free to use, share, and adapt this code for any purpose **as long as you credit the author**.
