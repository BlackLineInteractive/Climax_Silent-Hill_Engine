#include "ClimaxEngine/UI/UI.h"
#include "ClimaxEngine/Core/Common.h"
#include "ClimaxEngine/Render/GPUMesh.h"
#include "ClimaxEngine/Core/RWS/FileSystem/CArchiveManager.h"
#include "ClimaxEngine/Loader/Loader.h"
#include "ClimaxEngine/SG/SceneObject.h"
#include "imgui.h"
#include <algorithm>
#include "im_anim.h"
#include <string>
#include <map>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>

// Defined in main.cpp — persists the last-opened arc path to disk.
extern void SaveArcPref(const std::string &arcPath);
extern std::string LoadArcPref();

static char arcFilter[128] = "";

FileBrowserState g_FileBrowser;
namespace fs = std::filesystem;

// A path as it should appear on screen.
//
// The home directory is written as "~", so a panel does not put the machine's
// account name in every screenshot, and a long path keeps its tail rather than
// its head -- the archive name is what a reader needs, the directories above it
// are not. Purely cosmetic: nothing opens the abbreviated form.
static std::string DisplayPath(const std::string &raw, size_t maxLen = 52) {
  std::string s = raw;
#ifdef _WIN32
  const char *home = std::getenv("USERPROFILE");
#else
  const char *home = std::getenv("HOME");
#endif
  if (home && *home) {
    const std::string h(home);
    if (s.size() >= h.size() && s.compare(0, h.size(), h) == 0)
      s = "~" + s.substr(h.size());
  }
  if (s.size() > maxLen) {
    // Cut on a separator so the result still reads as a path.
    size_t cut = s.size() - (maxLen - 3);
    const size_t sep = s.find_first_of("/\\", cut);
    if (sep != std::string::npos)
      cut = sep;
    s = "..." + s.substr(cut);
  }
  return s;
}

// Where the browser should open the first time it is used.
//
// The working directory is no help: launched from Finder, an .app bundle
// inherits "/", so the browser opened on a list of system folders with the game
// data twenty clicks away. Somewhere the game data has actually been seen is a
// far better guess -- the mounted archive, then the one remembered from the
// last run, then the home directory.
static std::string DefaultBrowseDir() {
  auto tryDir = [](const std::string &file, std::string &out) {
    if (file.empty())
      return false;
    std::error_code ec;
    const fs::path p = fs::path(file).parent_path();
    if (p.empty() || !fs::is_directory(p, ec))
      return false;
    out = p.string();
    return true;
  };

  std::string out;
  if (auto *arc = ClimaxEngine::RWS::FileSystem::CArchiveManager::GetInstance()
                      .GetFirstArchive())
    if (tryDir(arc->Path(), out))
      return out;
  if (tryDir(LoadArcPref(), out))
    return out;

#ifdef _WIN32
  const char *home = std::getenv("USERPROFILE");
#else
  const char *home = std::getenv("HOME");
#endif
  std::error_code ec;
  if (home && *home && fs::is_directory(home, ec))
    return home;

  const fs::path cwd = fs::current_path(ec);
  return ec ? std::string("/") : cwd.string();
}

void FileBrowserState::Open(FileBrowserMode m) {
  mode = m;
  showBrowser = true;
  if (currentPath.empty()) {
    currentPath = DefaultBrowseDir();
  }
  RefreshEntries();
}

void FileBrowserState::RefreshEntries() {
  entries.clear();
  errorMessage.clear(); // Clear previous error messages
  try {
    if (fs::is_directory(currentPath)) {
      for (const auto &entry : fs::directory_iterator(currentPath)) {
        entries.push_back(entry.path());
      }
      std::sort(entries.begin(), entries.end(),
                [](const fs::path &a, const fs::path &b) {
                  bool aIsDir = fs::is_directory(a);
                  bool bIsDir = fs::is_directory(b);
                  if (aIsDir != bIsDir)
                    return aIsDir;
                  return a.filename().string() < b.filename().string();
                });
    }
  } catch (const std::filesystem::filesystem_error &e) {
    errorMessage = "Filesystem error: " + std::string(e.what());
    std::cerr << errorMessage << std::endl;
  } catch (const std::exception &e) {
    errorMessage = "General error: " + std::string(e.what());
    std::cerr << errorMessage << std::endl;
  }
}

void FileBrowserState::Render() {
  if (!showBrowser)
    return;

  const bool selectingMesh = (mode == FileBrowserMode::Mesh);
  const bool selectingArc = (mode == FileBrowserMode::Arc);

  ImGui::SetNextWindowSize(ImVec2(800, 600), ImGuiCond_FirstUseEver);
  const char *title = selectingArc    ? "Select .ARC / .arc"
                      : selectingMesh ? "Select Mesh Container (no extension)"
                                      : "Select .txd texture containers";
  if (ImGui::Begin(title, &showBrowser)) {
    if (!errorMessage.empty()) {
      ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Error: %s",
                         errorMessage.c_str());
      ImGui::Separator();
    }
    ImGui::Text("Path: %s", DisplayPath(currentPath, 64).c_str());
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", currentPath.c_str());
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
      if (ImGui::Button("Load") && !selectedMeshContainer.empty() &&
          !selectedTxds.empty()) {
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

    for (const auto &entry : entries) {
      bool isDir = fs::is_directory(entry);
      std::string name = entry.filename().string();
      std::string ext = entry.extension().string();

      std::string extLower = ext;
      std::transform(extLower.begin(), extLower.end(), extLower.begin(),
                     [](unsigned char c) { return std::tolower(c); });
      bool shouldShow = isDir || (selectingArc && extLower == ".arc") ||
                        (selectingMesh && ext.empty()) ||
                        (mode == FileBrowserMode::Txd && extLower == ".txd");

      if (!shouldShow)
        continue;

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
              for (const auto &subEntry : fs::directory_iterator(entry)) {
                std::string e = subEntry.path().extension().string();
                std::transform(e.begin(), e.end(), e.begin(),
                               [](unsigned char c) { return std::tolower(c); });
                if (e == ".txd") {
                  std::string p = subEntry.path().string();
                  if (std::find(selectedTxds.begin(), selectedTxds.end(), p) ==
                      selectedTxds.end())
                    selectedTxds.push_back(p);
                }
              }
            } catch (const std::exception &ex) {
              errorMessage = std::string("Cannot read directory: ") + ex.what();
            }
          }
        }
      } else {
        bool isSelected = false;
        if (mode == FileBrowserMode::Txd) {
          isSelected = std::find(selectedTxds.begin(), selectedTxds.end(),
                                 entry.string()) != selectedTxds.end();
        }

        if (ImGui::Selectable(name.c_str(), isSelected)) {
          if (selectingArc) {
            pendingMountArc = entry.string();
          } else if (selectingMesh) {
            // Single click selects the mesh container and moves to TXD
            // selection.
            selectedMeshContainer = entry.string();
            pendingOpenTxd = true;
          } else {
            auto it = std::find(selectedTxds.begin(), selectedTxds.end(),
                                entry.string());
            if (it != selectedTxds.end())
              selectedTxds.erase(it);
            else
              selectedTxds.push_back(entry.string());
          }
        }
      }
    }

    ImGui::EndChild();

    // --- Deferred actions (safe: loop is finished, iterator is no longer
    // alive) ---
    if (!pendingNavigate.empty()) {
      currentPath = pendingNavigate;
      RefreshEntries();
    }
    if (!pendingMountArc.empty()) {
      ClimaxEngine::RWS::FileSystem::CArchiveManager::GetInstance()
          .UnmountAll();
      if (ClimaxEngine::RWS::FileSystem::CArchiveManager::GetInstance().Mount(
              pendingMountArc)) {
        ScanAudioLibrary();
        SaveArcPref(pendingMountArc); // remember for next launch
        showBrowser = false;
        state.showArc = true;
        arcFilter[0] = '\0';
      } else {
        errorMessage =
            "Cannot open archive: " +
            ClimaxEngine::RWS::FileSystem::CArchiveManager::GetInstance()
                .GetFirstArchive()
                ->Error();
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

void Head(const char *s) {
  ImGui::Spacing();
  ImGui::TextColored(ImVec4(0.62f, 0.80f, 1.00f, 1.0f), "%s", s);
  ImGui::Separator();
}

void Key(const char *k, const char *what) {
  ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.45f, 1.0f), "%-18s", k);
  ImGui::SameLine(150);
  ImGui::TextUnformatted(what);
}

} // namespace

