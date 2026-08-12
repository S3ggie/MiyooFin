#include "OnScreenKeyboard.hpp"
#include "Theme.hpp"
#include "BitmapFont.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace miyoofin {

OnScreenKeyboard::OnScreenKeyboard()
    : m_config()
{
    buildKeyboard();
}

OnScreenKeyboard::OnScreenKeyboard(const Config &config)
    : m_config(config)
{
    buildKeyboard();
}

void OnScreenKeyboard::reset()
{
    m_caps = false;
    m_activeKeyRow = 0;
    m_activeKeyCol = 0;
}

bool OnScreenKeyboard::capsEnabled() const { return m_caps; }
int  OnScreenKeyboard::keyboardTop() const { return m_config.keyboardTop; }
int  OnScreenKeyboard::keyboardBottom() const
{
    return m_config.keyboardTop + NUM_ROWS * (KEY_H + KEY_GAP) - KEY_GAP;
}

// ===================================================================
// Layout — fixed 10-column rectangular grid
// ===================================================================

void OnScreenKeyboard::buildKeyboard()
{
    m_keys.clear();
    struct KeyDef { const char *label; char ch; int row; int col; int span; };
    const KeyDef defs[] = {
        // Row 0: digits (10 × 1-col)
        {"1",'1',0,0,1},{"2",'2',0,1,1},{"3",'3',0,2,1},{"4",'4',0,3,1},{"5",'5',0,4,1},
        {"6",'6',0,5,1},{"7",'7',0,6,1},{"8",'8',0,7,1},{"9",'9',0,8,1},{"0",'0',0,9,1},
        // Row 1: qwertyuiop
        {"q",'q',1,0,1},{"w",'w',1,1,1},{"e",'e',1,2,1},{"r",'r',1,3,1},{"t",'t',1,4,1},
        {"y",'y',1,5,1},{"u",'u',1,6,1},{"i",'i',1,7,1},{"o",'o',1,8,1},{"p",'p',1,9,1},
        // Row 2: asdfghjkl@
        {"a",'a',2,0,1},{"s",'s',2,1,1},{"d",'d',2,2,1},{"f",'f',2,3,1},{"g",'g',2,4,1},
        {"h",'h',2,5,1},{"j",'j',2,6,1},{"k",'k',2,7,1},{"l",'l',2,8,1},{"@",'@',2,9,1},
        // Row 3: zxcvbnm.:/
        {"z",'z',3,0,1},{"x",'x',3,1,1},{"c",'c',3,2,1},{"v",'v',3,3,1},{"b",'b',3,4,1},
        {"n",'n',3,5,1},{"m",'m',3,6,1},{".",'.',3,7,1},{":",':',3,8,1},{"/",'/',3,9,1},
        // Row 4: -_!?#&+='
        {"-",'-',4,0,1},{"_",'_',4,1,1},{"!",'!',4,2,1},{"~",'~',4,3,1},{"?",'?',4,4,1},
        {"#",'#',4,5,1},{"&",'&',4,6,1},{"+",'+',4,7,1},{"=",'=',4,8,1},{"'",'\'',4,9,1},
        // Row 5: five 2-col action keys
        {"SPACE",' ', 5,0,2},
        {"DEL", KEY_DEL,    5,2,2},
        {"CLR", KEY_CLR,     5,4,2},
        {m_config.submitLabel, KEY_SUBMIT, 5,6,2},
        {"CANCEL", KEY_CANCEL, 5,8,2},
    };

    for (const auto &d : defs) {
        Key k;
        k.ch     = d.ch;
        k.label  = d.label;
        k.row    = d.row;
        k.col    = d.col;
        k.colSpan = d.span;
        m_keys.push_back(k);
    }
}

int OnScreenKeyboard::keyIndex(int row, int col) const
{
    for (int i = 0; i < (int)m_keys.size(); ++i) {
        if (m_keys[i].row == row && m_keys[i].col == col)
            return i;
    }
    return -1;
}

const OnScreenKeyboard::Key *OnScreenKeyboard::activeKey() const
{
    int idx = keyIndex(m_activeKeyRow, m_activeKeyCol);
    if (idx < 0) return nullptr;
    return &m_keys[idx];
}

bool OnScreenKeyboard::selectionValid() const { return activeKey() != nullptr; }
int  OnScreenKeyboard::selectionRow() const   { return m_activeKeyRow; }

std::string OnScreenKeyboard::keyLabel(const Key &key) const
{
    std::string label(key.label);
    if (m_caps && label.size() == 1 && std::isalpha(
            static_cast<unsigned char>(label[0]))) {
        label[0] = static_cast<char>(std::toupper(
            static_cast<unsigned char>(label[0])));
    }
    return label;
}

char OnScreenKeyboard::processKey(const Key &key) const
{
    char c = key.ch;
    if (m_caps && std::isalpha(static_cast<unsigned char>(c)))
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return c;
}

// ===================================================================
// D-pad navigation
// ===================================================================

