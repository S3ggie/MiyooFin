#include "BitmapFont.hpp"
#include "../../third_party/font8x8_basic_public_domain.h"
#include <array>
#include <cstring>
#include <cstdio>

namespace miyoofin {

// MiyooFin's renderer consumes each row MSB-first. The public-domain source
// font is LSB-first, so reverse each row once while constructing the table.
static uint8_t reverseBits(uint8_t v)
{
    v = static_cast<uint8_t>((v >> 4) | (v << 4));
    v = static_cast<uint8_t>(((v & 0xCC) >> 2) | ((v & 0x33) << 2));
    v = static_cast<uint8_t>(((v & 0xAA) >> 1) | ((v & 0x55) << 1));
    return v;
}

using FontTable = std::array<std::array<uint8_t, 16>, 95>;

// Convert the pinned public-domain 8x8 ASCII font to the 8x16 layout expected
// by the existing MiyooFin renderer by doubling each source row vertically.
static const FontTable sFont = [] {
    FontTable result{};
    for (std::size_t glyph = 0; glyph < result.size(); ++glyph) {
        for (std::size_t row = 0; row < 8; ++row) {
            const uint8_t bits = reverseBits(kMiyooFinFont8x8Basic[glyph][row]);
            result[glyph][row * 2] = bits;
            result[glyph][row * 2 + 1] = bits;
        }
    }
    return result;
}();

/// Map a Unicode code point to an ASCII glyph for the bitmap font.
/// Returns 0 for characters that have no sensible ASCII equivalent.
static unsigned int mapCodePointImpl(unsigned int cp)
{
    if (cp >= 32 && cp <= 126) return cp;
    switch (cp) {
        case 0x00B2: return '2';
        case 0x00B3: return '3';
        case 0x00B9: return '1';
        case 0x00BD: return '/';
        case 0x00BC: return '/';
        case 0x00BE: return '/';
        case 0x00D7: return 'x';
        case 0x00F7: return '/';
        case 0x2013: return '-';
        case 0x2014: return '-';
        case 0x2018: return '\'';
        case 0x2019: return '\'';
        case 0x201C: return '"';
        case 0x201D: return '"';
        case 0x2026: return '.';
        case 0x2122: return 'T';
        case 0x00A9: return 'C';
        case 0x00AE: return 'R';

        case 0x00A1: return '!';
        case 0x00BF: return '?';
        case 0x00D1: return 'N';
        case 0x00F1: return 'n';
        case 0x00C0: return 'A';
        case 0x00C1: return 'A';
        case 0x00C2: return 'A';
        case 0x00C3: return 'A';
        case 0x00C4: return 'A';
        case 0x00C5: return 'A';
        case 0x00C7: return 'C';
        case 0x00C8: return 'E';
        case 0x00C9: return 'E';
        case 0x00CA: return 'E';
        case 0x00CB: return 'E';
        case 0x00CC: return 'I';
        case 0x00CD: return 'I';
        case 0x00CE: return 'I';
        case 0x00CF: return 'I';
        case 0x00D0: return 'D';
        case 0x00D2: return 'O';
        case 0x00D3: return 'O';
        case 0x00D4: return 'O';
        case 0x00D5: return 'O';
        case 0x00D6: return 'O';
        case 0x00D8: return 'O';
        case 0x00D9: return 'U';
        case 0x00DA: return 'U';
        case 0x00DB: return 'U';
        case 0x00DC: return 'U';
        case 0x00DD: return 'Y';
        case 0x00DE: return 'T';
        case 0x00E0: return 'a';
        case 0x00E1: return 'a';
        case 0x00E2: return 'a';
        case 0x00E3: return 'a';
        case 0x00E4: return 'a';
        case 0x00E5: return 'a';
        case 0x00E7: return 'c';
        case 0x00E8: return 'e';
        case 0x00E9: return 'e';
        case 0x00EA: return 'e';
        case 0x00EB: return 'e';
        case 0x00EC: return 'i';
        case 0x00ED: return 'i';
        case 0x00EE: return 'i';
        case 0x00EF: return 'i';
        case 0x00F0: return 'd';
        case 0x00F2: return 'o';
        case 0x00F3: return 'o';
        case 0x00F4: return 'o';
        case 0x00F5: return 'o';
        case 0x00F6: return 'o';
        case 0x00F8: return 'o';
        case 0x00F9: return 'u';
        case 0x00FA: return 'u';
        case 0x00FB: return 'u';
        case 0x00FC: return 'u';
        case 0x00FD: return 'y';
        case 0x00FE: return 't';
        case 0x00FF: return 'y';
        default: return 0;
    }
}

const uint8_t *BitmapFont::glyphData(unsigned char ch)
{
    if (ch < 32)
        return sFont[0].data();
    if (ch > 126)
        return sFont[94].data();
    return sFont[ch - 32].data();
}

unsigned int BitmapFont::mapCodePoint(unsigned int cp)
{
    return mapCodePointImpl(cp);
}

static int utf8SequenceBytes(const unsigned char *p,
                             const unsigned char *end)
{
    if (p >= end || *p == 0) return 0;

    unsigned char ch = *p;
    int expected = 1;
    if (ch < 0x80)                    expected = 1;
    else if ((ch & 0xE0) == 0xC0)     expected = 2;
    else if ((ch & 0xF0) == 0xE0)     expected = 3;
    else if ((ch & 0xF8) == 0xF0)     expected = 4;
    else return 1;

    int avail = static_cast<int>(end - p);
    if (avail < expected) return 1;

    for (int i = 1; i < expected; ++i)
        if ((p[i] & 0xC0) != 0x80) return 1;

    return expected;
}

std::string BitmapFont::truncateUtf8(const std::string &text, int maxGlyphs)
{
    if (maxGlyphs <= 0) return {};

    const auto *bytes =
        reinterpret_cast<const unsigned char *>(text.c_str());
    const unsigned char *end = bytes + text.size();

    int glyphs = 0;
    {
        const unsigned char *q = bytes;
        while (q < end && *q) {
            int len = utf8SequenceBytes(q, end);
            if (len == 0) break;
            q += len;
            ++glyphs;
        }
    }

    if (glyphs <= maxGlyphs)
        return text;

    const char *suffix = (maxGlyphs >= 2) ? ".." : ".";
    int suffixCols     = (maxGlyphs >= 2) ? 2   : 1;
    int keep = maxGlyphs - suffixCols;
    if (keep < 0) keep = 0;

    const unsigned char *p = bytes;
    const unsigned char *cut = p;
    int count = 0;
    while (p < end && *p && count < keep) {
        int len = utf8SequenceBytes(p, end);
        if (len == 0) break;
        p += len;
        cut = p;
        ++count;
    }

    return std::string(reinterpret_cast<const char *>(bytes),
                       cut - bytes)
         + suffix;
}

void BitmapFont::drawChar(SDL_Surface *surface, int x, int y,
                          unsigned char ch,
                          Uint8 fgR, Uint8 fgG, Uint8 fgB,
                          Uint8 bgR, Uint8 bgG, Uint8 bgB)
{
    if (!surface) return;

    const uint8_t *glyph = glyphData(ch);
    Uint32 fgCol = SDL_MapRGBA(surface->format, fgR, fgG, fgB, 255);
    Uint32 bgCol = SDL_MapRGBA(surface->format, bgR, bgG, bgB, 255);

    int maxY = (y + GLYPH_H > surface->h) ? surface->h - y : GLYPH_H;
    if (maxY <= 0) return;
    int maxX = (x + GLYPH_W > surface->w) ? surface->w - x : GLYPH_W;
    if (maxX <= 0) return;

    for (int row = 0; row < maxY; ++row) {
        uint8_t bits = glyph[row];
        Uint32 *pixel = (Uint32 *)((Uint8 *)surface->pixels
                                   + (y + row) * surface->pitch
                                   + x * sizeof(Uint32));
        for (int col = 0; col < maxX; ++col) {
            pixel[col] = (bits & 0x80) ? fgCol : bgCol;
            bits <<= 1;
        }
    }
}

void BitmapFont::drawString(SDL_Surface *surface, int x, int y,
                            const char *text,
                            Uint8 fgR, Uint8 fgG, Uint8 fgB,
                            Uint8 bgR, Uint8 bgG, Uint8 bgB,
                            int wrapCols)
{
    if (!text) return;

    int curX = x;
    int curY = y;
    int col = 0;

    while (*text) {
        unsigned char ch = (unsigned char)*text;

        if (ch == '\n') {
            curY += GLYPH_H + 2;
            curX = x;
            col = 0;
            ++text;
            continue;
        }

        unsigned int cp = 0;
        int extraBytes = 0;
        if (ch < 0x80) {
            cp = ch;
        } else if ((ch & 0xE0) == 0xC0) {
            cp = ch & 0x1F; extraBytes = 1;
        } else if ((ch & 0xF0) == 0xE0) {
            cp = ch & 0x0F; extraBytes = 2;
        } else if ((ch & 0xF8) == 0xF0) {
            cp = ch & 0x07; extraBytes = 3;
        } else {
            cp = 0;
        }
        for (int i = 0; i < extraBytes; ++i) {
            ++text;
            unsigned char nb = (unsigned char)*text;
            if ((nb & 0xC0) == 0x80)
                cp = (cp << 6) | (nb & 0x3F);
            else {
                cp = 0;
                break;
            }
        }
        ++text;

        unsigned int mapped = mapCodePointImpl(cp);
        unsigned char glyph;
        if (mapped >= 32 && mapped <= 126)
            glyph = (unsigned char)mapped;
        else
            glyph = '?';

        if (wrapCols > 0 && col >= wrapCols) {
            curY += GLYPH_H + 2;
            curX = x;
            col = 0;
        }

        drawChar(surface, curX, curY, glyph, fgR, fgG, fgB, bgR, bgG, bgB);
        curX += GLYPH_W;
        ++col;
    }
}

void BitmapFont::drawStringScaled(SDL_Surface *surface, int x, int y,
                                  const char *text, int scale,
                                  Uint8 fgR, Uint8 fgG, Uint8 fgB,
                                  Uint8 bgR, Uint8 bgG, Uint8 bgB)
{
    if (!surface || !text || scale <= 0) return;
    int cursorX = x;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(text);
         *p; ++p) {
        const uint8_t *glyph = glyphData(*p);
        for (int row = 0; row < GLYPH_H; ++row) {
            for (int col = 0; col < GLYPH_W; ++col) {
                const bool set = (glyph[row] & (0x80 >> col)) != 0;
                fillRect(surface, cursorX + col * scale, y + row * scale,
                         scale, scale,
                         set ? fgR : bgR, set ? fgG : bgG, set ? fgB : bgB,
                         255);
            }
        }
        cursorX += GLYPH_W * scale;
    }
}

