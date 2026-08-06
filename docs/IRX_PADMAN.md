# PADMAN.IRX (Pad Manager)

## Overview
`PADMAN.IRX` is the standard Sony driver for the PlayStation 2 DualShock 2 controllers. It is responsible for initializing the controllers, polling their state, and managing force feedback (rumble).

## Module Information
- **Filename:** `PADMAN.IRX`
- **Internal Module Name:** `padman`
- **System:** PS2 I/O Processor (MIPS R3000)
- **Role:** High-level controller input manager.

## Architecture & Integration
`PADMAN` relies on `SIO2MAN` to actually transmit data to the controller ports. 

### Core Mechanics
1. **Polling:** It constantly polls the controllers at a set frequency (usually tied to the VSYNC interrupt, e.g., 60Hz) to retrieve button states and analog stick values.
2. **Analog & Pressure:** It parses the complex data packets from the DualShock 2, which include not just digital button presses, but 8-bit pressure sensitivity values for the face buttons and D-Pad.
3. **Actuator Control:** It sends commands to the controller's two vibration motors (a small fast motor and a large slow motor) based on RPC requests from the game engine.

## Reverse Engineering Focus
For the Climax Engine Toolkit, you only need to be aware of `PADMAN` if you are implementing a playable engine port. The RPC commands sent to `PADMAN` will tell you how the engine maps user inputs to its internal event system. For a simple asset viewer, this module is irrelevant.