bool OnScreenKeyboard::navigateUp()
{
    if (m_activeKeyRow <= 0) return false;
    int targetRow = m_activeKeyRow - 1;
    const Key *cur = activeKey();
    if (!cur) return false;
    float curCentre = cur->col + cur->colSpan / 2.0f;
    int bestCol = -1;
    float bestDist = 999.0f;
    for (const auto &k : m_keys) {
        if (k.row != targetRow) continue;
        float kCentre = k.col + k.colSpan / 2.0f;
        float dist = std::abs(kCentre - curCentre);
        if (dist < bestDist || (dist == bestDist && k.col < bestCol)) {
            bestDist = dist;
            bestCol  = k.col;
        }
    }
    if (bestCol < 0) return false;
    m_activeKeyRow = targetRow;
    m_activeKeyCol = bestCol;
    return true;
}

bool OnScreenKeyboard::navigateDown()
{
    if (m_activeKeyRow >= NUM_ROWS - 1) return false;
    int targetRow = m_activeKeyRow + 1;
    const Key *cur = activeKey();
    if (!cur) return false;
    float curCentre = cur->col + cur->colSpan / 2.0f;
    int bestCol = -1;
    float bestDist = 999.0f;
    for (const auto &k : m_keys) {
        if (k.row != targetRow) continue;
        float kCentre = k.col + k.colSpan / 2.0f;
        float dist = std::abs(kCentre - curCentre);
        if (dist < bestDist || (dist == bestDist && k.col < bestCol)) {
            bestDist = dist;
            bestCol  = k.col;
        }
    }
    if (bestCol < 0) return false;
    m_activeKeyRow = targetRow;
    m_activeKeyCol = bestCol;
    return true;
}

bool OnScreenKeyboard::navigateLeft()
{
    if (m_activeKeyCol <= 0) return false;
    int prevCol = -1;
    for (const auto &k : m_keys) {
        if (k.row == m_activeKeyRow && k.col < m_activeKeyCol) {
            if (prevCol < 0 || k.col > prevCol)
                prevCol = k.col;
        }
    }
    if (prevCol < 0) return false;
    m_activeKeyCol = prevCol;
    return true;
}

bool OnScreenKeyboard::navigateRight()
{
    const Key *cur = activeKey();
    if (!cur) return false;
    int curEnd = cur->col + cur->colSpan;
    int nextCol = -1;
    for (const auto &k : m_keys) {
        if (k.row == m_activeKeyRow && k.col >= curEnd) {
            if (nextCol < 0 || k.col < nextCol)
                nextCol = k.col;
        }
    }
    if (nextCol < 0) return false;
    m_activeKeyCol = nextCol;
    return true;
}

int OnScreenKeyboard::handleAction(Action action)
{
    switch (action) {
    case Action::PrevPage:
        m_caps = !m_caps;
        return 0;
    case Action::Up:    return navigateUp()    ? 0 : 0;
    case Action::Down:  return navigateDown()  ? 0 : 0;
    case Action::Left:  return navigateLeft()  ? 0 : 0;
    case Action::Right: return navigateRight() ? 0 : 0;
    case Action::Confirm: {
        const Key *k = activeKey();
        if (k) return static_cast<int>(processKey(*k));
        return 0;
    }
    case Action::Back:    return static_cast<int>(KEY_DEL);
    case Action::Search:  return static_cast<int>(KEY_CLR);
    case Action::Settings: return static_cast<int>(KEY_SUBMIT);
    case Action::Menu:    return static_cast<int>(KEY_CANCEL);
    default: return -1;
    }
}

// ===================================================================
// Rendering — rectangular 10-column grid
// ===================================================================

void OnScreenKeyboard::render(SDL_Surface *fb) const
{
    for (const auto &k : m_keys) {
        int x = KEYBOARD_X + k.col * (KEY_W + KEY_GAP);
        int y = m_config.keyboardTop + k.row * (KEY_H + KEY_GAP);
        int w = k.colSpan * KEY_W + (k.colSpan - 1) * KEY_GAP;

        bool sel = (k.row == m_activeKeyRow && k.col == m_activeKeyCol);
        Uint8 bgR = sel ? Theme::ACCENT_R : 40;
        Uint8 bgG = sel ? Theme::ACCENT_G : 40;
        Uint8 bgB = sel ? Theme::ACCENT_B : 50;
        Uint8 fgR = sel ? 255 : Theme::TEXT_R;
        Uint8 fgG = sel ? 255 : Theme::TEXT_G;
        Uint8 fgB = sel ? 255 : Theme::TEXT_B;

        std::string label = keyLabel(k);

        BitmapFont::fillRect(fb, x, y, w, KEY_H, bgR, bgG, bgB, 255);
        BitmapFont::drawRect(fb, x, y, w, KEY_H, fgR, fgG, fgB);

        int labelW = (int)label.size() * BitmapFont::GLYPH_W * KEY_LABEL_SCALE;
        int lx = x + (w - labelW) / 2;
        int ly = y + (KEY_H - BitmapFont::GLYPH_H * KEY_LABEL_SCALE) / 2;
        BitmapFont::drawStringScaled(fb, lx, ly, label.c_str(), KEY_LABEL_SCALE,
                                     fgR, fgG, fgB, bgR, bgG, bgB);
    }

    BitmapFont::drawString(fb, 8, m_config.keyboardTop - 16,
        m_caps ? "CAPS ON" : "CAPS OFF",
        m_caps ? Theme::ACCENT_R : Theme::TEXT_R,
        m_caps ? Theme::ACCENT_G : Theme::TEXT_G,
        m_caps ? Theme::ACCENT_B : Theme::TEXT_B,
        Theme::BG_R, Theme::BG_G, Theme::BG_B);
}

} // namespace miyoofin
