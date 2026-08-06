# Sony Modules

The `.IRX` files documented in this folder are **standard Sony PlayStation 2 system modules** (or common middleware like `CDVDSTM`). 

As discussed with Ran-J (author of PS2Recomp), these modules handle standard I/O (like reading DVD data, memory card access, and basic audio processing). They do **NOT** contain game-specific logic or file format parsers for Silent Hill Origins/Shattered Memories or Ghost Rider.

Because these are standard modules, the actual parsing of the Climax `.ARC` file system is executed on the EE (Emotion Engine) CPU — meaning the logic resides inside the main game executable (`SLES_546.02`, etc.), not inside these IRX modules.

### The Exceptions
Note that `RTFSSIOP.IRX` (RenderWare File System) and `RWA.IRX` (RenderWare Audio) are **not** standard Sony modules. They are specific to the Climax/RenderWare engine, and their documentation remains in the main `docs/` folder.

- `RTFSSIOP.IRX` acts as a filesystem wrapper over these standard Sony modules, exposing functions like `_asynchronousFileSystemOpen`, `file`, `filename`, `mode`, `callback`, `addr`, `size`, `host:`, `hfs:`, `dvd:`, and `hdd:`. It handles asynchronous reading from the DVD via `cdvdman`, DMA transfers, etc.
