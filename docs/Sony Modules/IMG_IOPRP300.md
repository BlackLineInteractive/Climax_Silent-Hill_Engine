# IOPRP300.IMG (IOP Replacement Image)

## Overview
`IOPRP300.IMG` is not a single IOP module, but rather a packaged binary image containing a complete overlay/replacement for the PlayStation 2's I/O Processor kernel. 

## Module Information
- **Filename:** `IOPRP300.IMG`
- **Internal Modules Included:** `RESET`, `ROMDIR`, `EXTINFO`, `SYSMEM`, `LOADCORE`, `INTRMAN`, etc.
- **System:** PS2 I/O Processor (MIPS R3000)
- **Role:** Core OS/Kernel replacement.

## Architecture & Integration
Different PS2 console revisions shipped with different built-in IOP kernels in their BIOS. To ensure consistent behavior across all consoles, Sony provided `IOPRP` images in their SDKs (in this case, SDK version 3.0.0).

### Core Mechanics
1. **Rebooting the IOP:** When the game boots on the EE (Main CPU), one of its first actions is to call `sceSifRebootIop("cdrom0:\\MODULES\\IOPRP300.IMG;1")`. 
2. **Kernel Overlay:** This command resets the IOP processor and loads the provided `.IMG` file into IOP RAM, replacing the BIOS-provided kernel with the specific versions of `sysmem`, `loadcore`, and `intrman` that the game was compiled against.
3. **Module Loading:** After the IOP is rebooted with this image, the game proceeds to load its specific drivers (like `RWA.IRX` and `RTFSSIOP.IRX`). If the game tried to load these drivers without the correct `IOPRP` image, they might crash due to missing dependencies or mismatched system call tables.

## Reverse Engineering Focus
You do not need to reverse engineer this file. It is a standard Sony kernel image. Emulators (like PCSX2 or `ps2xIOP`) typically intercept the `sceSifRebootIop` call and just configure their HLE environment to simulate an IOP running that specific SDK version, ignoring the actual binary contents of the `.IMG`.
