#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// The boot sequence and the menus.
//
// Game logic: no GL, no SDL, no ImGui. It decides *what* is on screen and what
// a keypress does; drawing it is the renderer's job.
//
// The screens themselves are data -- 40 XML files in SH.ARC, parsed by
// Core/UI/ScreenDef.h -- so this holds only what the data does not: the order
// of the boot stages, and how a button press moves between elements.
//
// Where the order came from: Origins' executable carries the strings `Logo`,
// `Title` and `BootMenu`, and Ghost Rider -- unstripped -- has an object called
// `..._Objects_FrontEnd_BootSequence` alongside `Warnings`,
// `LanguageSelectionScreen` and `AttractMode`. Origins' archive holds 88 object
// classes and none of them are front-end, so in this game the sequence is code
// and this is a reconstruction of it, not a reading.
// ─────────────────────────────────────────────────────────────────────────────

#include <string>
#include <vector>

#include "ClimaxEngine/Core/UI/ScreenDef.h"

namespace ClimaxEngine {
namespace Game {

enum class BootStage {
    Logo,            // publisher and developer idents
    Warning,         // the health and epilepsy notice
    LanguageSelect,  // skipped when the language is already chosen
    MainMenu,
    InGame,
};

const char *BootStageName(BootStage s);

// What the player is doing this frame. Deliberately not SDL keycodes.
struct MenuInput {
    bool up = false;
    bool down = false;
    bool left = false;
    bool right = false;
    bool accept = false;
    bool cancel = false;
    bool anyKey = false;   // dismisses a timed screen early
};

// Which screen is up and which element is highlighted.
//
// Navigation comes from the XML: a BUTTON names `onup`, `ondown`, `onaccept`
// and `oncancel`, each holding either another element's id on the same screen
// or the id of a screen to open. Which of the two it is cannot be told from the
// attribute, so the resolution order is: an element on this screen first, a
// screen second. That is the only reading under which `mainmenu.xml` works --
// `ondown="main_menu_load_game"` is a sibling, `onaccept="new_game_menu_screen"`
// is not.
class MenuState {
public:
    // `screens` maps a screen id to its parsed <SCREEN> element. The pointers
    // must outlive this object.
    void SetScreens(std::vector<std::pair<std::string, const UI::Element *>> screens);

    // Opens a screen by id. False when there is no such screen.
    bool Open(const std::string &screenId);

    const std::string &ScreenId() const { return m_screenId; }
    const UI::Element *Screen() const { return m_screen; }
    const std::string &ActiveId() const { return m_activeId; }
    const UI::Element *Active() const;

    // Applies one frame of input. Returns the id of whatever was accepted and
    // could not be resolved -- a command for the caller to act on, such as
    // starting a game -- or an empty string.
    std::string Update(const MenuInput &in);

private:
    const UI::Element *FindScreen(const std::string &id) const;
    void SelectDefault();

    std::vector<std::pair<std::string, const UI::Element *>> m_screens;
    std::string m_screenId;
    const UI::Element *m_screen = nullptr;
    std::string m_activeId;
    std::vector<std::string> m_back;   // screen stack, for cancel
};

// Drives the stages before the menu, and the menu after them.
class FrontEnd {
public:
    // Seconds each timed stage holds before moving on. The retail timings are
    // not in the data; these are ordinary values, and any of them can be cut
    // short by a keypress, which is what the original does.
    float logoSeconds = 4.0f;
    float warningSeconds = 6.0f;

    // Set false to make the language stage appear; the game shows it once, on
    // first boot, and remembers the answer.
    bool languageChosen = false;

    BootStage Stage() const { return m_stage; }
    MenuState &Menu() { return m_menu; }

    // Restart from the first stage.
    void Reset();

    // One frame. Returns a command the caller must act on -- "start a new
    // game", "load" -- or an empty string. Stage changes are not commands;
    // read Stage() for those.
    std::string Update(float dt, const MenuInput &in);

private:
    BootStage m_stage = BootStage::Logo;
    float m_elapsed = 0.0f;
    MenuState m_menu;

    void Enter(BootStage s);
};

} // namespace Game
} // namespace ClimaxEngine
