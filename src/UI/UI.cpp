#include "ClimaxEngine/UI/UI.h"
#include "ClimaxEngine/Core/Arc.h"
#include "ClimaxEngine/Loader/Loader.h"
#include "ClimaxEngine/Core/Common.h"
#include "imgui.h"
#include <algorithm>
#include <cstring>
#include <iostream>

static char arcFilter[128] = "";

FileBrowserState g_FileBrowser;
namespace fs = std::filesystem;

void FileBrowserState::Open(FileBrowserMode m) {
    mode = m;
    showBrowser = true;
    if (currentPath.empty()) {
        currentPath = fs::current_path().string();
    }
    RefreshEntries();
}

void FileBrowserState::RefreshEntries() {
    entries.clear();
    errorMessage.clear(); // Clear previous error messages
    try {
        if (fs::is_directory(currentPath)) {
            for (const auto& entry : fs::directory_iterator(currentPath)) {
                entries.push_back(entry.path());
            }
            std::sort(entries.begin(), entries.end(), [](const fs::path& a, const fs::path& b) {
                bool aIsDir = fs::is_directory(a);
                bool bIsDir = fs::is_directory(b);
                if (aIsDir != bIsDir) return aIsDir;
                return a.filename().string() < b.filename().string();
            });
        }
    }
    catch (const std::filesystem::filesystem_error& e) {
        errorMessage = "Filesystem error: " + std::string(e.what());
        std::cerr << errorMessage << std::endl;
    }
    catch (const std::exception& e) {
        errorMessage = "General error: " + std::string(e.what());
        std::cerr << errorMessage << std::endl;
    }
}

