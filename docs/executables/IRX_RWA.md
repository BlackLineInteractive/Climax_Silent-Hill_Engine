# RWA.IRX (RenderWare Audio)

## Overview
`RWA.IRX` is the RenderWare Audio driver module for the PlayStation 2 IOP. Although Silent Hill Origins uses the proprietary Climax Engine for graphics and logic, it licenses and utilizes RenderWare Audio (RWA) for its sound subsystem. This module handles everything from audio banking to real-time 3D sound positioning and streaming.

## Module Information
- **Filename:** `RWA.IRX`
- **Internal Module Name:** `renderware_audio_spu2`
- **System:** PS2 I/O Processor (MIPS R3000)
- **Role:** High-level audio engine and middleware.

## Architecture & Integration
RWA sits atop standard Sony drivers (`LIBSD.IRX` and `SDRDRV.IRX`) and coordinates with `CDVDSTM.IRX` to stream large music files. 

### Core Mechanics
1. **Audio Banks:** It parses RenderWare's proprietary sound banks (which your Toolkit handles in `AudioParser.cpp`) and uploads required ADPCM/VAG samples into the SPU2's 2MB Sound RAM.
2. **3D Positional Audio:** It calculates volume, panning, and pitch dynamically based on the listener's (camera's) position in the 3D world, translating these into raw SPU2 register writes via `LIBSD`.
3. **Streaming Coordination:** For BGM and cutscene dialogue, RWA requests data chunks from `CDVDSTM.IRX`, decodes them on the fly, and feeds them to the SPU2 playback buffers.

## Reverse Engineering Focus
For the Climax Game Engine Toolkit, reversing RWA provides crucial insights into:
- How ADPCM audio streams are interleaved and parsed.
- How audio banks are structured in memory.
- The RPC interface used to trigger specific sound effects by ID.
