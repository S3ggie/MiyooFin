#ifndef MIYOOFIN_DESIGN_HPP
#define MIYOOFIN_DESIGN_HPP

#include <cstdint>

namespace miyoofin {
namespace design {

// The single source of truth for MiyooFin's look. It is the Home screen's
// palette (near-black navy canvas, electric-blue accent, cool-grey text)
// extracted so every screen shares it. All colours are 8-bit sRGB.
struct Rgb
{
    std::uint8_t r, g, b;
};

// Surfaces, darkest to lightest. Depth comes from stepping up one surface at a
// time (canvas -> panel -> raised), never from arbitrary greys.
inline Rgb kCanvas{7, 9, 18};    // page background
inline Rgb kHeaderBg{4, 6, 14};  // header strip
inline Rgb kFooterBg{4, 5, 10};  // footer strip
inline Rgb kPanel{13, 17, 31};   // cards, list rows, input fields
inline Rgb kRaised{21, 28, 48};  // focused/selected row, key caps
inline Rgb kBorder{38, 46, 68};  // hairlines and unfocused outlines
inline Rgb kDivider{20, 26, 44}; // quiet separators

// Accent (focus, selection, primary actions).
inline Rgb kAccent{60, 150, 255};
inline Rgb kAccentHi{150, 205, 255}; // top highlight of a focused edge
inline Rgb kAccentDim{24, 70, 150};  // glow, pressed, accent fills
inline Rgb kAccentSoft{16, 36, 78};  // tinted background of an active tab/row

// Text, brightest to quietest.
inline Rgb kText{238, 244, 255};
inline Rgb kTextSecondary{175, 180, 195};
inline Rgb kTextMuted{118, 126, 148};

// Status colours (always paired with a label or icon, never colour alone).
constexpr Rgb kSuccess{60, 200, 110};
constexpr Rgb kWarning{235, 190, 70};
constexpr Rgb kDanger{225, 80, 80};

// The palette is runtime-selectable: MiyooFin (blue) and MiyooFin Music (black with a
// purple accent and magenta highlight). Switch only on the UI thread, before drawing.
inline void usePalette(bool music)
{
    if (music) {
        kCanvas = {0, 0, 0};
        kHeaderBg = {5, 3, 9};
        kFooterBg = {5, 3, 9};
        kPanel = {14, 10, 22};
        kRaised = {34, 20, 52};
        kBorder = {56, 40, 78};
        kDivider = {26, 18, 38};
        kAccent = {150, 80, 255};
        kAccentHi = {240, 70, 220};
        kAccentDim = {84, 40, 150};
        kAccentSoft = {40, 18, 70};
        kText = {255, 255, 255};
        kTextSecondary = {200, 194, 214};
        kTextMuted = {136, 126, 156};
    } else {
        kCanvas = {7, 9, 18};
        kHeaderBg = {4, 6, 14};
        kFooterBg = {4, 5, 10};
        kPanel = {13, 17, 31};
        kRaised = {21, 28, 48};
        kBorder = {38, 46, 68};
        kDivider = {20, 26, 44};
        kAccent = {60, 150, 255};
        kAccentHi = {150, 205, 255};
        kAccentDim = {24, 70, 150};
        kAccentSoft = {16, 36, 78};
        kText = {238, 244, 255};
        kTextSecondary = {175, 180, 195};
        kTextMuted = {118, 126, 148};
    }
}

// Geometry on the 640x480 framebuffer. Everything is a multiple of 4.
constexpr int kScreenW = 640;
constexpr int kScreenH = 480;
constexpr int kHeaderH = 44;
constexpr int kFooterH = 24;
constexpr int kMargin = 16; // outer horizontal margin
constexpr int kGap = 8;     // default gap between sibling elements
constexpr int kRadius = 4;  // corner radius of cards, buttons, fields
// Header tab pills: shared by drawing and pointer hit-testing so they cannot
// drift apart. Pills are left-aligned after the wordmark.
constexpr int kTabsLeft = 164;
constexpr int kTabPad = 8;  // horizontal padding inside a pill
constexpr int kTabGap = 8;  // space between pills
constexpr int kTabTop = 10; // pill top and height
constexpr int kTabHeight = 24;
constexpr int kContentTop = kHeaderH + 8;
constexpr int kContentBottom = kScreenH - kFooterH - 8;

} // namespace design
} // namespace miyoofin

#endif // MIYOOFIN_DESIGN_HPP