void FileBrowserState::Render() {
    if (!showBrowser) return;

    const bool selectingMesh = (mode == FileBrowserMode::Mesh);
    const bool selectingArc  = (mode == FileBrowserMode::Arc);

    ImGui::SetNextWindowSize(ImVec2(800, 600), ImGuiCond_FirstUseEver);
    const char* title =
        selectingArc  ? "Select SH.ARC / IGC.ARC" :
        selectingMesh ? "Select Mesh Container (no extension)"
                      : "Select .txd texture containers";
    if (ImGui::Begin(title, &showBrowser)) {
        if (!errorMessage.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Error: %s", errorMessage.c_str());
            ImGui::Separator();
        }
        ImGui::Text("Path: %s", currentPath.c_str());
        ImGui::Separator();

        if (ImGui::Button("..") && fs::path(currentPath).has_parent_path()) {
            currentPath = fs::path(currentPath).parent_path().string();
            RefreshEntries();
        }
        ImGui::SameLine();
        if (ImGui::Button("Refresh")) {
            RefreshEntries();
        }

        if (mode == FileBrowserMode::Txd) {
            ImGui::SameLine();
            ImGui::Text("Selected: %zu .txd(s)", selectedTxds.size());
            ImGui::SameLine();
            if (ImGui::Button("Clear")) {
                selectedTxds.clear();
            }
            ImGui::SameLine();
            if (ImGui::Button("Load") && !selectedMeshContainer.empty() && !selectedTxds.empty()) {
                LoadLevel(selectedMeshContainer, selectedTxds);
                showBrowser = false;
            }
        }

        ImGui::Separator();
        ImGui::BeginChild("FileList");

        // These are set inside the loop and acted on AFTER it to avoid
        // invalidating the `entries` iterator mid-loop.
        pendingNavigate.clear();
        pendingMountArc.clear();
        pendingOpenTxd = false;

        for (const auto& entry : entries) {
            bool isDir = fs::is_directory(entry);
            std::string name = entry.filename().string();
            std::string ext = entry.extension().string();

            std::string extLower = ext;
            std::transform(extLower.begin(), extLower.end(), extLower.begin(),
                [](unsigned char c){ return std::tolower(c); });
            bool shouldShow = isDir ||
                (selectingArc  && extLower == ".arc") ||
                (selectingMesh && ext.empty()) ||
                (mode == FileBrowserMode::Txd && extLower == ".txd");

            if (!shouldShow) continue;

            if (isDir) {
                // Single click navigates into the directory.
                if (ImGui::Selectable(("[DIR] " + name).c_str(), false)) {
                    pendingNavigate = entry.string();
                }

                // Convenience button: add all .txd files from this directory.
                if (mode == FileBrowserMode::Txd) {
                    ImGui::SameLine();
                    if (ImGui::Button(("Add all .txd##" + name).c_str())) {
                        try {
                            for (const auto& subEntry : fs::directory_iterator(entry)) {
                                std::string e = subEntry.path().extension().string();
                                std::transform(e.begin(), e.end(), e.begin(),
                                    [](unsigned char c){ return std::tolower(c); });
                                if (e == ".txd") {
                                    std::string p = subEntry.path().string();
                                    if (std::find(selectedTxds.begin(), selectedTxds.end(), p) == selectedTxds.end())
                                        selectedTxds.push_back(p);
                                }
                            }
                        } catch (const std::exception& ex) {
                            errorMessage = std::string("Cannot read directory: ") + ex.what();
                        }
                    }
                }
            }
            else {
                bool isSelected = false;
                if (mode == FileBrowserMode::Txd) {
                    isSelected = std::find(selectedTxds.begin(), selectedTxds.end(), entry.string()) != selectedTxds.end();
                }

                if (ImGui::Selectable(name.c_str(), isSelected)) {
                    if (selectingArc) {
                        pendingMountArc = entry.string();
                    }
                    else if (selectingMesh) {
                        // Single click selects the mesh container and moves to TXD selection.
                        selectedMeshContainer = entry.string();
                        pendingOpenTxd = true;
                    }
                    else {
                        auto it = std::find(selectedTxds.begin(), selectedTxds.end(), entry.string());
                        if (it != selectedTxds.end()) selectedTxds.erase(it);
                        else selectedTxds.push_back(entry.string());
                    }
                }
            }
        }

        ImGui::EndChild();

        // --- Deferred actions (safe: loop is finished, iterator is no longer alive) ---
        if (!pendingNavigate.empty()) {
            currentPath = pendingNavigate;
            RefreshEntries();
        }
        if (!pendingMountArc.empty()) {
            if (g_Arc.Open(pendingMountArc)) {
                showBrowser = false;
                state.showArc = true;
                arcFilter[0] = '\0';
            } else {
                errorMessage = "Cannot open archive: " + g_Arc.Error();
            }
        }
        if (pendingOpenTxd) {
            showBrowser = false;
            Open(FileBrowserMode::Txd);
        }
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Built-in manual
// ---------------------------------------------------------------------------
namespace {

void Head(const char* s) {
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.62f, 0.80f, 1.00f, 1.0f), "%s", s);
    ImGui::Separator();
}

void Key(const char* k, const char* what) {
    ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.45f, 1.0f), "%-18s", k);
    ImGui::SameLine(150);
    ImGui::TextUnformatted(what);
}

} // namespace