void RenderManualWindow() {
  const ImGuiViewport *vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(
      ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f),
      ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(620, 620), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Manual", &state.showManual)) {
    ImGui::End();
    return;
  }

  ImGui::TextWrapped(
      "Climax Silent Hill Engine Toolkit 0.5 — 3D Level Viewer, Asset Decoder "
      "& Archive Extractor "
      "for Silent Hill Origins and Silent Hill: Shattered Memories.");

  Head("Loading a level");
  ImGui::BulletText(
      "Click \"Open SH.ARC\" and pick SH.ARC from your game folder.");
  ImGui::BulletText("The Archive panel lists every level by its real name.");
  ImGui::BulletText(
      "Click a name to load it. Textures are found automatically.");
  ImGui::BulletText(
      "\"Levels only\" hides textures and other files from the list.");
  ImGui::Spacing();
  ImGui::TextWrapped(
      "Already extracted files still work - use \"Open Loose File\".");

  Head("Moving the camera");
  Key("Right-drag", "Turn the camera around the pivot");
  Key("Scroll wheel", "Zoom in and out");
  Key("Drag the ball", "Same as right-drag (top right corner)");
  Key("Arrows", "Drag the coloured arrows to move the pivot");
  Key("Ctrl + drag", "Move the pivot in fixed steps");
  Key("1", "Reset the camera");
  ImGui::Spacing();
  ImGui::TextWrapped(
      "Levels have fixed cameras built in. Pick one from "
      "\"Jump to camera\" to see the room the way the game shows it.");

  Head("Keyboard");
  Key("F1", "Hide or show the whole interface");
  Key("F2", "Open or close this manual");
  Key("G", "Hide or show the pivot arrows");
  Key("1", "Reset the camera");

  Head("What you see");
  ImGui::TextWrapped("Blue markers are game objects: spawn points, cameras, "
                     "pickups, lights and "
                     "triggers. They sit where the game puts them.");
  ImGui::Spacing();
  ImGui::TextWrapped(
      "Some objects have no position of their own - zones, messages and other "
      "logic. They all sit at 0,0,0. Turn on \"At origin\" if you want to see "
      "them.");
  ImGui::Spacing();
  ImGui::TextWrapped("\"Unplaced models\" shows models that no object in the "
                     "level uses. They have "
                     "no position either, so they stack up in the middle of "
                     "the scene. Off by default.");
  ImGui::Spacing();
  ImGui::TextWrapped("\"Collision Wire\" draws the collision mesh - the "
                     "invisible shape the player "
                     "walks on. Not every level has one.");

  Head("Textures");
  ImGui::BulletText("The Textures panel lists every texture with its size.");
  ImGui::BulletText("Double-click a texture to open it full screen.");
  ImGui::BulletText(
      "In full screen: scroll to zoom, or use +, -, 1:1 and Fit.");
  ImGui::BulletText("Escape closes it.");

  Head("Render modes");
  ImGui::BulletText("Textured - normal view");
  ImGui::BulletText("Vert.Color - the baked lighting, without textures");
  ImGui::BulletText("Flat / Normals - shape only, useful for spotting holes");
  ImGui::BulletText("Depth - distance from the camera");
  ImGui::BulletText("Checker - a grid on the UVs, shows stretched textures");
  ImGui::BulletText("Unlit - texture with no shading at all");

  Head("Shattered Memories (Wii)");
  ImGui::TextWrapped(
      "Open data.arc or igc.arc the same way you open SH.ARC. These archives "
      "keep no file names, so entries are labelled from what is inside them - "
      "a texture name, an asset path, or an audio file name.");
  ImGui::Spacing();
  ImGui::TextWrapped(
      "Click a container to load it. Levels, props and characters all show up "
      "in 3D with their textures, the same as the PlayStation 2 game.");
  ImGui::Spacing();
  ImGui::TextWrapped(
      "Characters stand in their rest pose - the Wii skinning data is read but "
      "not applied yet.");

  Head("Sound");
  ImGui::TextWrapped(
      "The Playback panel opens by itself the first time there is something to "
      "play. After that use Panels > Audio to show or hide it. It has three "
      "lists:");
  ImGui::Spacing();
  ImGui::BulletText("Level - the sounds stored inside the level container "
                    "itself: footsteps, doors, room tone. Every level carries "
                    "its own bank. Click one to hear it.");
  ImGui::BulletText("Music - the tracks from the MUSIC folder on the disc.");
  ImGui::BulletText("Cutscenes - the speech and music from IGC.ARC.");
  ImGui::Spacing();
  ImGui::TextWrapped("Music and cutscenes are found on their own, as long as "
                     "MUSIC and IGC.ARC "
                     "sit in the same folder as SH.ARC. You can also drag a "
                     "sound file onto the "
                     "window: .rws, .ads, .vag and the IGC streams all work.");
  ImGui::Spacing();
  ImGui::BulletText("Loop - repeat the sound instead of stopping at the end.");
  ImGui::BulletText("Save WAV - write the sound next to the program.");

  Head("Playback panel");
  ImGui::TextWrapped("Sound, skeletal animation and UV animation all live in "
                     "one window now - open it with the \"Playback\" checkbox "
                     "under Panels.");
  ImGui::Spacing();
  ImGui::BulletText("Sound - the player described above.");
  ImGui::BulletText("Skeletal - character animation. Clips are read from the "
                    "container but nothing moves yet.");
  ImGui::BulletText("UV - the scrolling texture animations that drive fire "
                    "and torches.");
  ImGui::Spacing();
  ImGui::TextWrapped("The UV tab lists every clip the level carries and shows, "
                     "per layer, how fast it scrolls. Fire uses two layers at "
                     "different speeds; that column is how you tell a stalled "
                     "clip from a slow one.");

  Head("Settings");
  ImGui::TextWrapped("The Settings section at the bottom of this panel holds "
                     "everything about the interface itself.");
  ImGui::Spacing();
  Key("Scale", "Size of every panel and label");
  Key("Motion", "Speed of transitions; 0 turns them off");
  Key("Tooltips", "These explanations, on or off");
  ImGui::Spacing();
  ImGui::TextWrapped("The archive you last opened is remembered and re-opened "
                     "on the next run. \"Forget\" clears it.");

  Head("Exporting to glTF");
  ImGui::TextWrapped("\"Export glTF\" writes a .glb next to the program. The "
                     "level is split by "
                     "texture: every piece that uses the same texture becomes "
                     "one object named "
                     "after that texture, instead of one giant mesh or "
                     "thousands of fragments.");
  ImGui::Spacing();
  ImGui::BulletText("Embed textures - pack the images into the .glb");
  ImGui::BulletText("Vertex colors - keep the baked lighting");
  ImGui::BulletText("Lights - export the level lights with their real colours");
  ImGui::BulletText(
      "Bake instances - write a copy of a model at each place it is used");
  ImGui::Spacing();
  ImGui::TextWrapped("From the command line:");
  ImGui::TextDisabled(
      "  ClimaxGameEngineToolkit SH.ARC MO_1_Room102 --export room.glb");

  Head("Known limits");
  ImGui::BulletText("Animations are read but not played yet.");
  ImGui::BulletText("The game's own fog is not reproduced.");
  ImGui::BulletText(
      "A few levels have textures that look stretched or misplaced.");
  ImGui::BulletText("Cutscene video is not decoded - only the sound.");

  Head("License");
  ImGui::TextWrapped(
      "GNU General Public License v3. You may use, change and share this "
      "program, as long as anything you share stays under the same license "
      "and keeps the source available. See the LICENSE file.");

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
  const ImGuiViewport *vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + 276.0f, vp->Pos.y + 480.0f),
                          ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(360, 420), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Archive", &state.showArc)) {
    ImGui::End();
    return;
  }

  if (!ClimaxEngine::RWS::FileSystem::CArchiveManager::GetInstance()
           .GetFirstArchive()) {
    ImGui::TextDisabled("No archive mounted.");
    ImGui::TextWrapped("Use \"Open SH.ARC\" to mount the game archive and "
                       "browse levels by their real names.");
    ImGui::End();
    return;
  }

  const auto &entries =
      ClimaxEngine::RWS::FileSystem::CArchiveManager::GetInstance()
          .GetFirstArchive()
          ->Entries();
  const std::string arcPath =
      ClimaxEngine::RWS::FileSystem::CArchiveManager::GetInstance()
          .GetFirstArchive()
          ->Path();
  ImGui::TextDisabled("%s", DisplayPath(arcPath).c_str());
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", arcPath.c_str());

  static bool onlyContainers = true;
  static bool sortBySize = false;
  ImGui::Checkbox("Levels only", &onlyContainers);
  ImGui::SameLine();
  ImGui::Checkbox("Sort by size", &sortBySize);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip(
        "Show only entries without a known file extension —\n"
        "hides .arc, .txd, .xml, etc. Game containers\n"
        "sometimes use a period as a qualifier (CPlayer.Travis).\n"
        "Turn this off to reach the audio in IGC.ARC.");
  ImGui::SetNextItemWidth(-1);
  ImGui::InputTextWithHint("##arcfilter", "filter by name...", arcFilter,
                           sizeof(arcFilter));

  ImGui::Separator();
  ImGui::BeginChild("##arclist", ImVec2(0, 0), false, ImGuiWindowFlags_NavFlattened);

  int pendingLoad = -1;
  int pendingPlay = -1;
  size_t shown = 0;

  // Entries the audio decoder owns. IGC.ARC is nothing but these, and feeding
  // one to the container parser produces an empty scene and no sound.
  auto isAudioEntry = [](const std::string &n) {
    const size_t dot = n.rfind('.');
    if (dot == std::string::npos)
      return false;
    std::string e = n.substr(dot + 1);
    for (auto &c : e)
      c = (char)tolower((unsigned char)c);
    return e == "igcstream" || e == "ads" || e == "vag" || e == "rws" ||
           e == "abc";
  };

  std::vector<int> filteredIndices;
  for (size_t i = 0; i < entries.size(); i++) {
    const std::string &n = entries[i].name;
    auto dotPos = n.rfind('.');
    bool isKnownExt = false;
    if (dotPos != std::string::npos) {
      std::string ext = n.substr(dotPos + 1);
      bool allLower = !ext.empty() &&
                      std::all_of(ext.begin(), ext.end(), [](unsigned char c) {
                        return c == tolower(c);
                      });
      isKnownExt = allLower && ext.size() <= 5;
    }
    const bool isContainer = !isKnownExt;
    if (onlyContainers && !isContainer)
      continue;
    if (arcFilter[0]) {
      std::string lo = n, needle = arcFilter;
      std::transform(lo.begin(), lo.end(), lo.begin(),
                     [](unsigned char c) { return std::tolower(c); });
      std::transform(needle.begin(), needle.end(), needle.begin(),
                     [](unsigned char c) { return std::tolower(c); });
      if (lo.find(needle) == std::string::npos)
        continue;
    }
    filteredIndices.push_back((int)i);
  }

  if (sortBySize) {
    std::sort(filteredIndices.begin(), filteredIndices.end(), [&](int a, int b) {
      return entries[a].Size() > entries[b].Size();
    });
  }

  int currentIndexInList = -1;
  shown = filteredIndices.size();

  for (size_t list_i = 0; list_i < filteredIndices.size(); list_i++) {
    int i = filteredIndices[list_i];
    const std::string &n = entries[i].name;
    const bool audio = isAudioEntry(n);
    const bool current = (g_CurrentMeshContainer == n);
    if (current) currentIndexInList = (int)list_i;

    ImGui::PushID(i);
    
    float target_indent = current ? 15.0f : 0.0f;
    float indent = iam_tween_float(ImGui::GetID("arc_item"), ImGui::GetID("indent_channel"), target_indent, 0.3f, iam_ease_preset(iam_ease_out_cubic), iam_policy_crossfade, ImGui::GetIO().DeltaTime);
    
    if (indent > 0.1f) ImGui::Indent(indent);

    // Make the selectable leave space for the size label, or just format the string
    char labelBuf[512];
    snprintf(labelBuf, sizeof(labelBuf), "%s", n.c_str());
    
    // We want the size to be aligned right. ImGui doesn't support right aligned text in selectable easily without tables,
    // but we can just use SameLine after.
    // To make the background span nicely and look visually readable, we give Selectable a specific width
    float availWidth = ImGui::GetContentRegionAvail().x;
    if (ImGui::Selectable(labelBuf, current, 0, ImVec2(availWidth - 80.0f, 0))) {
      if (audio)
        pendingPlay = i;
      else
        pendingLoad = i;
    }
    
    if (indent > 0.1f) ImGui::Unindent(indent);
    
    if (ImGui::IsItemHovered()) {
      const size_t nTxd =
          ClimaxEngine::RWS::FileSystem::CArchiveManager::GetInstance()
              .GetFirstArchive()
              ->TxdsFor(n)
              .size();
      ImGui::SetTooltip("%u bytes%s\n%zu matching .txd", entries[i].Size(),
                        entries[i].Stored() ? " (stored uncompressed)"
                                            : " uncompressed",
                        nTxd);
    }
    
    ImGui::SameLine(ImGui::GetWindowWidth() - 80.0f - ImGui::GetStyle().ScrollbarSize);
    ImGui::TextDisabled("%.0f KB", entries[i].Size() / 1024.0f);
    
    ImGui::PopID();
  }

  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && currentIndexInList != -1) {
      if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) && currentIndexInList > 0) {
          int prev = filteredIndices[currentIndexInList - 1];
          if (isAudioEntry(entries[prev].name)) pendingPlay = prev; else pendingLoad = prev;
      }
      if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) && currentIndexInList < (int)filteredIndices.size() - 1) {
          int next = filteredIndices[currentIndexInList + 1];
          if (isAudioEntry(entries[next].name)) pendingPlay = next; else pendingLoad = next;
      }
  }
  ImGui::EndChild();
  ImGui::End();

  // Deferred: loading rebuilds the global scene, so do it after the list is
  // done.
  if (pendingLoad >= 0)
    LoadLevelFromArc(pendingLoad);
  if (pendingPlay >= 0) {
    std::vector<uint8_t> blob;
    AudioClip clip;
    if (ClimaxEngine::RWS::FileSystem::CArchiveManager::GetInstance()
            .GetFirstArchive()
            ->Read((size_t)pendingPlay, blob) &&
        !blob.empty() && Audio::LoadBuffer(blob.data(), blob.size(), clip)) {
      clip.name = entries[(size_t)pendingPlay].name;
      PlayAudioClip(clip);
    }
  }
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
  if (!ImGui::Begin("Structure")) {
    ImGui::End();
    return;
  }

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
      for (const auto &t : g_ShoTypes)
        total += t.count;
      ImGui::TextDisabled("%zu types  |  %u objects total", g_ShoTypes.size(),
                          total);
      ImGui::Spacing();
      for (const auto &t : g_ShoTypes) {
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
      for (const auto &s : g_ShoSections) {
        // Colour-code by section type
        ImVec4 col = ImVec4(0.75f, 0.90f, 0.75f, 1.0f);
        if (s.name == "rwID_CBSP")
          col = ImVec4(0.95f, 0.50f, 0.30f, 1.0f);
        else if (s.name == "rwID_CLUMP")
          col = ImVec4(0.90f, 0.80f, 0.30f, 1.0f);
        else if (s.name.rfind("rwaID", 0) == 0)
          col = ImVec4(0.65f, 0.65f, 0.85f, 1.0f);
        else if (s.name == "rwID_POLYAREA")
          col = ImVec4(0.80f, 0.60f, 0.90f, 1.0f);

        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::Bullet();
        ImGui::SameLine(0, 4);
        ImGui::Text("%-26s", s.name.c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextDisabled("@ 0x%05X  (%u B)", s.offset, s.size);
      }
      // Collision summary
      if (GpuPeek(g_Collision)) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.15f, 0.95f, 0.30f, 1.0f),
                           "  Collision: %zu verts  %zu tris",
                           g_Collision.verts.size(),
                           g_Collision.indices.size() / 3);
      }
      if (!g_GameObjects.empty()) {
        size_t placed = 0;
        for (const auto &go : g_GameObjects)
          if (!go.atOrigin)
            placed++;
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.47f, 0.85f, 1.0f, 1.0f),
                           "  Game objects: %zu  (%zu placed)",
                           g_GameObjects.size(), placed);
        ImGui::Indent();
        for (size_t i = 0; i < g_GameObjects.size(); i++) {
          auto &go = g_GameObjects[i];
          if (go.atOrigin)
            ImGui::TextDisabled("[%2zu] %-26s  (logical)", i,
                                go.className.c_str());
          else
            ImGui::TextColored(ImVec4(0.72f, 0.90f, 1.0f, 1.0f),
                               "[%2zu] %-26s  (%.2f, %.2f, %.2f)", i,
                               go.className.c_str(), go.position.x,
                               go.position.y, go.position.z);
          if (ImGui::IsItemHovered() && !go.instName.empty())
            ImGui::SetTooltip("%s\n@ 0x%05X", go.instName.c_str(), go.offset);

          if (!go.clipSectionIndices.empty()) {
            ImGui::Indent();
            ImGui::PushID((int)i);
            ImGui::TextDisabled("Anim:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(150.0f);
            if (ImGui::BeginCombo(
                    "##clip",
                    go.currentClipIndex >= 0
                        ? g_ShoSections
                              [go.clipSectionIndices[go.currentClipIndex]]
                                  .name.c_str()
                        : "None")) {
              if (ImGui::Selectable("None", go.currentClipIndex == -1))
                go.currentClipIndex = -1;
              for (size_t j = 0; j < go.clipSectionIndices.size(); j++) {
                if (ImGui::Selectable(
                        g_ShoSections[go.clipSectionIndices[j]].name.c_str(),
                        go.currentClipIndex == (int)j))
                  go.currentClipIndex = (int)j;
              }
              ImGui::EndCombo();
            }
            if (go.currentClipIndex >= 0) {
              ImGui::SameLine();
              float dur =
                  g_ShoSections[go.clipSectionIndices[go.currentClipIndex]]
                      .animClip.duration;
              ImGui::SetNextItemWidth(100.0f);
              ImGui::SliderFloat("##time", &go.animTime, 0.0f, dur, "%.2fs");
            }
            ImGui::PopID();
            ImGui::Unindent();
          }
        }
        ImGui::Unindent();
      }
      if (!g_Clumps.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.15f, 1.0f),
                           "  Clumps: %zu objects", g_Clumps.size());
        ImGui::Indent();
        for (size_t i = 0; i < g_Clumps.size(); i++) {
          const auto &cl = g_Clumps[i];
          ImGui::TextDisabled("[%zu] %-20s  (%.2f, %.2f, %.2f)", i,
                              cl.sectionName.c_str(), cl.position.x,
                              cl.position.y, cl.position.z);
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
  std::string sceneName =
      g_CurrentMeshContainer.empty()
          ? "(no level)"
          : fs::path(g_CurrentMeshContainer).filename().string();

  size_t totalMeshes = 0;
  size_t totalTris = 0;
  for (auto &obj :
       ClimaxEngine::SG::CSceneObjectRegistrar::GetInstance().GetObjects()) {
    auto meshes = obj->GetMeshes();
    totalMeshes += meshes.size();
    for (auto *ch : meshes)
      totalTris += ch->vertices.size() / 3;
  }

  ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
  bool rootOpen = ImGui::TreeNodeEx("##root",
                                    ImGuiTreeNodeFlags_SpanAvailWidth |
                                        ImGuiTreeNodeFlags_DefaultOpen,
                                    "%s", sceneName.c_str());
  if (rootOpen) {
    ImGui::SameLine();
    ImGui::TextDisabled("  %zu meshes  %zu tris", totalMeshes, totalTris);
  }

  if (rootOpen) {
    for (const auto &[texName, meshes] : g_MeshTexMap) {
      size_t triCount = 0;
      for (auto *ch : meshes)
        triCount += ch->vertices.size() / 3;

      // Resolve texture (try lowercase then uppercase)
      bool hasTex = g_TextureMap.count(texName) > 0;
      if (!hasTex) {
        std::string u = texName;
        for (auto &x : u)
          x = (char)toupper((unsigned char)x);
        hasTex = g_TextureMap.count(u) > 0;
      }

      // Thumbnail (16 px)
      GLuint thumbId = 0;
      {
        auto it = g_TexInfo.find(texName);
        if (it != g_TexInfo.end())
          thumbId = it->second.glID;
        else {
          std::string u = texName;
          for (auto &x : u)
            x = (char)toupper((unsigned char)x);
          auto it2 = g_TexInfo.find(u);
          if (it2 != g_TexInfo.end())
            thumbId = it2->second.glID;
        }
      }

      ImGui::PushID(texName.c_str());

      if (thumbId) {
        ImGui::Image((ImTextureID)(intptr_t)thumbId, ImVec2(16, 16));
        ImGui::SameLine();
      } else {
        ImGui::Dummy(ImVec2(16, 16));
        ImGui::SameLine();
      }

      if (!hasTex)
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.45f, 1.0f));
      bool nodeOpen = ImGui::TreeNodeEx(
          "##mat", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", texName.c_str());
      if (!hasTex) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), " [MISSING]");
        ImGui::PopStyleColor();
      } else {
        ImGui::SameLine();
        ImGui::TextDisabled("  %zu meshes  %zu tris", meshes.size(), triCount);
      }

      if (nodeOpen) {
        int ci = 0;
        for (auto *ch : meshes) {
          size_t t = ch->vertices.size() / 3;
          ImGui::Bullet();
          ImGui::SameLine();
          ImGui::TextDisabled("Mesh #%d   (%zu tris)", ci++, t);
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
static bool s_texFullscreen = false;
static std::string s_texFullName;
static float s_texZoom = 1.0f;

// Helper: is string all-uppercase?
static bool IsAllUpper(const std::string &s) {
  for (char c : s)
    if (c >= 'a' && c <= 'z')
      return false;
  return true;
}

void RenderTxdWindow() {
  // Anchored to the right edge of the viewport rather than to a hard-coded
  // 1280 px width, and pushed below the orbit sphere overlay it used to sit
  // under.
  const ImGuiViewport *vp = ImGui::GetMainViewport();
  const float TXD_W = 230.0f, TXD_TOP = 140.0f;
  ImGui::SetNextWindowPos(
      ImVec2(vp->Pos.x + vp->Size.x - TXD_W - 10.0f, vp->Pos.y + TXD_TOP),
      ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(
      ImVec2(TXD_W, std::max(200.0f, vp->Size.y - TXD_TOP - 10.0f)),
      ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Textures")) {
    ImGui::End();
    return;
  }

  if (g_TexInfo.empty()) {
    ImGui::TextDisabled("(no textures loaded)");
    ImGui::End();
    return;
  }

  const float THUMB = 56.0f;
  const float ITEM_H = THUMB + 6.0f;
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 4));
  ImGui::BeginChild("##txdlist", ImVec2(0, 0), false);

  for (const auto &[name, pi] : g_TexInfo) {
    // Skip upper-case aliases (duplicates of lowercase entries)
    if (IsAllUpper(name)) {
      std::string lo = name;
      for (auto &c : lo)
        c = (char)tolower((unsigned char)c);
      if (g_TexInfo.count(lo))
        continue;
    }

    ImGui::PushID(name.c_str());

    // Row background highlight on hover
    float rowY = ImGui::GetCursorPosY();
    ImVec2 rowMin = ImGui::GetCursorScreenPos();
    ImVec2 rowMax =
        ImVec2(rowMin.x + ImGui::GetContentRegionAvail().x, rowMin.y + ITEM_H);
    bool hovered = ImGui::IsMouseHoveringRect(rowMin, rowMax);
    
    float target_alpha = hovered ? 80.0f : 0.0f;
    float anim_alpha = iam_tween_float(ImGui::GetID(name.c_str()), ImGui::GetID("txd_hover"), target_alpha, 0.2f, iam_ease_preset(iam_ease_out_cubic), iam_policy_crossfade, ImGui::GetIO().DeltaTime);

    if (anim_alpha > 0.1f)
      ImGui::GetWindowDrawList()->AddRectFilled(
          rowMin, rowMax, IM_COL32(60, 100, 160, (int)anim_alpha), 4.0f);

    // Thumbnail with scale animation
    float target_scale = hovered ? 1.08f : 1.0f;
    float anim_scale = iam_tween_float(ImGui::GetID(name.c_str()), ImGui::GetID("txd_scale"), target_scale, 0.25f, iam_ease_preset(iam_ease_out_back), iam_policy_crossfade, ImGui::GetIO().DeltaTime);
    float scaled_thumb = THUMB * anim_scale;
    float offset = (scaled_thumb - THUMB) * -0.5f;
    
    ImVec2 curPos = ImGui::GetCursorPos();
    ImGui::SetCursorPos(ImVec2(curPos.x + offset, curPos.y + offset));
    ImGui::Image((ImTextureID)(intptr_t)pi.glID, ImVec2(scaled_thumb, scaled_thumb));
    ImGui::SetCursorPos(ImVec2(curPos.x, curPos.y)); // Restore X/Y for layout
    
    ImGui::SameLine(THUMB + 8.0f);

    // Text info column
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() +
                           ImGui::GetContentRegionAvail().x);
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
        s_texFullName = name;
        s_texZoom = 1.0f;
      }
    }

    ImGui::PopID();
  }

  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::End();

  // ---- Fullscreen texture viewer ----
  if (!s_texFullscreen)
    return;

  ImGuiIO &io = ImGui::GetIO();
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(io.DisplaySize);
  ImGui::SetNextWindowBgAlpha(0.93f);
  ImGui::Begin("##texfull", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoScrollbar);

  // Top bar
  if (ImGui::Button("  X  ") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    s_texFullscreen = false;
    ImGui::End();
    return;
  }
  ImGui::SameLine();
  ImGui::TextColored(ImVec4(0.8f, 0.9f, 1.0f, 1.0f), "%s",
                     s_texFullName.c_str());
  if (g_TexInfo.count(s_texFullName)) {
    auto &pi = g_TexInfo[s_texFullName];
    ImGui::SameLine();
    ImGui::TextDisabled("  %d x %d  %dbit", pi.width, pi.height, pi.depth);
  }
  ImGui::SameLine(ImGui::GetWindowWidth() - 220.0f);
  if (ImGui::Button(" - ##z"))
    s_texZoom = std::max(s_texZoom * 0.8f, 0.05f);
  ImGui::SameLine();
  if (ImGui::Button(" + ##z"))
    s_texZoom = std::min(s_texZoom * 1.25f, 16.0f);
  ImGui::SameLine();
  if (ImGui::Button("1:1"))
    s_texZoom = 1.0f;
  ImGui::SameLine();
  if (ImGui::Button("Fit") && g_TexInfo.count(s_texFullName)) {
    auto &pi = g_TexInfo[s_texFullName];
    float fw = io.DisplaySize.x - 24;
    float fh = io.DisplaySize.y - 48;
    s_texZoom =
        std::min(fw / std::max(pi.width, 1), fh / std::max(pi.height, 1));
  }

  // Scroll-to-zoom
  if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
      io.MouseWheel != 0.0f)
    s_texZoom =
        std::clamp(s_texZoom * (io.MouseWheel > 0 ? 1.1f : 0.9f), 0.05f, 16.0f);

  ImGui::Separator();

  if (g_TexInfo.count(s_texFullName)) {
    auto &pi = g_TexInfo[s_texFullName];
    float tw = pi.width * s_texZoom;
    float th = pi.height * s_texZoom;
    ImGui::BeginChild("##imgview", ImVec2(0, 0), false,
                      ImGuiWindowFlags_HorizontalScrollbar |
                          ImGuiWindowFlags_AlwaysVerticalScrollbar);
    // Center small images
    float offX = std::max(0.0f, (ImGui::GetContentRegionAvail().x - tw) * 0.5f);
    float offY = std::max(0.0f, (ImGui::GetContentRegionAvail().y - th) * 0.5f);
    if (offX > 0 || offY > 0)
      ImGui::SetCursorPos(ImVec2(offX, offY));
    ImGui::Image((ImTextureID)(intptr_t)pi.glID, ImVec2(tw, th));
    ImGui::EndChild();
  }
  ImGui::End();
}

