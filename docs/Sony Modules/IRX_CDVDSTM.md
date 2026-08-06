# CDVDSTM.IRX (CD/DVD Stream Driver)

## Overview
`CDVDSTM.IRX` is an IOP module responsible for streaming data asynchronously from the optical drive (CD/DVD) on the PlayStation 2. Its primary function in the context of the Climax Engine is to support continuous background loading without interrupting gameplay, which is heavily utilized for streaming background music (BGM) and Full Motion Video (FMV).

## Module Information
- **Filename:** `CDVDSTM.IRX`
- **Internal Module Name:** `cdvd_st_driver`
- **System:** PS2 I/O Processor (MIPS R3000)
- **Role:** High-throughput streaming data driver.

## Architecture & Integration
The module sits between the low-level `cdvdman` (CD/DVD Manager) and higher-level audio/file systems (like `RWA.IRX` and `RTFSSIOP.IRX`). 

### Core Mechanics
1. **Ring Buffers:** To prevent data starvation (underruns), `CDVDSTM` usually employs large circular ring buffers in IOP RAM.
2. **Asynchronous Reading:** When the EE (Main CPU) or another IOP module requests a stream (e.g., a music track), `CDVDSTM` issues asynchronous read commands (`sceCdRead`) to `cdvdman`.
3. **Interrupt Handling:** It uses callbacks/interrupts to notify when a sector or block of sectors has been successfully read, at which point the data is transferred to the destination (like the SPU2 for audio).

## Known Issues in Emulation / Toolkit
If music (BGM) stutters during emulation but sound effects (SFX) play normally, the issue often stems from this module's interaction with the host file system.
- **Root Cause:** SFX are pre-loaded entirely into the 2MB Sound RAM. Streaming audio relies on `CDVDSTM` to constantly fill the buffer. If the RPC call delays or the streaming buffer isn't filled fast enough in the emulated environment, the audio playback pointer overtakes the write pointer, causing a stutter (audio underrun).

## Reverse Engineering Focus
For the Climax Game Engine Toolkit, reversing this module helps define:
- The exact chunk size the engine expects per read operation.
- The RPC command IDs used to initiate, pause, and stop streaming.