void RenderManualWindow() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f),
        ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(620, 620), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Manual", &state.showManual)) { ImGui::End(); return; }

    ImGui::TextWrapped("Climax Silent Hill Engine Toolkit — 3D Level Viewer, Asset Decoder & Archive Extractor "
                       "for Silent Hill Origins and Silent Hill: Shattered Memories.");

    Head("Loading a level");
    ImGui::BulletText("Click \"Open SH.ARC\" and pick SH.ARC from your game folder.");
    ImGui::BulletText("The Archive panel lists every level by its real name.");
    ImGui::BulletText("Click a name to load it. Textures are found automatically.");
    ImGui::BulletText("\"Levels only\" hides textures and other files from the list.");
    ImGui::Spacing();
    ImGui::TextWrapped("Already extracted files still work - use \"Open Loose File\".");

    Head("Moving the camera");
    Key("Right-drag",   "Turn the camera around the pivot");
    Key("Scroll wheel", "Zoom in and out");
    Key("Drag the ball","Same as right-drag (top right corner)");
    Key("Arrows",       "Drag the coloured arrows to move the pivot");
    Key("Ctrl + drag",  "Move the pivot in fixed steps");
    Key("1",            "Reset the camera");
    ImGui::Spacing();
    ImGui::TextWrapped("Levels have fixed cameras built in. Pick one from "
                       "\"Jump to camera\" to see the room the way the game shows it.");

    Head("Keyboard");
    Key("F1", "Hide or show the whole interface");
    Key("F2", "Open or close this manual");
    Key("G",  "Hide or show the pivot arrows");
    Key("1",  "Reset the camera");

    Head("What you see");
    ImGui::TextWrapped(
        "Blue markers are game objects: spawn points, cameras, pickups, lights and "
        "triggers. They sit where the game puts them.");
    ImGui::Spacing();
    ImGui::TextWrapped(
        "Some objects have no position of their own - zones, messages and other "
        "logic. They all sit at 0,0,0. Turn on \"At origin\" if you want to see them.");
    ImGui::Spacing();
    ImGui::TextWrapped(
        "\"Unplaced models\" shows models that no object in the level uses. They have "
        "no position either, so they stack up in the middle of the scene. Off by default.");
    ImGui::Spacing();
    ImGui::TextWrapped(
        "\"Collision Wire\" draws the collision mesh - the invisible shape the player "
        "walks on. Not every level has one.");

    Head("Textures");
    ImGui::BulletText("The Textures panel lists every texture with its size.");
    ImGui::BulletText("Double-click a texture to open it full screen.");
    ImGui::BulletText("In full screen: scroll to zoom, or use +, -, 1:1 and Fit.");
    ImGui::BulletText("Escape closes it.");

    Head("Render modes");
    ImGui::BulletText("Textured - normal view");
    ImGui::BulletText("Vert.Color - the baked lighting, without textures");
    ImGui::BulletText("Flat / Normals - shape only, useful for spotting holes");
    ImGui::BulletText("Depth - distance from the camera");
    ImGui::BulletText("Checker - a grid on the UVs, shows stretched textures");
    ImGui::BulletText("Unlit - texture with no shading at all");

    Head("Exporting to glTF");
    ImGui::TextWrapped(
        "\"Export glTF\" writes a .glb next to the program. The level is split by "
        "texture: every piece that uses the same texture becomes one object named "
        "after that texture, instead of one giant mesh or thousands of fragments.");
    ImGui::Spacing();
    ImGui::BulletText("Embed textures - pack the images into the .glb");
    ImGui::BulletText("Vertex colors - keep the baked lighting");
    ImGui::BulletText("Lights - export the level lights with their real colours");
    ImGui::BulletText("Bake instances - write a copy of a model at each place it is used");
    ImGui::Spacing();
    ImGui::TextWrapped("From the command line:");
    ImGui::TextDisabled("  SHOViewer SH.ARC MO_1_Room102 --export room.glb");

    Head("Known limits");
    ImGui::BulletText("Animations are read but not played yet.");
    ImGui::BulletText("The game's own fog is not reproduced.");
    ImGui::BulletText("A few levels have textures that look stretched or misplaced.");

    ImGui::Spacing();
    ImGui::End();
}