// One tab of the Playback panel. Sound, skeletal animation and UV animation are
// three views of the same question -- what is this level playing right now --
// so they share a window instead of scattering three of them across the screen.
// Transport buttons, drawn rather than spelled.
//
// They used to be the ASCII stand-ins "|<", "||", "[]" and ">|", which read as
// punctuation until you worked out what they meant. These are vector glyphs on
// an invisible button, with the hover and press states tweened through ImAnim
// so the control answers the pointer instead of snapping.
enum class Transport { Prev, Play, Pause, Stop, Next };

static bool TransportButton(const char *id, Transport kind, float size,
                            bool enabled = true) {
  ImGui::PushID(id);
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const bool pressed = ImGui::InvisibleButton("##t", ImVec2(size, size)) && enabled;
  const bool hovered = ImGui::IsItemHovered() && enabled;
  const bool held    = ImGui::IsItemActive() && enabled;

  const float dt = ImGui::GetIO().DeltaTime;
  const ImGuiID gid = ImGui::GetID("##t");
  const float lift = iam_tween_float(gid, ImGui::GetID("lift"),
                                     held ? 1.0f : (hovered ? 0.6f : 0.0f), 0.16f,
                                     iam_ease_preset(iam_ease_out_cubic),
                                     iam_policy_crossfade, dt);

  const ImVec4 base = ImGui::GetStyleColorVec4(ImGuiCol_Button);
  const ImVec4 warm = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
  const ImVec4 bg(base.x + (warm.x - base.x) * lift,
                  base.y + (warm.y - base.y) * lift,
                  base.z + (warm.z - base.z) * lift, enabled ? 1.0f : 0.35f);
  const ImU32 fg = ImGui::GetColorU32(
      enabled ? ImVec4(0.92f, 0.92f, 0.94f, 1.0f) : ImVec4(0.5f, 0.5f, 0.5f, 1.0f));

  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImVec2 c(p0.x + size * 0.5f, p0.y + size * 0.5f);
  dl->AddCircleFilled(c, size * 0.5f, ImGui::GetColorU32(bg), 32);

  const float r = size * (0.26f + 0.02f * lift);   // glyph grows a hair on hover
  const float b = r * 0.34f;                       // bar thickness
  switch (kind) {
  case Transport::Play:
    dl->AddTriangleFilled(ImVec2(c.x - r * 0.55f, c.y - r),
                          ImVec2(c.x - r * 0.55f, c.y + r),
                          ImVec2(c.x + r * 0.85f, c.y), fg);
    break;
  case Transport::Pause:
    dl->AddRectFilled(ImVec2(c.x - r * 0.62f, c.y - r), ImVec2(c.x - r * 0.62f + b, c.y + r), fg, 1.0f);
    dl->AddRectFilled(ImVec2(c.x + r * 0.62f - b, c.y - r), ImVec2(c.x + r * 0.62f, c.y + r), fg, 1.0f);
    break;
  case Transport::Stop:
    dl->AddRectFilled(ImVec2(c.x - r * 0.8f, c.y - r * 0.8f),
                      ImVec2(c.x + r * 0.8f, c.y + r * 0.8f), fg, 2.0f);
    break;
  case Transport::Prev:
    dl->AddRectFilled(ImVec2(c.x - r, c.y - r), ImVec2(c.x - r + b, c.y + r), fg, 1.0f);
    dl->AddTriangleFilled(ImVec2(c.x + r, c.y - r), ImVec2(c.x + r, c.y + r),
                          ImVec2(c.x - r + b * 1.4f, c.y), fg);
    break;
  case Transport::Next:
    dl->AddRectFilled(ImVec2(c.x + r - b, c.y - r), ImVec2(c.x + r, c.y + r), fg, 1.0f);
    dl->AddTriangleFilled(ImVec2(c.x - r, c.y - r), ImVec2(c.x - r, c.y + r),
                          ImVec2(c.x + r - b * 1.4f, c.y), fg);
    break;
  }
  ImGui::PopID();
  return pressed;
}

