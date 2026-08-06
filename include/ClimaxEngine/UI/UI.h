#pragma once
#include <string>
#include <vector>
#include <filesystem>

#include "ClimaxEngine/Platform/PS2/AudioParser.h"

enum class FileBrowserMode {
    Arc,   // pick an .ARC archive to mount
    Mesh,  // pick a loose container file (no extension)
    Txd,   // pick loose .txd texture dictionaries
};

struct FileBrowserState {
    std::string currentPath;
    std::vector<std::filesystem::path> entries;
    std::vector<std::string> selectedTxds;
    std::string selectedMeshContainer;
    bool showBrowser = false;
    FileBrowserMode mode = FileBrowserMode::Arc;
    std::string errorMessage;
    // Deferred navigation: set inside the render loop, executed after it
    std::string pendingNavigate;
    std::string pendingMountArc;
    bool pendingOpenTxd = false;

    void Open(FileBrowserMode m);
    void RefreshEntries();
    void Render();
};

extern FileBrowserState g_FileBrowser;

// Structure / hierarchy panel
void RenderStructureWindow();

// TXD texture browser panel
void RenderTxdWindow();

// SH.ARC contents browser — levels by their real archive names
void RenderArcWindow();

// Built-in manual
void RenderManualWindow();

// Audio player panel. The controls live in main.cpp, next to the SDL device.
void RenderAudioPlayer();
void RenderAnimationPlayer();
void ToggleAudioPlayback();
void SetAudioProgress(float progress);
void StopAudio();
void PlayAudioClip(const AudioClip& clip);
void PlayLibraryEntry(int index);          // decodes g_AudioLibrary[index] first
const AudioClip& CurrentAudioClip();
// Diagnostic: how often the device asked for audio, and how often it asked
// later than the buffer could cover.
void AudioHealth(int& calls, int& late, double& worstMs, double& bufferMs);
// Looks beside the mounted archive for MUSIC/*.RWS and IGC.ARC and fills
// g_AudioLibrary. Safe to call again after mounting a different archive.
void ScanAudioLibrary();