// ---------------------------------------------------------------------------
// SH.ARC contents browser
//
// The archive carries the real internal name of every file, so levels can be
// picked by name ("MO_1_Room102") instead of by hunting for extensionless files
// on disk. Selecting one also pulls in every texture dictionary the archive
// names after it.
// ---------------------------------------------------------------------------
void RenderArcWindow() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + 276.0f, vp->Pos.y + 480.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(360, 420), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Archive", &state.showArc)) { ImGui::End(); return; }

    if (!g_Arc.IsOpen()) {
        ImGui::TextDisabled("No archive mounted.");
        ImGui::TextWrapped("Use \"Open SH.ARC\" to mount the game archive and "
                           "browse levels by their real names.");
        ImGui::End();
        return;
    }

    const auto& entries = g_Arc.Entries();
    ImGui::TextDisabled("%s", g_Arc.Path().c_str());

    static bool onlyContainers = true;
    ImGui::Checkbox("Levels only", &onlyContainers);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Show only entries without a file extension —\n"
                          "those are the geometry containers.");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##arcfilter", "filter by name...", arcFilter, sizeof(arcFilter));

    ImGui::Separator();
    ImGui::BeginChild("##arclist");

    int pendingLoad = -1;
    size_t shown = 0;
    for (size_t i = 0; i < entries.size(); i++) {
        const std::string& n = entries[i].name;
        const bool isContainer = n.find('.') == std::string::npos;
        if (onlyContainers && !isContainer) continue;
        if (arcFilter[0]) {
            std::string lo = n, needle = arcFilter;
            std::transform(lo.begin(), lo.end(), lo.begin(),
                           [](unsigned char c){ return std::tolower(c); });
            std::transform(needle.begin(), needle.end(), needle.begin(),
                           [](unsigned char c){ return std::tolower(c); });
            if (lo.find(needle) == std::string::npos) continue;
        }
        shown++;

        const bool current = (g_CurrentMeshContainer == n);
        ImGui::PushID((int)i);
        if (ImGui::Selectable(n.c_str(), current) && isContainer)
            pendingLoad = (int)i;
        if (ImGui::IsItemHovered()) {
            const size_t nTxd = g_Arc.TxdsFor(n).size();
            ImGui::SetTooltip("%u bytes uncompressed\n%zu matching .txd",
                              entries[i].uncompressedSize, nTxd);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%.0f KB", entries[i].uncompressedSize / 1024.0f);
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::End();

    // Deferred: loading rebuilds the global scene, so do it after the list is done.
    if (pendingLoad >= 0) LoadLevelFromArc(pendingLoad);
    (void)shown;
}

// ---------------------------------------------------------------------------
// Structure / hierarchy window
// ---------------------------------------------------------------------------
void RenderStructureWindow() {
    // Placed to the right of the pinned 256 px control panel — the old default
    // opened it straight on top of that panel.
    ImGui::SetNextWindowPos(ImVec2(276, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(340, 460), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Structure")) { ImGui::End(); return; }

    if (g_MeshTexMap.empty() && g_ShoTypes.empty() && g_ShoSections.empty()) {
        ImGui::TextDisabled("Load a level to see its hierarchy.");
        ImGui::End();
        return;
    }

    // ============================================================
    // Section 1: Game object types (from SHO type directory table)
    // ============================================================
    if (!g_ShoTypes.empty()) {
        ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
        if (ImGui::CollapsingHeader("Game Objects")) {
            ImGui::PushID("gametypes");
            uint32_t total = 0;
            for (const auto& t : g_ShoTypes) total += t.count;
            ImGui::TextDisabled("%zu types  |  %u objects total", g_ShoTypes.size(), total);
            ImGui::Spacing();
            for (const auto& t : g_ShoTypes) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.80f, 0.90f, 1.00f, 1.0f));
                ImGui::Bullet();
                ImGui::SameLine(0, 4);
                ImGui::Text("%-36s", t.name.c_str());
                ImGui::PopStyleColor();
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.55f, 0.88f, 0.55f, 1.0f), "x%u", t.count);
            }
            ImGui::PopID();
        }
        ImGui::Spacing();
    }

    // ============================================================
    // Section 2: File sections (0x716 containers)
    // ============================================================
    if (!g_ShoSections.empty()) {
        ImGui::SetNextItemOpen(false, ImGuiCond_FirstUseEver);
        if (ImGui::CollapsingHeader("File Sections")) {
            ImGui::PushID("sections");
            ImGui::TextDisabled("%zu sections", g_ShoSections.size());
            ImGui::Spacing();
            for (const auto& s : g_ShoSections) {
                // Colour-code by section type
                ImVec4 col = ImVec4(0.75f, 0.90f, 0.75f, 1.0f);
                if      (s.name == "rwID_CBSP")          col = ImVec4(0.95f, 0.50f, 0.30f, 1.0f);
                else if (s.name == "rwID_CLUMP")         col = ImVec4(0.90f, 0.80f, 0.30f, 1.0f);
                else if (s.name.rfind("rwaID", 0) == 0)  col = ImVec4(0.65f, 0.65f, 0.85f, 1.0f);
                else if (s.name == "rwID_POLYAREA")      col = ImVec4(0.80f, 0.60f, 0.90f, 1.0f);

                ImGui::PushStyleColor(ImGuiCol_Text, col);
                ImGui::Bullet();
                ImGui::SameLine(0, 4);
                ImGui::Text("%-26s", s.name.c_str());
                ImGui::PopStyleColor();
                ImGui::SameLine();
                ImGui::TextDisabled("@ 0x%05X  (%u B)", s.offset, s.size);
            }
            // Collision summary
            if (g_Collision.uploaded) {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.15f, 0.95f, 0.30f, 1.0f),
                    "  Collision: %zu verts  %zu tris",
                    g_Collision.verts.size(), g_Collision.indices.size() / 3);
            }
            if (!g_GameObjects.empty()) {
                size_t placed = 0;
                for (const auto& go : g_GameObjects) if (!go.atOrigin) placed++;
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.47f, 0.85f, 1.0f, 1.0f),
                    "  Game objects: %zu  (%zu placed)", g_GameObjects.size(), placed);
                ImGui::Indent();
                for (size_t i = 0; i < g_GameObjects.size(); i++) {
                    const auto& go = g_GameObjects[i];
                    if (go.atOrigin)
                        ImGui::TextDisabled("[%2zu] %-26s  (logical)", i, go.className.c_str());
                    else
                        ImGui::TextColored(ImVec4(0.72f, 0.90f, 1.0f, 1.0f),
                            "[%2zu] %-26s  (%.2f, %.2f, %.2f)", i, go.className.c_str(),
                            go.position.x, go.position.y, go.position.z);
                    if (ImGui::IsItemHovered() && !go.instName.empty())
                        ImGui::SetTooltip("%s\n@ 0x%05X", go.instName.c_str(), go.offset);
                }
                ImGui::Unindent();
            }
            if (!g_Clumps.empty()) {
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.15f, 1.0f),
                    "  Clumps: %zu objects", g_Clumps.size());
                ImGui::Indent();
                for (size_t i = 0; i < g_Clumps.size(); i++) {
                    const auto& cl = g_Clumps[i];
                    ImGui::TextDisabled("[%zu] %-20s  (%.2f, %.2f, %.2f)",
                        i, cl.sectionName.c_str(),
                        cl.position.x, cl.position.y, cl.position.z);
                }
                ImGui::Unindent();
            }
            ImGui::PopID();
        }
        ImGui::Spacing();
    }

    // ============================================================
    // Section 3: Scene — texture groups → meshes
    // ============================================================
    std::string sceneName = g_CurrentMeshContainer.empty()
        ? "(no level)"
        : fs::path(g_CurrentMeshContainer).filename().string();

    size_t totalMeshes = g_Chunks.size();
    size_t totalTris   = 0;
    for (const auto& ch : g_Chunks) totalTris += ch.vertices.size() / 3;

    ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
    bool rootOpen = ImGui::TreeNodeEx("##root",
        ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen,
        "%s", sceneName.c_str());
    if (rootOpen) {
        ImGui::SameLine();
        ImGui::TextDisabled("  %zu meshes  %zu tris", totalMeshes, totalTris);
    }

    if (rootOpen) {
        for (const auto& [texName, chunkIdxs] : g_MeshTexMap) {
            size_t triCount = 0;
            for (int ci : chunkIdxs)
                if (ci < (int)g_Chunks.size())
                    triCount += g_Chunks[ci].vertices.size() / 3;

            // Resolve texture (try lowercase then uppercase)
            bool hasTex = g_TextureMap.count(texName) > 0;
            if (!hasTex) {
                std::string u = texName;
                for (auto& x : u) x = (char)toupper((unsigned char)x);
                hasTex = g_TextureMap.count(u) > 0;
            }

            // Thumbnail (16 px)
            GLuint thumbId = 0;
            {
                auto it = g_TexInfo.find(texName);
                if (it != g_TexInfo.end()) thumbId = it->second.glID;
                else {
                    std::string u = texName;
                    for (auto& x : u) x = (char)toupper((unsigned char)x);
                    auto it2 = g_TexInfo.find(u);
                    if (it2 != g_TexInfo.end()) thumbId = it2->second.glID;
                }
            }

            ImGui::PushID(texName.c_str());

            if (thumbId) { ImGui::Image((ImTextureID)(intptr_t)thumbId, ImVec2(16, 16)); ImGui::SameLine(); }
            else          { ImGui::Dummy(ImVec2(16, 16)); ImGui::SameLine(); }

            if (!hasTex) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.45f, 1.0f));
            bool nodeOpen = ImGui::TreeNodeEx("##mat",
                ImGuiTreeNodeFlags_SpanAvailWidth,
                "%s", texName.c_str());
            if (!hasTex) {
                ImGui::SameLine(); ImGui::TextColored(ImVec4(1.f,0.4f,0.4f,1.f), " [MISSING]");
                ImGui::PopStyleColor();
            } else {
                ImGui::SameLine();
                ImGui::TextDisabled("  %zu meshes  %zu tris", chunkIdxs.size(), triCount);
            }

            if (nodeOpen) {
                for (int ci : chunkIdxs) {
                    if (ci >= (int)g_Chunks.size()) continue;
                    size_t t = g_Chunks[ci].vertices.size() / 3;
                    ImGui::Bullet();
                    ImGui::SameLine();
                    ImGui::TextDisabled("Mesh #%d   (%zu tris)", ci, t);
                }
                ImGui::TreePop();
            }

            ImGui::PopID();
        }
        ImGui::TreePop();
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
// TXD texture browser
// ---------------------------------------------------------------------------
static bool        s_texFullscreen = false;
static std::string s_texFullName;
static float       s_texZoom = 1.0f;