static void AudioTabBody() {
  auto timecode = [](float sec) {
    const int t = (int)sec;
    char buf[32];
    snprintf(buf, sizeof(buf), "%d:%02d", t / 60, t % 60);
    return std::string(buf);
  };

  {
    const AudioClip &cur = CurrentAudioClip();

    // ── Now playing ────────────────────────────────────────────────
    if (cur.Valid()) {
      ImGui::TextUnformatted(cur.name.empty() ? "(unnamed)" : cur.name.c_str());
      ImGui::TextDisabled("%s  %d Hz  %s  %s", cur.codec.c_str(),
                          cur.sampleRate, cur.channels == 2 ? "stereo" : "mono",
                          timecode(cur.Seconds()).c_str());
      if (!cur.source.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("| %s", cur.source.c_str());
      }
    } else {
      ImGui::TextDisabled("Nothing loaded — pick a sound below.");
    }

    ImGui::Separator();

    // ── Transport ──────────────────────────────────────────────────
    ImGui::BeginDisabled(!cur.Valid());
    
    ImGui::Spacing();
    
    // Progress bar with time text centered
    const float elapsed = cur.Valid() ? cur.Seconds() * state.audioProgress : 0.0f;
    char progText[64];
    snprintf(progText, sizeof(progText), "%s / %s", timecode(elapsed).c_str(), timecode(cur.Seconds()).c_str());
    
    ImGui::PushItemWidth(-1);
    float prog = state.audioProgress;
    if (ImGui::SliderFloat("##progress", &prog, 0.0f, 1.0f, progText))
      SetAudioProgress(prog);
    ImGui::PopItemWidth();
    
    ImGui::Spacing();
    
    // Centered transport controls
    float btnSize = 32.0f;
    float space = ImGui::GetStyle().ItemSpacing.x;
    float totalWidth = (btnSize * 4) + (space * 3) + 70.0f; // 70 for Loop checkbox
    ImGui::SetCursorPosX((ImGui::GetWindowSize().x - totalWidth) * 0.5f);

    auto playOffset = [&](int dir) {
        if (!cur.Valid()) return;
        for (size_t i = 0; i < g_Sounds.size(); ++i) {
            if (g_Sounds[i].name == cur.name) {
                int nextIdx = (int)i + dir;
                if (nextIdx >= 0 && nextIdx < (int)g_Sounds.size()) {
                    state.audioSelected = nextIdx;
                    PlayAudioClip(g_Sounds[nextIdx]);
                }
                return;
            }
        }
        for (size_t i = 0; i < g_AudioLibrary.size(); ++i) {
            if (g_AudioLibrary[i].name == cur.name) {
                int nextIdx = (int)i + dir;
                while (nextIdx >= 0 && nextIdx < (int)g_AudioLibrary.size()) {
                    if (g_AudioLibrary[nextIdx].group == g_AudioLibrary[i].group) {
                        PlayLibraryEntry(nextIdx);
                        return;
                    }
                    nextIdx += dir;
                }
                return;
            }
        }
    };

    if (TransportButton("prev", Transport::Prev, btnSize, cur.Valid())) playOffset(-1);
    ImGui::SameLine();
    if (TransportButton("play",
                        state.isAudioPlaying ? Transport::Pause : Transport::Play,
                        btnSize, cur.Valid()))
      ToggleAudioPlayback();
    ImGui::SameLine();
    if (TransportButton("stop", Transport::Stop, btnSize, cur.Valid())) StopAudio();
    ImGui::SameLine();
    if (TransportButton("next", Transport::Next, btnSize, cur.Valid())) playOffset(1);
    ImGui::SameLine();
    
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (btnSize - ImGui::GetFrameHeight()) * 0.5f);
    ImGui::Checkbox("Loop", &state.audioLoop);
    
    ImGui::Spacing();
    
    // Bottom row: Volume and Save WAV
    ImGui::SetNextItemWidth(120.0f);
    ImGui::SliderFloat("Volume", &state.audioVolume, 0.0f, 1.5f, "%.2f");
    
    static float lastExportTime = -10.0f;
    bool recentlySaved = ((float)ImGui::GetTime() - lastExportTime) < 2.0f;

    ImGui::SameLine(ImGui::GetWindowSize().x - 90.0f);
    if (recentlySaved) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.7f, 0.2f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.8f, 0.2f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.2f, 0.6f, 0.2f, 1.0f));
    }
    
    if (ImGui::Button(recentlySaved ? "Saved!###savewav" : "Save WAV###savewav", ImVec2(80, 0))) {
      std::string name = cur.name.empty() ? std::string("sound") : cur.name;
      for (auto &c : name)
        if (c == '/' || c == '\\' || c == ':')
          c = '_';
      if (Audio::WriteWav(name + ".wav", cur)) {
        std::cout << "[audio] wrote " << name << ".wav\n";
        lastExportTime = (float)ImGui::GetTime();
      }
    }
    
    if (recentlySaved) {
        ImGui::PopStyleColor(3);
    }

    ImGui::EndDisabled();

    {
      int calls = 0, late = 0;
      double worst = 0.0, buf = 0.0;
      AudioHealth(calls, late, worst, buf);
      if (calls > 0) {
        ImGui::SameLine();
        if (late > 0)
          ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f),
                             "%d/%d late, worst %.0f ms (buffer %.0f)", late,
                             calls, worst, buf);
        else
          ImGui::TextDisabled("%d buffers, worst gap %.0f ms of %.0f", calls,
                              worst, buf);
      }
    }

    ImGui::Separator();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##audiofilter", "filter by name",
                             state.audioFilter, sizeof(state.audioFilter));
    const bool filtering = state.audioFilter[0] != '\0';
    auto matches = [&](const std::string &n) {
      if (!filtering)
        return true;
      // Case-insensitive substring; the bank names are lower case but
      // the filter box is whatever the user typed.
      std::string hay = n, needle = state.audioFilter;
      for (auto &c : hay)
        c = (char)tolower((unsigned char)c);
      for (auto &c : needle)
        c = (char)tolower((unsigned char)c);
      return hay.find(needle) != std::string::npos;
    };

    if (ImGui::BeginTabBar("##audiotabs")) {
      // Level sound bank — decoded with the container, so it is already
      // in memory and every entry can show its length.
      if (ImGui::BeginTabItem("Level")) {
        ImGui::BeginChild("##lvl", ImVec2(0, 0));
        if (g_Sounds.empty()) {
          ImGui::TextWrapped(
              "No sounds. A level keeps its own bank in an "
              "rwaID_WAVEDICT section inside the container; load a "
              "level from the Archive panel to fill this list.");
        } else {
          for (size_t i = 0; i < g_Sounds.size(); ++i) {
            const AudioClip &c = g_Sounds[i];
            if (!matches(c.name))
              continue;
            ImGui::PushID((int)i);
            const bool sel = state.audioSelected == (int)i;
            if (ImGui::Selectable(c.name.c_str(), sel,
                                  ImGuiSelectableFlags_AllowDoubleClick)) {
              state.audioSelected = (int)i;
              PlayAudioClip(c);
            }
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 96.0f);
            ImGui::TextDisabled("%d Hz  %s", c.sampleRate,
                                timecode(c.Seconds()).c_str());
            ImGui::PopID();
          }
        }
        ImGui::EndChild();
        ImGui::EndTabItem();
      }

      // Music and cutscenes live outside the archive and are decoded on
      // demand, so these lists carry names only. Both tabs stay visible
      // even when empty -- otherwise a failed scan looks the same as a
      // game folder that simply has no music.
      for (const char *group : {"Music", "Cutscenes"}) {
        size_t count = 0;
        for (const auto &r : g_AudioLibrary)
          if (r.group == group)
            count++;
        if (!ImGui::BeginTabItem(group))
          continue;
        ImGui::BeginChild(group, ImVec2(0, 0));
        if (!count) {
          const bool isMusic = std::string(group) == "Music";
          ImGui::TextWrapped(
              "Nothing found. %s has to sit in the same folder as the "
              "archive you mounted.",
              isMusic ? "A MUSIC folder" : "IGC.ARC");
          if (ClimaxEngine::RWS::FileSystem::CArchiveManager::GetInstance()
                  .GetFirstArchive()) {
            ImGui::Spacing();
            ImGui::TextDisabled("Looked next to:");
            ImGui::TextWrapped(
                "%s",
                DisplayPath(ClimaxEngine::RWS::FileSystem::CArchiveManager::
                                GetInstance()
                                    .GetFirstArchive()
                                    ->Path())
                    .c_str());
          } else {
            ImGui::Spacing();
            ImGui::TextDisabled("No archive is mounted yet.");
          }
        }
        for (size_t i = 0; i < g_AudioLibrary.size(); ++i) {
          const AudioSourceRef &r = g_AudioLibrary[i];
          if (r.group != group || !matches(r.name))
            continue;
          ImGui::PushID(1000 + (int)i);
          if (ImGui::Selectable(r.name.c_str(), cur.name == r.name))
            PlayLibraryEntry((int)i);
          ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }
  }
}

