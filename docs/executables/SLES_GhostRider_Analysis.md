# SLES Analysis: Ghost Rider vs. Silent Hill (SHO / SHSM)

Symbol survey of the main executables (ELF / `SLES`) for three Climax titles:

1. **Silent Hill Origins (SHO)**: `SLES_551.47`
2. **Silent Hill Shattered Memories (SHSM)**: `SLES_555.69`
3. **Ghost Rider (GR)**: `SLES_543.17`

## Results of symbol extraction

- **SHO**: `stripped` — symbols and debug information removed at release.
- **SHSM**: `stripped` — likewise.
- **Ghost Rider**: **not stripped**. It carries a full symbol table, roughly
  **14 000 unique C++ symbols**, including namespaces, classes and methods.

Ghost Rider is therefore the Rosetta Stone for the Climax engine. All three
games share the same architecture, so its symbols recover logic that is
anonymous in SHO and SHSM. See [EXECUTABLES.md](EXECUTABLES.md) for the
disassembly work built on top of this, and `tools/attrmap.py` for the property
table extractor.

## Key findings from the Ghost Rider symbols

Demangling the C++ names turns up the classes responsible for exactly the
things this toolkit re-implements.

### 1. RenderWare FileSystem and ARC parsing

`.ARC` handling lives on the EE (main processor), not in the IOP module:

- `RWS::FileSystem` — the file system root
- `RWS::FileSystem::CArchiveManager` — manages mounted ARC archives
- `RWS::FileSystem::CArchive` — one mounted `.ARC`

`CArchiveManager` methods:

- `FindAchive(char const*)` — spelled that way in the binary
- `Mount(char const*)`
- `FindEntry(char const*, RWS::FileSystem::CArchive**)`

There is also a `MemoryFileSystem` (likely for streaming into memory) and an
`AssetTracker`. This confirms that `RTFSSIOP.IRX` only supplies raw bytes to
`RWS::FileSystem::CArchiveManager` — see [IRX_RTFSSIOP.md](IRX_RTFSSIOP.md).

### 2. Audio and RWA

Both an `Audio::` and an `RWS::` namespace are present:

- `Audio::CAudioRelay` — audio queueing
- `RWS::RwsAudio::AddDictionary(RwaWaveDict*)`
- `RWS::RwsAudio::AllocateVirtualVoice()`
- `RWS::RwsAudio::FadeEnvironment(int, unsigned int)`

RenderWare Audio (`RWA.IRX`, see [IRX_RWA.md](IRX_RWA.md)) talks to the EE-side
`RwsAudio` class through "virtual voices".

### 3. Engine core

Hundreds of methods carry a `ClimaxP1...` prefix, confirming the engine's
internal name as "Climax P1":

- `ClimaxP1AtomicDataReadStream()`
- `ClimaxP1DictionarySchemaCreateChainGraphFromDict()`

A parallel `ClimaxT1...` family covers the material, animation and skinning
plugins that [TODO.md](TODO.md) documents in detail.

## What this means for the toolkit

Knowing the original architecture, the toolkit's own classes can mirror it
(`RWS::FileSystem::CArchiveManager`, `RWS::FileSystem::CArchive`, and so on)
rather than inventing a parallel vocabulary. That keeps the reverse-engineered
implementation aligned with the original code and makes every future symbol
lookup translate directly onto our own structure.