// Helper: is string all-uppercase?
static bool IsAllUpper(const std::string& s) {
    for (char c : s) if (c >= 'a' && c <= 'z') return false;
    return true;
}

void RenderTxdWindow() {
    // Anchored to the right edge of the viewport rather than to a hard-coded
    // 1280 px width, and pushed below the orbit sphere overlay it used to sit under.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float TXD_W = 230.0f, TXD_TOP = 140.0f;
    ImGui::SetNextWindowPos(
        ImVec2(vp->Pos.x + vp->Size.x - TXD_W - 10.0f, vp->Pos.y + TXD_TOP),
        ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(
        ImVec2(TXD_W, std::max(200.0f, vp->Size.y - TXD_TOP - 10.0f)),
        ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Textures")) { ImGui::End(); return; }

    if (g_TexInfo.empty()) {
        ImGui::TextDisabled("(no textures loaded)");
        ImGui::End();
        return;
    }

    const float THUMB = 56.0f;
    const float ITEM_H = THUMB + 6.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 4));
    ImGui::BeginChild("##txdlist", ImVec2(0, 0), false);

    for (const auto& [name, pi] : g_TexInfo) {
        // Skip upper-case aliases (duplicates of lowercase entries)
        if (IsAllUpper(name)) {
            std::string lo = name;
            for (auto& c : lo) c = (char)tolower((unsigned char)c);
            if (g_TexInfo.count(lo)) continue;
        }

        ImGui::PushID(name.c_str());

        // Row background highlight on hover
        float rowY = ImGui::GetCursorPosY();
        ImVec2 rowMin = ImGui::GetCursorScreenPos();
        ImVec2 rowMax = ImVec2(rowMin.x + ImGui::GetContentRegionAvail().x, rowMin.y + ITEM_H);
        bool hovered = ImGui::IsMouseHoveringRect(rowMin, rowMax);
        if (hovered)
            ImGui::GetWindowDrawList()->AddRectFilled(rowMin, rowMax,
                IM_COL32(60, 100, 160, 80), 4.0f);

        // Thumbnail
        ImGui::Image((ImTextureID)(intptr_t)pi.glID, ImVec2(THUMB, THUMB));
        ImGui::SameLine(THUMB + 8.0f);

        // Text info column
        ImGui::BeginGroup();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
        ImGui::TextUnformatted(name.c_str());
        ImGui::PopTextWrapPos();
        ImGui::TextDisabled("%d x %d  %dbit", pi.width, pi.height, pi.depth);
        ImGui::EndGroup();

        // Invisible selectable over the entire row
        ImGui::SetCursorPosY(rowY);
        if (ImGui::Selectable("##row", false,
                ImGuiSelectableFlags_AllowDoubleClick |
                ImGuiSelectableFlags_SpanAllColumns,
                ImVec2(0, ITEM_H))) {
            if (ImGui::IsMouseDoubleClicked(0)) {
                s_texFullscreen = true;
                s_texFullName   = name;
                s_texZoom       = 1.0f;
            }
        }

        ImGui::PopID();
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::End();

    // ---- Fullscreen texture viewer ----
    if (!s_texFullscreen) return;

    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::SetNextWindowBgAlpha(0.93f);
    ImGui::Begin("##texfull", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);

    // Top bar
    if (ImGui::Button("  X  ") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        s_texFullscreen = false;
        ImGui::End();
        return;
    }
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.8f, 0.9f, 1.0f, 1.0f), "%s", s_texFullName.c_str());
    if (g_TexInfo.count(s_texFullName)) {
        auto& pi = g_TexInfo[s_texFullName];
        ImGui::SameLine();
        ImGui::TextDisabled("  %d x %d  %dbit", pi.width, pi.height, pi.depth);
    }
    ImGui::SameLine(ImGui::GetWindowWidth() - 220.0f);
    if (ImGui::Button(" - ##z")) s_texZoom = std::max(s_texZoom * 0.8f,  0.05f);
    ImGui::SameLine();
    if (ImGui::Button(" + ##z")) s_texZoom = std::min(s_texZoom * 1.25f, 16.0f);
    ImGui::SameLine();
    if (ImGui::Button("1:1"))    s_texZoom = 1.0f;
    ImGui::SameLine();
    if (ImGui::Button("Fit") && g_TexInfo.count(s_texFullName)) {
        auto& pi = g_TexInfo[s_texFullName];
        float fw = io.DisplaySize.x - 24;
        float fh = io.DisplaySize.y - 48;
        s_texZoom = std::min(fw / std::max(pi.width, 1),
                             fh / std::max(pi.height, 1));
    }

    // Scroll-to-zoom
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && io.MouseWheel != 0.0f)
        s_texZoom = std::clamp(s_texZoom * (io.MouseWheel > 0 ? 1.1f : 0.9f), 0.05f, 16.0f);

    ImGui::Separator();

    if (g_TexInfo.count(s_texFullName)) {
        auto& pi = g_TexInfo[s_texFullName];
        float tw = pi.width  * s_texZoom;
        float th = pi.height * s_texZoom;
        ImGui::BeginChild("##imgview", ImVec2(0, 0), false,
            ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_AlwaysVerticalScrollbar);
        // Center small images
        float offX = std::max(0.0f, (ImGui::GetContentRegionAvail().x - tw) * 0.5f);
        float offY = std::max(0.0f, (ImGui::GetContentRegionAvail().y - th) * 0.5f);
        if (offX > 0 || offY > 0) ImGui::SetCursorPos(ImVec2(offX, offY));
        ImGui::Image((ImTextureID)(intptr_t)pi.glID, ImVec2(tw, th));
        ImGui::EndChild();
    }
    ImGui::End();
}