static void SkeletalTabBody() {
  if (g_AnimClips.empty()) {
    ImGui::TextDisabled("This container carries no skeletal clips.");
    ImGui::TextDisabled("Open a character - CPlayerBehaviour.Travis has 147.");
    return;
  }

  std::vector<std::shared_ptr<ClimaxEngine::SG::CClumpObject>> clumps;
  for (auto &o : ClimaxEngine::SG::CSceneObjectRegistrar::GetInstance().GetObjects())
    if (auto c = std::dynamic_pointer_cast<ClimaxEngine::SG::CClumpObject>(o))
      if (!c->skeleton.bones.empty()) clumps.push_back(c);

  ImGui::Text("%d clips", (int)g_AnimClips.size());
  ImGui::SameLine();
  ImGui::TextDisabled("| %d skeletons", (int)clumps.size());

  if (clumps.empty()) {
    ImGui::TextDisabled("Nothing in the scene has a skeleton to drive.");
    return;
  }

  ImGui::Separator();

  const int cur = state.animClipIndex;
  const std::string label =
      (cur >= 0 && cur < (int)g_AnimClips.size())
          ? g_AnimClips[(size_t)cur].name + "  (" +
                std::to_string(g_AnimClips[(size_t)cur].tracks.size()) + " tracks, " +
                std::to_string((int)(g_AnimClips[(size_t)cur].duration * 1000) / 1000.0f).substr(0, 4) + "s)"
          : std::string("Rest pose");

  ImGui::SetNextItemWidth(-1.0f);
  if (ImGui::BeginCombo("##clip", label.c_str())) {
    if (ImGui::Selectable("Rest pose", cur < 0)) state.animClipIndex = -1;
    for (size_t i = 0; i < g_AnimClips.size(); i++) {
      char buf[96];
      snprintf(buf, sizeof(buf), "%s   %zu tracks   %.2fs", g_AnimClips[i].name.c_str(),
               g_AnimClips[i].tracks.size(), g_AnimClips[i].duration);
      if (ImGui::Selectable(buf, cur == (int)i)) {
        state.animClipIndex = (int)i;
        for (auto &c : clumps) c->animTime = 0.0f;
      }
    }
    ImGui::EndCombo();
  }

  ImGui::Spacing();

  const bool playing = state.animSpeed > 0.0f;
  if (TransportButton("aprev", Transport::Prev, 32.0f, cur > 0)) {
    state.animClipIndex = cur - 1;
    for (auto &c : clumps) c->animTime = 0.0f;
  }
  ImGui::SameLine();
  if (TransportButton("aplay", playing ? Transport::Pause : Transport::Play, 32.0f))
    state.animSpeed = playing ? 0.0f : 1.0f;
  ImGui::SameLine();
  if (TransportButton("astop", Transport::Stop, 32.0f)) {
    state.animSpeed = 0.0f;
    for (auto &c : clumps) c->animTime = 0.0f;
  }
  ImGui::SameLine();
  if (TransportButton("anext", Transport::Next, 32.0f,
                      cur + 1 < (int)g_AnimClips.size())) {
    state.animClipIndex = cur + 1;
    for (auto &c : clumps) c->animTime = 0.0f;
  }
  ImGui::SameLine();
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6.0f);
  ImGui::Checkbox("Rest", &state.animRestPose);

  ImGui::Spacing();

  if (cur >= 0 && cur < (int)g_AnimClips.size()) {
    const float dur = g_AnimClips[(size_t)cur].duration;
    float t = clumps.front()->animTime;
    if (dur > 0.0f) t = std::fmod(t, dur);
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::SliderFloat("##atime", &t, 0.0f, dur > 0.0f ? dur : 1.0f, "%.2f s"))
      for (auto &c : clumps) c->animTime = t;
  }

  ImGui::SetNextItemWidth(-60.0f);
  ImGui::SliderFloat("##aspeed", &state.animSpeed, 0.0f, 3.0f, "%.2fx");
  ImGui::SameLine(); ImGui::TextDisabled("Speed");

  ImGui::Checkbox("Bone overlay", &state.showBoneOverlay);
  ImGui::SameLine();
  ImGui::Checkbox("Skinning", &state.animSkinning);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Experimental. The four weights per vertex are read, but\n"
                      "the bone-slot mapping is not settled: turning this on\n"
                      "tears the model apart. Off means every piece follows its\n"
                      "frame, which is correct for the rigid ones.");

  ImGui::Spacing();
  ImGui::TextDisabled("PS2 characters are segmented, not vertex-skinned:");
  ImGui::TextDisabled("each piece follows one frame of the hierarchy.");
}

