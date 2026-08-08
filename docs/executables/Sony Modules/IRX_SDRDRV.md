# SDRDRV.IRX (Sound Driver)

## Overview
`SDRDRV.IRX` is a standard Sony Sound Driver that sits one level higher than `LIBSD.IRX`. While `LIBSD` provides raw access to the SPU2 registers, `SDRDRV` provides a structured sequencing and sound effect management system.

## Module Information
- **Filename:** `SDRDRV.IRX`
- **Internal Module Name:** `sdr_driver` / `sdrdrv`
- **Dependencies:** `libsd`, `sysmem`, `intrman`
- **System:** PS2 I/O Processor (MIPS R3000)
- **Role:** Polyphony management and audio sequencing.

## Architecture & Integration
This module acts as a middleman. It receives high-level commands (e.g., "Play Sound Effect 5 at Volume 100") and translates them into the necessary `LIBSD` calls.

### Core Mechanics
1. **Polyphony Manager:** Since the SPU2 only has 48 hardware voices, `SDRDRV` manages voice allocation. If 50 sounds are triggered at once, it determines which 2 sounds to drop (usually based on priority or volume) so the hardware doesn't crash.
2. **Sequencing:** It can parse basic MIDI-like sequence data to play musical notes, handling timing and instrument selection automatically.
3. **Effect DSP:** It manages the SPU2's built-in DSP effects, such as applying specific reverb presets (e.g., "Room", "Hall", "Echo") to specific voices, which is highly relevant for atmosphere in a game like Silent Hill.

## Reverse Engineering Focus
In the Climax Toolkit context, analyzing `SDRDRV` RPC commands can reveal the structure of sound effect triggers (SFX IDs, volume panning formats, and reverb settings) used by the engine's event system.
