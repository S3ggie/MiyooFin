#ifndef MIYOOFIN_THEME_HPP
#define MIYOOFIN_THEME_HPP

#include <SDL2/SDL.h>

namespace miyoofin {

/// Visual constants used throughout the UI.
struct Theme {
    // Background colour (dark)
    static constexpr Uint8 BG_R = 18;
    static constexpr Uint8 BG_G = 18;
    static constexpr Uint8 BG_B = 24;
    static constexpr Uint8 BG_A = 255;

    // Text colours (light on dark)
    static constexpr Uint8 TEXT_R       = 220;
    static constexpr Uint8 TEXT_G       = 220;
    static constexpr Uint8 TEXT_B       = 220;

    // Accent / title colour (Jellyfin-inspired purple-teal)
    static constexpr Uint8 ACCENT_R     = 170;  // 0xAA
    static constexpr Uint8 ACCENT_G     = 0xB0; // teal-ish
    static constexpr Uint8 ACCENT_B     = 0xBE; //

    // Diagnostic highlight
    static constexpr Uint8 HIGHLIGHT_R  = 255;
    static constexpr Uint8 HIGHLIGHT_G  = 0;
    static constexpr Uint8 HIGHLIGHT_B  = 80;  // pink-red
};

/// Helper to pack RGBA into a uint32_t for direct surface pixel writes.
inline Uint32 rgba(Uint8 r, Uint8 g, Uint8 b, Uint8 a, const SDL_PixelFormat *fmt)
{
    return SDL_MapRGBA(fmt, r, g, b, a);
}

} // namespace miyoofin

#endif // MIYOOFIN_THEME_HPP