// UV animation needs no transport of its own -- it is driven by the same clock
// the scene is, and every material that names a clip runs it. What this tab is
// for is seeing which clips a level carries and what they actually do, which is
// the only way to tell a stalled clip from one that is merely slow.
static void UVAnimTabBody() {
  if (g_UVAnims.empty()) {
    ImGui::TextDisabled("This level carries no UV animations.");
    ImGui::TextDisabled("They live in the container's 0x2B sections; fire and");
    ImGui::TextDisabled("torches are the usual carriers.");
    return;
  }

  ImGui::Text("%d clips", (int)g_UVAnims.size());
  ImGui::SameLine();
  ImGui::TextDisabled("| driven by the scene clock");

  ImGui::Checkbox("Run", &state.uvAnimRun);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(-90.0f);
  ImGui::SliderFloat("Speed", &state.uvAnimSpeed, 0.0f, 4.0f, "%.2fx");

  ImGui::Separator();

  static std::string sel;
  if (sel.empty() || !g_UVAnims.count(sel)) sel = g_UVAnims.begin()->first;

  ImGui::SetNextItemWidth(-1.0f);
  if (ImGui::BeginCombo("##uvclip", sel.c_str())) {
    for (auto &kv : g_UVAnims)
      if (ImGui::Selectable(kv.first.c_str(), kv.first == sel)) sel = kv.first;
    ImGui::EndCombo();
  }

  const UVAnimClip &clip = g_UVAnims[sel];
  ImGui::TextDisabled("%.2f s   %d layer%s", clip.duration,
                      (int)clip.layers.size(),
                      clip.layers.size() == 1 ? "" : "s");

  const float t = clip.duration > 0.0f
                      ? std::fmod(state.uvAnimTime, clip.duration)
                      : 0.0f;
  float shown = t;
  ImGui::SetNextItemWidth(-1.0f);
  ImGui::SliderFloat("##uvtime", &shown, 0.0f,
                     clip.duration > 0.0f ? clip.duration : 1.0f, "%.2f s");

  ImGui::Spacing();
  if (ImGui::BeginTable("uvlayers", 5,
                        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("Layer");
    ImGui::TableSetupColumn("Keys");
    ImGui::TableSetupColumn("Scale");
    ImGui::TableSetupColumn("Offset now");
    ImGui::TableSetupColumn("Drift / s");
    ImGui::TableHeadersRow();

    for (size_t L = 0; L < clip.layers.size(); L++) {
      const auto &k = clip.layers[L];
      if (k.empty()) continue;
      // Where the layer sits at the current time, by the same linear walk the
      // renderer does.
      size_t i = 0;
      while (i + 1 < k.size() && k[i + 1].time <= t) i++;
      const size_t j = std::min(i + 1, k.size() - 1);
      const float span = k[j].time - k[i].time;
      const float a = span > 1e-6f ? (t - k[i].time) / span : 0.0f;
      const float uo = k[i].uOff + (k[j].uOff - k[i].uOff) * a;
      const float vo = k[i].vOff + (k[j].vOff - k[i].vOff) * a;
      const float du = clip.duration > 0.0f
                           ? (k.back().uOff - k.front().uOff) / clip.duration
                           : 0.0f;
      const float dv = clip.duration > 0.0f
                           ? (k.back().vOff - k.front().vOff) / clip.duration
                           : 0.0f;

      ImGui::TableNextRow();
      ImGui::TableNextColumn(); ImGui::Text("%d", (int)L);
      ImGui::TableNextColumn(); ImGui::Text("%d", (int)k.size());
      ImGui::TableNextColumn();
      ImGui::Text("%.2f, %.2f", k[i].uScale, k[i].vScale);
      ImGui::TableNextColumn(); ImGui::Text("%.2f, %.2f", uo, vo);
      ImGui::TableNextColumn(); ImGui::Text("%.2f, %.2f", du, dv);
    }
    ImGui::EndTable();
  }

  if (clip.layers.size() > 1)
    ImGui::TextDisabled("Layer 1 is not drawn yet -- see TODO.md.");
}

void RenderPlaybackPanel() {
  if (!state.showAudioPlayer)
    return;

  ImGui::SetNextWindowSize(ImVec2(460, 500), ImGuiCond_FirstUseEver);
  if (ImGui::Begin("Playback", &state.showAudioPlayer)) {
    if (ImGui::BeginTabBar("##playback")) {
      if (ImGui::BeginTabItem("Sound")) {
        AudioTabBody();
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Skeletal")) {
        SkeletalTabBody();
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("UV")) {
        UVAnimTabBody();
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }
  }
  ImGui::End();
}