void BitmapFont::drawRect(SDL_Surface *surface,
                          int x, int y, int w, int h,
                          Uint8 r, Uint8 g, Uint8 b)
{
    if (!surface || w <= 0 || h <= 0) return;
    Uint32 col = SDL_MapRGBA(surface->format, r, g, b, 255);

    int x2 = x + w - 1;
    int y2 = y + h - 1;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x2 >= surface->w) x2 = surface->w - 1;
    if (y2 >= surface->h) y2 = surface->h - 1;
    if (x > x2 || y > y2) return;

    for (int px = x; px <= x2; ++px) {
        ((Uint32 *)((Uint8 *)surface->pixels + y * surface->pitch))[px] = col;
        ((Uint32 *)((Uint8 *)surface->pixels + y2 * surface->pitch))[px] = col;
    }
    for (int py = y + 1; py < y2; ++py) {
        ((Uint32 *)((Uint8 *)surface->pixels + py * surface->pitch))[x] = col;
        ((Uint32 *)((Uint8 *)surface->pixels + py * surface->pitch))[x2] = col;
    }
}

void BitmapFont::fillRect(SDL_Surface *surface,
                          int x, int y, int w, int h,
                          Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
    if (!surface) return;
    Uint32 col = SDL_MapRGBA(surface->format, r, g, b, a);

    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > surface->w) w = surface->w - x;
    if (y + h > surface->h) h = surface->h - y;
    if (w <= 0 || h <= 0) return;

    for (int py = y; py < y + h; ++py) {
        Uint32 *pixel = (Uint32 *)((Uint8 *)surface->pixels + py * surface->pitch);
        for (int px = x; px < x + w; ++px) {
            pixel[px] = col;
        }
    }
}

} // namespace miyoofin
