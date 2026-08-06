# SIO2MAN.IRX (Serial I/O 2 Manager)

## Overview
`SIO2MAN.IRX` is a core Sony system module that manages the SIO2 (Serial I/O 2) bus on the PlayStation 2. The SIO2 bus is the hardware interface used to communicate with all external controller ports and memory card slots.

## Module Information
- **Filename:** `SIO2MAN.IRX`
- **Internal Module Name:** `sio2man`
- **System:** PS2 I/O Processor (MIPS R3000)
- **Role:** Bus manager for controllers and memory cards.

## Architecture & Integration
`SIO2MAN` provides a centralized API for other drivers (`PADMAN.IRX` for controllers, `MC2_D.IRX` for memory cards) to safely send and receive data over the shared SIO2 bus without colliding with each other.

### Core Mechanics
1. **Device Registration:** Drivers register their specific devices (e.g., "Port 1, Slot 1") with `SIO2MAN`.
2. **Command Queuing:** It queues read/write commands from multiple drivers and dispatches them sequentially to the hardware bus.
3. **Hardware Abstraction:** It abstracts away the complex DMA and interrupt timing required to speak the proprietary Sony serial protocol.

## Reverse Engineering Focus
This is a standard Sony module. For the Climax Game Engine Toolkit, you do not need to reverse engineer this module unless you are building a full low-level emulator (LLE) for the IOP. High-Level Emulation (HLE) simply intercepts the higher-level calls to `PADMAN` and `MC2_D` instead.
