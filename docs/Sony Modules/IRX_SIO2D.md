# SIO2D.IRX (Serial I/O 2 Driver)

## Overview
`SIO2D.IRX` is the low-level hardware driver companion to `SIO2MAN.IRX`. While `SIO2MAN` manages the queues and API, `SIO2D` handles the actual physical interrupts and DMA transfers for the SIO2 bus.

## Module Information
- **Filename:** `SIO2D.IRX`
- **Internal Module Name:** `sio2d`
- **System:** PS2 I/O Processor (MIPS R3000)
- **Role:** Low-level hardware interface for the SIO2 bus.

## Architecture & Integration
This module directly interacts with the IOP's hardware registers mapped to the SIO2 interface.

### Core Mechanics
1. **Interrupt Handling:** When a memory card or controller finishes sending a packet of data, it triggers a hardware interrupt. `SIO2D` catches this interrupt and notifies `SIO2MAN`.
2. **DMA Transfers:** It configures Direct Memory Access (DMA) channels to rapidly stream data (like a large memory card save block) from the SIO2 hardware buffer directly into IOP RAM.

## Reverse Engineering Focus
Like `SIO2MAN`, this is a standard system driver. It is rarely modified by game developers and holds no game-specific logic or asset formats. It is generally ignored in toolkits focused on asset extraction or HLE (High-Level Emulation).
