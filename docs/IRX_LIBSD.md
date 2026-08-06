# LIBSD.IRX (Sound Device Library)

## Overview
`LIBSD.IRX` is the standard Sony low-level sound library for the PlayStation 2. It serves as the foundational abstraction layer for communicating with the SPU2 (Sound Processing Unit 2) hardware. Almost every PS2 game relies on this module (or a slightly modified version of it) to generate audio.

## Module Information
- **Filename:** `LIBSD.IRX`
- **Internal Module Name:** `Sound_Device_Library` / `libsd`
- **System:** PS2 I/O Processor (MIPS R3000)
- **Role:** Low-level hardware abstraction for SPU2.

## Architecture & Integration
`LIBSD` does not handle high-level concepts like "music tracks" or "3D positional audio." Instead, it provides a raw API for manipulating SPU2 hardware registers.

### Core Mechanics
1. **Memory Management:** It handles the transfer of sound samples (ADPCM/VAG) from the IOP's main RAM into the 2MB Sound RAM dedicated to the SPU2 via DMA channels.
2. **Voice Control:** The SPU2 has 48 hardware voices. `LIBSD` provides functions to assign samples to these voices, set their pitch (sample rate), ADSR envelope (Attack, Decay, Sustain, Release), and volume.
3. **Core API:** Higher-level drivers like `SDRDRV.IRX` and `RWA.IRX` use `LIBSD`'s exported functions (e.g., `sceSdSetParam`, `sceSdSetAddr`) to construct complex soundscapes.

## Reverse Engineering Focus
For a toolkit or emulator, reimplementing `LIBSD` means accurately emulating the SPU2 registers and voice mixing behavior. If reverse engineering the Climax Engine natively, you don't usually reverse `LIBSD` itself, but rather observe how `RWA` calls `LIBSD` to understand how the engine expects sounds to be pitched and mixed.
