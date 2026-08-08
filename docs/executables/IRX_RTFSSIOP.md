# RTFSSIOP.IRX (Real-Time File System Stream IOP)

## Overview
`RTFSSIOP.IRX` is a custom file system streaming module heavily utilized by the Climax Engine (and often associated with RenderWare's streaming systems). It provides an asynchronous, high-speed abstraction layer for loading game assets (archives, level chunks, textures) from the DVD directly into memory without stalling the main processor.

## Module Information
- **Filename:** `RTFSSIOP.IRX`
- **Internal Module Name:** (Inferred: Custom File System Streamer)
- **Dependencies:** `sysmem`, `intrman`, `ioman`, `loadcore`, `sifcmd`
- **System:** PS2 I/O Processor (MIPS R3000)
- **Role:** Asynchronous data streaming and archive management.

## Architecture & Integration
The module uses the Sony `ioman` (I/O Manager) and `sifcmd` (SIF Command/RPC) to act as a bridge between the Main CPU (EE) and the storage medium. 

### Core Mechanics
1. **RPC Interface:** The game engine running on the EE sends RPC commands to `RTFSSIOP` (e.g., "Load file X from ARC Y to memory address Z"). 
2. **Asynchronous DMA:** The module processes this request in the background, reading from the CD/DVD driver and using DMA (Direct Memory Access) to push the data across the SIF bus to the EE memory.
3. **Archive Handling:** It likely contains the low-level logic for traversing Climax's custom `.ARC` containers (like `SH.ARC` or `MO_1_Room102`), identifying offsets, and streaming only the required chunks.

## Reverse Engineering Focus
For the Toolkit (specifically `Loader.cpp` and `Arc.cpp`), decompiling this module reveals:
- The exact magic numbers and table structures of the `.ARC` files as parsed natively on the PS2.
- The block size and streaming strategy used to load level geometry piece by piece.
