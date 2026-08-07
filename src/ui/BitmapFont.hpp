#ifndef MIYOOFIN_BITMAP_FONT_HPP
#define MIYOOFIN_BITMAP_FONT_HPP

#include <SDL2/SDL.h>
#include <cstdint>

namespace miyoofin {

/// 8×16 monospace bitmap font for rendering text directly onto
/// the software framebuffer.  Covers printable ASCII (32–126).
/// Each glyph is 16 bytes, 1 bit per pixel, MSB = leftmost pixel.
class BitmapFont {
public:
    /// Draw a single character at (x, y) on the surface.
    static void drawChar(SDL_Surface *surface, int x, int y,
                         unsigned char ch,
                         Uint8 fgR, Uint8 fgG, Uint8 fgB,
                         Uint8 bgR, Uint8 bgG, Uint8 bgB);

    /// Draw a null-terminated string.
    /// @param x  left edge in pixels
    /// @param y  top edge in pixels
    /// @param wrapCols  if > 0, wrap at this column (character count)
    static void drawString(SDL_Surface *surface, int x, int y,
                           const char *text,
                           Uint8 fgR, Uint8 fgG, Uint8 fgB,
                           Uint8 bgR, Uint8 bgG, Uint8 bgB,
                           int wrapCols = 0);

    /// Glyph dimensions
    static constexpr int GLYPH_W = 8;
    static constexpr int GLYPH_H = 16;

    /// Draw a 1-pixel border (rect) with the given colour.
    static void drawRect(SDL_Surface *surface,
                         int x, int y, int w, int h,
                         Uint8 r, Uint8 g, Uint8 b);

    /// Fill a rectangle on the surface.
    static void fillRect(SDL_Surface *surface,
                         int x, int y, int w, int h,
                         Uint8 r, Uint8 g, Uint8 b, Uint8 a = 255);

private:
    static const uint8_t *glyphData(unsigned char ch);
};

} // namespace miyoofin

#endif // MIYOOFIN_BITMAP_FONT_HPP