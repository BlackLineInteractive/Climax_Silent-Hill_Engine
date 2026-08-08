#include "ClimaxEngine/Game/FrontEnd.h"

namespace ClimaxEngine {
namespace Game {

const char *LanguageFlag(Language l) {
    switch (l) {
    case Language::English_GB: return "sho_flg_GB";
    case Language::English_US: return "sho_flg_US";
    case Language::French:     return "sho_flg_FR";
    case Language::German:     return "sho_flg_DE";
    case Language::Italian:    return "sho_flg_IT";
    case Language::Spanish:    return "sho_flg_ES";
    }
    return "sho_flg_GB";
}

const char *LanguageStrings(Language l) {
    switch (l) {
    case Language::English_GB:
    case Language::English_US: return "Strings.Eng";
    case Language::French:     return "Strings.Fre";
    case Language::German:     return "Strings.Ger";
    case Language::Italian:    return "Strings.Ita";
    case Language::Spanish:    return "Strings.Spa";
    }
    return "Strings.Eng";
}

int LanguageCount() { return 6; }

const char *BootStageName(BootStage s) {
    switch (s) {
    case BootStage::Logo: return "Logo";
    case BootStage::AspectSelect: return "AspectSelect";
    case BootStage::LanguageSelect: return "LanguageSelect";
    case BootStage::MainMenu: return "MainMenu";
    case BootStage::InGame: return "InGame";
    }
    return "?";
}

// ── MenuState ────────────────────────────────────────────────────────────────

void MenuState::SetScreens(
    std::vector<std::pair<std::string, const UI::Element *>> screens) {
    m_screens = std::move(screens);
    m_screenId.clear();
    m_screen = nullptr;
    m_activeId.clear();
    m_back.clear();
}

const UI::Element *MenuState::FindScreen(const std::string &id) const {
    for (const auto &[name, el] : m_screens)
        if (name == id)
            return el;
    // The XML names screens both by file ("mainmenu") and by the id on the
    // <SCREEN> element ("main_menu_screen"), and `onaccept` uses the second.
    for (const auto &[name, el] : m_screens)
        if (el && el->Attr("id") == id)
            return el;
    return nullptr;
}

bool MenuState::Open(const std::string &screenId) {
    const UI::Element *s = FindScreen(screenId);
    if (!s)
        return false;
    m_screenId = screenId;
    m_screen = s;
    SelectDefault();
    return true;
}

void MenuState::SelectDefault() {
    m_activeId.clear();
    if (!m_screen)
        return;
    // `default_active="true"` marks the button the screen opens on; failing
    // that, the first child that can take focus.
    for (const UI::Element &c : m_screen->children)
        if (c.Bool("default_active")) {
            m_activeId = c.Attr("id");
            return;
        }
    for (const UI::Element &c : m_screen->children)
        if (c.tag == "BUTTON" || c.tag == "TOGGLEBUTTON" || c.tag == "SLIDERBUTTON") {
            m_activeId = c.Attr("id");
            return;
        }
}

const UI::Element *MenuState::Active() const {
    if (!m_screen || m_activeId.empty())
        return nullptr;
    for (const UI::Element &c : m_screen->children)
        if (c.Attr("id") == m_activeId)
            return &c;
    return nullptr;
}

std::string MenuState::Update(const MenuInput &in) {
    const UI::Element *cur = Active();
    if (!cur)
        return {};

    // Movement: the named element has to exist on this screen, or the press
    // does nothing. A missing target is a dead end in the data, not a screen
    // change -- treating it as one would open the main menu from a stray typo.
    auto move = [&](const char *attr) {
        const std::string *to = cur->Get(attr);
        if (!to || to->empty())
            return;
        for (const UI::Element &c : m_screen->children)
            if (c.Attr("id") == *to) {
                m_activeId = *to;
                return;
            }
    };

    if (in.up) move("onup");
    else if (in.down) move("ondown");
    else if (in.left) move("onleft");
    else if (in.right) move("onright");

    if (in.cancel) {
        const std::string *to = cur->Get("oncancel");
        if (to && !to->empty() && Open(*to))
            return {};
        if (!m_back.empty()) {
            const std::string prev = m_back.back();
            m_back.pop_back();
            m_screenId = prev;
            m_screen = FindScreen(prev);
            SelectDefault();
        }
        return {};
    }

    if (in.accept) {
        cur = Active();   // movement above may have changed it
        const std::string *to = cur ? cur->Get("onaccept") : nullptr;
        if (!to || to->empty())
            return {};
        // A sibling first, then a screen, then it is a command for the caller.
        for (const UI::Element &c : m_screen->children)
            if (c.Attr("id") == *to) {
                m_activeId = *to;
                return {};
            }
        if (FindScreen(*to)) {
            m_back.push_back(m_screenId);
            Open(*to);
            return {};
        }
        return *to;
    }
    return {};
}

// ── FrontEnd ─────────────────────────────────────────────────────────────────

void FrontEnd::Reset() {
    m_stage = BootStage::Logo;
    m_elapsed = 0.0f;
}

void FrontEnd::Enter(BootStage s) {
    m_stage = s;
    m_elapsed = 0.0f;
    if (s == BootStage::MainMenu)
        m_menu.Open("mainmenu");
}

std::string FrontEnd::Update(float dt, const MenuInput &in) {
    m_elapsed += dt;

    switch (m_stage) {
    case BootStage::Logo:
        // Advances when the LOGOW/LOGON clip actually ends, or -- once it has
        // had a moment to be seen -- on a keypress, the same as the original.
        // `logoSeconds` only fires if no video is playing at all, so the boot
        // sequence can never hang on a missing asset.
        if (in.mediaEnded || m_elapsed >= logoSeconds ||
            ((in.anyKey || in.accept) && m_elapsed > 1.0f)) {
            if (!aspectChosen) Enter(BootStage::AspectSelect);
            else if (!languageChosen) Enter(BootStage::LanguageSelect);
            else Enter(BootStage::MainMenu);
        }
        return {};

    case BootStage::AspectSelect:
        if (in.left) widescreen = false;
        if (in.right) widescreen = true;
        if (in.accept) {
            aspectChosen = true;
            Enter(languageChosen ? BootStage::MainMenu : BootStage::LanguageSelect);
        }
        return {};

    case BootStage::LanguageSelect: {
        const int n = LanguageCount();
        if (in.left)  languageIndex = (languageIndex + n - 1) % n;
        if (in.right) languageIndex = (languageIndex + 1) % n;
        language = (Language)languageIndex;
        if (in.accept) {
            languageChosen = true;
            Enter(BootStage::MainMenu);
        }
        return {};
    }

    case BootStage::MainMenu: {
        const std::string cmd = m_menu.Update(in);
        if (!cmd.empty())
            return cmd;
        return {};
    }

    case BootStage::InGame:
        return {};
    }
    return {};
}

} // namespace Game
} // namespace ClimaxEngine
