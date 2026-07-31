#pragma once
#include <string>
#include <vector>
#include <filesystem>

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
