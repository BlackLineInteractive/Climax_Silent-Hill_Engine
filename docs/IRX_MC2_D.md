# MC2_D.IRX (Memory Card Driver)

## Overview
`MC2_D.IRX` is a standard Sony driver (often updated in later SDK versions, denoted by the `_D` or similar suffixes) for interfacing with the PlayStation 2 Memory Cards (8MB MagicGate cards).

## Module Information
- **Filename:** `MC2_D.IRX`
- **Internal Module Name:** `mc2_d`
- **System:** PS2 I/O Processor (MIPS R3000)
- **Role:** File system driver for memory cards.

## Architecture & Integration
Like `PADMAN`, this module relies on `SIO2MAN` to communicate physically with the memory card slots. However, `MC2_D` implements a full file system hierarchy (FAT-like).

### Core Mechanics
1. **File Operations:** It exposes RPC functions equivalent to standard C file I/O (`open`, `read`, `write`, `mkdir`, `delete`) specifically targeted at the memory card slots (`mc0:` and `mc1:`).
2. **Icon & Save Data:** It handles the specific requirements of PS2 save data, including the mandatory `icon.sys` and 3D icon files used by the PS2 BIOS browser.
3. **MagicGate Authentication:** It silently handles the proprietary MagicGate encryption/authentication required to read and write to official PS2 memory cards.

## Reverse Engineering Focus
If you are analyzing the Climax Game Engine's save system, examining the RPC calls to `MC2_D` will reveal the exact save file structure, how the engine serializes player data (inventory, location, health), and how it generates its 3D save icons.
