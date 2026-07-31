#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// ── Buffer-based entry points ───────────────────────────────────────────────
// Everything parses from memory so containers can come either from a loose file
// or straight out of SH.ARC without staging them on disk first.
using NamedBlob = std::pair<std::string, std::vector<uint8_t>>;

void LoadTexturesFromTxdData(const std::vector<uint8_t>& data,
                             const std::vector<std::string>& allowedNames,
                             bool fallback = false);
void LoadGeometryData(const std::vector<uint8_t>& data);
void ParseContainerStructureData(const std::vector<uint8_t>& data);
void LoadLevelData(const std::string& displayName,
                   const std::vector<uint8_t>& container,
                   const std::vector<NamedBlob>& txds);

// ── Path-based wrappers (loose files on disk) ───────────────────────────────
void LoadTexturesFromTxd(const std::string& txdPath,
                         const std::vector<std::string>& allowedNames,
                         bool fallback = false);
void LoadGeometry(const std::string& geomPath);
void LoadLevel(const std::string& meshContainerPath,
               const std::vector<std::string>& txdPaths);
void ParseContainerStructure(const std::string& path);

// ── Archive entry point ─────────────────────────────────────────────────────
// Loads container `entryIndex` from g_Arc together with every texture
// dictionary the archive names after it. False if the entry cannot be inflated.
bool LoadLevelFromArc(int entryIndex);

// Reads a whole file into memory. Empty vector on failure.
std::vector<uint8_t> ReadWholeFile(const std::string& path);
