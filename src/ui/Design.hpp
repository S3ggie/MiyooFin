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
constexpr Rgb kCanvas{7, 9, 18};    // page background
constexpr Rgb kHeaderBg{4, 6, 14};  // header strip
constexpr Rgb kFooterBg{4, 5, 10};  // footer strip
constexpr Rgb kPanel{13, 17, 31};   // cards, list rows, input fields
constexpr Rgb kRaised{21, 28, 48};  // focused/selected row, key caps
constexpr Rgb kBorder{38, 46, 68};  // hairlines and unfocused outlines
constexpr Rgb kDivider{20, 26, 44}; // quiet separators

// Accent (focus, selection, primary actions).
constexpr Rgb kAccent{60, 150, 255};
constexpr Rgb kAccentHi{150, 205, 255}; // top highlight of a focused edge
constexpr Rgb kAccentDim{24, 70, 150};  // glow, pressed, accent fills
constexpr Rgb kAccentSoft{16, 36, 78};  // tinted background of an active tab/row

// Text, brightest to quietest.
constexpr Rgb kText{238, 244, 255};
constexpr Rgb kTextSecondary{175, 180, 195};
constexpr Rgb kTextMuted{118, 126, 148};

// Status colours (always paired with a label or icon, never colour alone).
constexpr Rgb kSuccess{60, 200, 110};
constexpr Rgb kWarning{235, 190, 70};
constexpr Rgb kDanger{225, 80, 80};

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
