# Documentation

Three kinds of thing live here, and they are kept apart because they age
differently.

| | |
|---|---|
| **[TODO.md](TODO.md)** | the work queue — what is open, what was tried, and why it failed. Read this first. |
| **[formats/](formats/)** | how the shipped data is laid out. Written by hand, verified against the archives. |
| **[executables/](executables/)** | what the games' own code says. Recovered from the PS2 binaries. |
| **[generated/](generated/)** | machine output. Never edit by hand — regenerate. |

---

## formats/

| file | covers |
|---|---|
| [SH_FORMAT.md](formats/SH_FORMAT.md) | Silent Hill Origins (PS2/PSP): the archive, containers, geometry, textures, audio, UI string tables, the front-end XML, fonts |
| [SHSM_ARC_FORMAT.md](formats/SHSM_ARC_FORMAT.md) | Shattered Memories (Wii): `data.arc`, GX geometry and textures |
| [ANIMATION_SPEC.md](formats/ANIMATION_SPEC.md) | skeletal animation across both games, plus the Travis clip catalogue |
| [PROTOTYPE_2006.md](formats/PROTOTYPE_2006.md) | the August 2006 PSP prototype, which is a different build entirely |

## executables/

| file | covers |
|---|---|
| [EXECUTABLES.md](executables/EXECUTABLES.md) | how the binaries are read, and what has been recovered from them |
| [SLES_GhostRider_Analysis.md](executables/SLES_GhostRider_Analysis.md) | Ghost Rider's binary — unstripped, so it is the primary source of truth for engine behaviour |
| [IRX_RWA.md](executables/IRX_RWA.md), [IRX_RTFSSIOP.md](executables/IRX_RTFSSIOP.md) | the IOP modules |
| Sony Modules/ | reference material for the stock Sony IRX modules |

A standing caution: Ghost Rider shares Climax's codebase but **not every
routine**. Its `UTILS::GetStringHash` is a real symbol implementing a real
function, and Origins does not use it — see the string-table section of
`SH_FORMAT.md`. Use Ghost Rider to learn what a system does, then confirm the
specifics against Origins' own data.

## generated/

Machine output. Each file has one producer; to refresh it, run that command.

| file | produced by |
|---|---|
| `sho_attribute_map.json` | `python3 tools/sho_attrs.py` |
| `gr_attribute_map.json` | `python3 tools/attrmap.py` on Ghost Rider |
| `sho_behaviour.json` | `python3 tools/sho_behaviour.py` |
| `property_observations.json` | `python3 tools/property_observations.py game-iso/SHO/SH.ARC --json docs/generated/property_observations.json` |
| `port_class_map.json` | `python3 tools/merge_port_map.py` — merges the attribute maps with the observations |
| `sho_class_registry.json` | the class list read out of the archive's type directory |
| `GR_symbols.txt` | `nm` over Ghost Rider's binary |

`port_class_map.json` is the merged view and the one to read; the three inputs
above it are kept so a bad merge can be traced back to its source.

---

## Tools

Everything under `tools/` is a standalone script that reads the retail data.
None of them are needed to build or run the toolkit.

| script | what it does |
|---|---|
| `strings.py` | UI string tables — `dump` to TSV, `build` back, `lookup` an id |
| `extract_scripts.py` | pulls the 40 UI/puzzle/cutscene XML files out of `SH.ARC` |
| `attrmap.py`, `sho_attrs.py`, `sho_behaviour.py` | recover class property tables from the binaries |
| `property_observations.py` | describes each property from the shipped data rather than the code |
| `merge_port_map.py`, `sho_port_gen.py` | merge the above and emit port headers |
| `mips.py`, `sles.py` | the MIPS disassembler and ELF reader the others build on |
| `dump_prototype.py`, `extract_eboot_strings.py` | the 2006 PSP prototype |
| `Ghidra*.java` | headless Ghidra scripts (Java, not Python — the install has no PyGhidra) |
